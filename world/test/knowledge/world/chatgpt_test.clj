(ns knowledge.world.chatgpt-test
  "Sign in with ChatGPT against a stub of OpenAI's documented endpoints, and the Responses route. No
   real account, no network."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.model-api :as model-api]
            [knowledge.world.adapters.responses :as responses]
            [knowledge.world.chatgpt :as chatgpt]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.models :as models]
            [knowledge.world.ws-client :as ws])
  (:import [com.sun.net.httpserver HttpExchange HttpHandler HttpServer]
           [java.net InetSocketAddress URI URLDecoder]
           [java.nio.file Files LinkOption]
           [java.nio.file.attribute PosixFilePermission]
           [java.security KeyPairGenerator Signature]
           [java.util Base64]))

(defn- b64 [^bytes bs] (.encodeToString (.withoutPadding (Base64/getUrlEncoder)) bs))
(defn- json [v] (j/write-value-as-string v))
(defn- unsigned ^bytes [^java.math.BigInteger n]
  (let [bs (.toByteArray n)] (if (zero? (aget bs 0)) (java.util.Arrays/copyOfRange bs 1 (alength bs)) bs)))

(def ^:private keypair (delay (.generateKeyPair (doto (KeyPairGenerator/getInstance "RSA") (.initialize 2048)))))

(defn- id-token [claims]
  (let [head (b64 (.getBytes (json {:alg "RS256" :kid "k1"}) "UTF-8"))
        body (b64 (.getBytes (json claims) "UTF-8"))
        s (doto (Signature/getInstance "SHA256withRSA") (.initSign (.getPrivate @keypair))
                (.update (.getBytes (str head "." body) "US-ASCII")))]
    (str head "." body "." (b64 (.sign s)))))

(defn- query [^String q]
  (into {} (for [kv (str/split (or q "") #"&") :let [[k v] (str/split kv #"=" 2)] :when k]
             [k (URLDecoder/decode (str v) "UTF-8")])))

(defn- respond! [^HttpExchange x status ^String body]
  (let [bs (.getBytes body "UTF-8")]
    (.sendResponseHeaders x status (alength bs))
    (with-open [o (.getResponseBody x)] (.write o bs))))

(defn- stub!
  "OpenAI as its docs describe it. calls collects what reached it; responses is what the token and
   model endpoints do next."
  [state]
  (let [server (HttpServer/create (InetSocketAddress. "127.0.0.1" 0) 0)
        port (.getPort (.getAddress server))
        base (str "http://127.0.0.1:" port)
        handle (fn [path f] (.createContext server path (reify HttpHandler (handle [_ x] (f x)))))]
    (handle "/.well-known/openid-configuration"
            (fn [x] (respond! x 200 (json {:jwks_uri (str base "/jwks") :revocation_endpoint (str base "/revoke")}))))
    (handle "/jwks"
            (fn [x] (let [k (.getPublic @keypair)]
                      (respond! x 200 (json {:keys [{:kid "k1" :kty "RSA" :n (b64 (unsigned (.getModulus k)))
                                                     :e (b64 (unsigned (.getPublicExponent k)))}]})))))
    (handle "/revoke" (fn [x] (swap! state update :revoked (fnil inc 0)) (respond! x 200 "")))
    (handle "/token"
            (fn [x] (let [f (query (slurp (.getRequestBody x)))]
                      (swap! state update :token-calls (fnil conj []) f)
                      (if (= "refresh_token" (get f "grant_type"))
                        (if (:refresh-fails @state)
                          (respond! x 400 (json {:error (:refresh-fails @state)}))
                          (respond! x 200 (json {:access_token "at-2" :refresh_token "rt-2" :expires_in 3600})))
                        (respond! x 200 (json {:access_token "at-1" :refresh_token "rt-1" :token_type "Bearer" :expires_in 3600
                                               :scope (:scope @state)
                                               :id_token (id-token {:iss base :aud "oaiapp_test" :sub "user-1" :email "a@example.com"
                                                                    :exp (+ 3600 (quot (System/currentTimeMillis) 1000))
                                                                    :nonce (:nonce @state)})}))))))
    (handle "/v1/models"
            (fn [x] (swap! state update :bearers (fnil conj []) (.getFirst (.getRequestHeaders x) "authorization"))
              (respond! x 200 (json {:models [{:slug "gpt-a" :display_name "GPT A" :visibility "list"}
                                              {:slug "hidden" :display_name "Hidden" :visibility "hide"}]}))))
    (.start server)
    {:server server :base base}))

(defmacro ^:private with-stub [[state base] & body]
  `(let [~state (atom {:scope "chatgpt.tokens.use.direct email offline_access openid profile resource.invoke"})
         stub# (stub! ~state)
         ~base (:base stub#)
         old# @chatgpt/endpoints]
     (reset! chatgpt/endpoints {:issuer ~base :authorize (str ~base "/authorize") :token (str ~base "/token")
                                :discovery (str ~base "/.well-known/openid-configuration")})
     (try ~@body
          (finally (reset! chatgpt/endpoints old#) (.stop ^HttpServer (:server stub#) 0)))))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- result [r] (let [x (:result r)] (if (and (map? x) (contains? x :result)) (:result x) x)))
(defn- url-params [url] (query (.getRawQuery (URI. url))))
(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))

(deftest the-first-sign-in-registers-a-client-and-keeps-tokens-off-the-log
  (with-stub [state base]
    (fx/with-world [{:keys [world root witness]}]
      (let [w (fx/attach-as world (:token witness))]
        (fx/grant! root (:id witness) :everything #{:read :write :create :evaluate})
        (with-redefs [models/probe-url (fn [_ key] {:status :ok :http 200 :models ["gpt-a"]
                                                    :catalogue [{:slug "gpt-a" :name "GPT A"}] :key-seen key})]
          (let [begin (result (act! w "model.signin.begin" {:port 1455}))
                p (url-params (:url begin))]
            (testing "first sign-in says what the docs say"
              (is (= chatgpt/dynamic-client (get p "client_id")))
              (is (= "ORBIS" (get p "agent_name_hint")))
              (is (str/starts-with? (get p "ext_agent_host_id") "urn:uuid:"))
              (is (= "http://127.0.0.1:1455/auth/callback" (get p "redirect_uri")))
              (is (= "S256" (get p "code_challenge_method")))
              (is (= "https://api.openai.com/v1" (get p "resource")))
              (is (str/includes? (get p "scope") "chatgpt.tokens.use.direct")))
            (testing "a wrong state exchanges nothing"
              (is (false? (:ok (act! w "model.signin.finish" {:attempt (:attempt begin) :state "wrong" :code "c"}))))
              (is (empty? (:token-calls @state))))
            (let [begin (result (act! w "model.signin.begin" {:port 1455}))
                  p (url-params (:url begin))
                  _ (swap! state assoc :nonce (get p "nonce"))
                  r (act! w "model.signin.finish" {:attempt (:attempt begin) :state (get p "state") :code "the-code"
                                                   :client-id "oaiapp_test"})
                  out (result r)
                  model (:model out)
                  model (if (string? model) (parse-uuid model) model)
                  call (first (:token-calls @state))]
              (is (:ok r) (pr-str r))
              (is (= "oaiapp_test" (get call "client_id")) "the exchange uses the issued client, not the dynamic one")
              (is (seq (get call "code_verifier")))
              (is (= :chatgpt-plan (field world model :model/api)))
              (is (= "gpt-a" (field world model :model/id)) "the Model takes a real id from the catalogue")
              (testing "tokens are in a private file, and nowhere on the log"
                (let [f (io/file (:data-dir world) "credentials" "chatgpt" (str model ".json"))
                      perms (Files/getPosixFilePermissions (.toPath f) (make-array LinkOption 0))
                      log-bytes (apply str (for [g (file-seq (io/file (:data-dir world) "log")) :when (.isFile g)]
                                             (String. (Files/readAllBytes (.toPath g)) "ISO-8859-1")))]
                  (is (= #{PosixFilePermission/OWNER_READ PosixFilePermission/OWNER_WRITE} perms))
                  (is (str/includes? (slurp f) "rt-1"))
                  (doseq [secret ["rt-1" "at-1" "the-code"]]
                    (is (not (str/includes? log-bytes secret)) secret))
                  (is (not (str/includes? (pr-str (objects/fields (act/db world) model)) "at-1")))))
              (testing "a token outside the plan scope is not kept"
                (swap! state assoc :scope "openid email profile")
                (let [b (result (act! w "model.signin.begin" {:port 1455}))
                      q (url-params (:url b))]
                  (swap! state assoc :nonce (get q "nonce"))
                  (is (= :plan-not-granted
                         (some-> (act! w "model.signin.finish" {:attempt (:attempt b) :state (get q "state") :code "c2"
                                                                :client-id "oaiapp_test"})
                                 :err :code keyword)))))
              (testing "the world's own loop cannot sign in"
                (is (= :person-only (some-> (act! root "model.signin.begin" {}) :err :code keyword)))))))))))

(deftest refreshing-rotates-and-a-dead-token-signs-out-only-that-model
  (with-stub [state base]
    (let [dir (str (fx/temp-dir)) model (random-uuid)
          write! (fn [m] (#'chatgpt/write-record! dir model m))]
      (write! {:client_id "oaiapp_test" :access_token "old" :refresh_token "rt-1" :expires_in 10
               :saved_at "2020-01-01T00:00:00Z" :subject "s"})
      (is (= "at-2" (chatgpt/access-token dir model)))
      (is (= "rt-2" (:refresh_token (chatgpt/read-record dir model))))
      (is (= "oaiapp_test" (get (last (:token-calls @state)) "client_id")))
      (write! (assoc (chatgpt/read-record dir model) :saved_at "2020-01-01T00:00:00Z"))
      (swap! state assoc :refresh-fails "refresh_token_expired")
      (is (nil? (chatgpt/access-token dir model)))
      (let [left (chatgpt/read-record dir model)]
        (is (nil? (:refresh_token left)))
        (is (= "oaiapp_test" (:client_id left)) "the issued client stays for the next sign-in"))
      (testing "a temporary failure keeps the credentials"
        (write! {:client_id "c" :access_token "x" :refresh_token "r" :expires_in 1 :saved_at "2020-01-01T00:00:00Z"})
        (swap! state assoc :refresh-fails "server_error")
        (is (thrown? Exception (chatgpt/access-token dir model)))
        (is (= "r" (:refresh_token (chatgpt/read-record dir model)))))
      (testing "sign-out revokes, then clears"
        (is (= {:revoked true} (chatgpt/sign-out! dir model)))
        (is (= 1 (:revoked @state)))
        (is (not (chatgpt/signed-in? dir model)))))))

(deftest a-listing-shows-what-is-meant-to-be-listed
  (is (= [{:slug "a" :name "A"} {:slug "b" :name "b"}]
         (chatgpt/catalogue {:models [{:slug "a" :display_name "A" :visibility "list"}
                                      {:slug "x" :visibility "hide"}
                                      {:slug "b" :visibility "list"}]}))))

(deftest the-responses-body-follows-the-preview-rules
  (let [messages [{:role "system" :content "be plain"}
                  {:role "user" :content "hi"}
                  {:role "assistant" :content "" :tool_calls [{:id "c1" :function {:name "read" :arguments "{}"}}]}
                  {:role "tool" :tool_call_id "c1" :content "ok"}]
        body (responses/request-body {:model "gpt-a" :tools? true} model-api/op-tools messages :off)]
    (is (= false (:store body)))
    (is (= true (:stream body)))
    (is (= "be plain" (:instructions body)))
    (is (= [{:role "user" :content "hi"}
            {:type "function_call" :call_id "c1" :name "read" :arguments "{}"}
            {:type "function_call_output" :call_id "c1" :output "ok"}]
           (:input body)))
    (is (= "namespace" (:type (first (:tools body)))))
    (is (every? #(= "function" (:type %)) (:tools (first (:tools body)))))
    (doseq [k [:max_output_tokens :temperature :metadata :top_p :user :previous_response_id :stream_options]]
      (is (not (contains? body k)) k))))

(deftest only-completed-is-success
  (let [line (fn [m] (str "data: " (json m)))
        said (atom "")
        ok (responses/consume-events
            [(line {:type "response.output_text.delta" :delta "he"})
             (line {:type "response.output_text.delta" :delta "llo"})
             (line {:type "response.output_item.done" :item {:type "function_call" :name "read" :arguments "{}"}})
             (line {:type "response.completed" :response {:model "gpt-a" :usage {:input_tokens 5 :output_tokens 2}}})]
            #(swap! said str %) nil)]
    (is (= :done (:state ok)))
    (is (= "hello" (:text ok) @said))
    (is (= 5 (get-in ok [:usage :prompt-tokens])))
    (is (= [{:name "read" :arguments "{}"}] (:tool-calls ok)))
    (testing "a limit after text began fails"
      (let [o (responses/consume-events
               [(line {:type "response.output_text.delta" :delta "he"})
                (line {:type "response.failed" :response {:error {:code "subscription_sharing_usage_limit_exceeded" :message "limit"}}})]
               nil nil)]
        (is (= :failed (:state o)))
        (is (= :limit (models/failure-status (:error o))))))
    (is (= :failed (:state (responses/consume-events [(line {:type "response.output_text.delta" :delta "x"})] nil nil)))
        "a stream that stops short is no answer")
    (testing "an admission error with no standard shape"
      (is (str/includes? (responses/error-of-body 503 "{\"detail\":\"not enabled\"}") "not enabled")))))
