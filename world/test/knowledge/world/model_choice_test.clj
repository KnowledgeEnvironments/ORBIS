(ns knowledge.world.model-choice-test
  "Which Model a wake runs on: a person's choice, per agent or per conversation, and a fallback."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.chatgpt :as chatgpt]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.log.core :as log]
            [knowledge.world.models :as models]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws])
  (:import [com.sun.net.httpserver HttpExchange HttpHandler HttpServer]
           [java.net InetSocketAddress]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))

(defn- model! [root nm]
  (get-in (ws/request!! root :act {:class :model :action :create
                                   :args {:kind :model :format :openai-chat :residency :local :name nm :model-id nm}})
          [:result :object]))

(defn- status! [world m s]
  (log/append!! (:log world) (log/pending :field-set m (:world-id world) {:name :model/status :type :keyword :value s :reader :both})))

(deftest a-person-chooses-and-an-agent-does-not
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          a (model! root "a") b (model! root "b") c (model! root "c")
          w (fx/attach-as world (:token witness))
          state #(objects/state (act/db world) root-id)]
      (fx/grant! root (:id witness) :everything #{:read :write :evaluate})
      (testing "an agent cannot pick its own Model"
        (let [[r] (lops/execute! world root-id nil [{:op :set-field :args {:object root-id :field :principal/model-choice
                                                                            :type :map :value {:model (str a)} :reader :both}}])]
          (is (false? (:ok r)) (pr-str r))
          (is (= "person-only" (some-> r :err :code name)))))
      (testing "a person's choice holds, and the agent can use it"
        (let [r (act! w "model.choose" {:principal (str root-id) :model (str a) :id "a-large"})] (is (:ok r) (pr-str r)))
        (is (= {:model a :model-id "a-large" :source :principal} (models/for-wake (act/db world) (state) nil)))
        (is (resolve/resolves? (act/db world) root-id (str "/" a) :evaluate)))
      (testing "a conversation's choice beats the agent's"
        (let [thread (random-uuid)]
          (log/append!! (:log world) (log/pending :created thread root-id {:class :object :fields {}}))
          (is (:ok (act! w "model.choose" {:principal (str root-id) :model (str b) :thread (str thread)})))
          (is (= b (:model (models/for-wake (act/db world) (state) thread))))
          (is (= a (:model (models/for-wake (act/db world) (state) nil))))))
      (testing "down gives way to the fallback, and says so"
        (is (:ok (act! w "model.choose" {:principal (str root-id) :model (str c) :fallback true})))
        (status! world a :unreachable)
        (is (= {:model c :fell-back-from a :source :fallback} (models/for-wake (act/db world) (state) nil)))
        (status! world c :limit)
        (is (= a (:model (models/for-wake (act/db world) (state) nil))) "both down: the choice stands, and fails plainly"))
      (testing "an id the Model does not offer is refused"
        (log/append!! (:log world) (log/pending :field-set a (:world-id world) {:name :model/available :type [:list :string] :value ["x"] :reader :both}))
        (is (false? (:ok (act! w "model.choose" {:principal (str root-id) :model (str a) :id "nope"})))))
      (testing "a Principal-scoped residency allowlist is enforced for choices and fallbacks"
        (is (:ok (ws/request!! w :set-field {:object root-id :field :policy/model-residencies
                                             :type [:list :keyword] :value [:tenant]})))
        (is (= :residency (get-in (act! w "model.choose" {:principal (str root-id) :model (str a)}) [:err :code])))
        (is (nil? (models/for-wake (act/db world) (state) nil)) "no configured Model has tenant residency")
        (is (nil? (models/fallback-for (act/db world) (state) nil))))
      (testing "local-only refuses external and admits local"
        (let [external (get-in (ws/request!! root :act {:class :model :action :create
                                                        :args {:kind :model :format :openai-chat :residency :external
                                                               :name "external" :model-id "external"}})
                               [:result :object])]
          (is (:ok (ws/request!! w :set-field {:object root-id :field :policy/model-residencies
                                               :type [:list :keyword] :value [:local]})))
          (is (= :residency (get-in (act! w "model.choose" {:principal (str root-id) :model (str external)}) [:err :code])))
          (is (:ok (act! w "model.choose" {:principal (str root-id) :model (str b)})))))
      (testing "clear goes back to the first eligible Model"
        (is (:ok (act! w "model.choose" {:principal (str root-id) :clear true})))
        (is (nil? (get-in (state) [:object/fields :principal/model-choice])))))))

(deftest what-a-failure-says-about-its-model
  (is (= :limit (models/failure-status "subscription_sharing_usage_limit_exceeded: x")))
  (is (= :unauthenticated (models/failure-status "HTTP 401 no")))
  (is (= :unreachable (models/failure-status "java.net.ConnectException: refused")))
  (is (= :unreachable (models/failure-status "HTTP 503 subscription_sharing_usage_unavailable")))
  (is (nil? (models/failure-status "HTTP 400 subscription_sharing_unsupported_capability"))))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))

(defn- reply-to [world message]
  (let [db (act/db world)]
    (some #(when (= message (get-in (objects/fields db %) [:message/in-reply-to :field/value])) %)
          (objects/objects-with-field db :message/in-reply-to))))

(defn- text-of [world id] (get-in (objects/fields (act/db world) id) [:message/text :field/value]))

(defn- say! [world root witness to text]
  (let [route (resolve-as root "root.route")
        w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{route} #{:transmit})
    (fx/grant! root (:id witness) :everything #{:read :write :evaluate})
    {:w w :ask (fn [t] (get-in (ws/request!! w :send {:to #{to} :route route :text t}) [:result :message]))}))

(deftest a-fresh-configured-external-model-answers-with-no-policy
  (let [dir (fx/temp-dir)
        server (HttpServer/create (InetSocketAddress. "127.0.0.1" 0) 0)
        line (fn [m] (str "data: " (j/write-value-as-string m) "\n\n"))]
    (.createContext server "/v1/models"
                    (reify HttpHandler
                      (handle [_ x]
                        (let [bs (.getBytes (j/write-value-as-string {:object "list" :data [{:id "configured"}]}) "UTF-8")]
                          (.add (.getResponseHeaders x) "content-type" "application/json")
                          (.sendResponseHeaders x 200 (alength bs))
                          (with-open [o (.getResponseBody x)] (.write o bs))))))
    (.createContext server "/v1/chat/completions"
                    (reify HttpHandler
                      (handle [_ x]
                        (let [bs (.getBytes (str (line {:choices [{:delta {:content "configured answer"}}] :model "configured"})
                                                 (line {:choices [{:delta {} :finish_reason "stop"}]
                                                        :usage {:prompt_tokens 5 :completion_tokens 2} :model "configured"})
                                                 "data: [DONE]\n\n") "UTF-8")]
                          (.add (.getResponseHeaders x) "content-type" "text/event-stream")
                          (.sendResponseHeaders x 200 (alength bs))
                          (with-open [o (.getResponseBody x)] (.write o bs))))))
    (.start server)
    (let [url (str "http://127.0.0.1:" (.getPort (.getAddress server)) "/v1")
          world (system/start (merge (fx/config dir 0) {:model-url url :model-id "configured"}))]
      (try
        (let [root (fx/attach-root world)
              witness (fx/create-witness! root)
              root-id (:principal root)
              model (resolve-as root "root.model")
              {:keys [w ask]} (say! world root witness root-id "x")
              q (ask "answer through the configured Model")
              reply (until #(reply-to world q) 15000)]
          (is (= :external (get-in (objects/fields (act/db world) model) [:model/residency :field/value])))
          (is (nil? (get-in (objects/fields (act/db world) root-id) [:policy/model-residencies :field/value])))
          (is reply "the fresh world's external default Model answers without a residency policy")
          (is (= "configured answer" (text-of world reply)))
          (ws/close! w)
          (ws/close! root))
        (finally (system/stop world) (.stop server 0) (fx/delete-tree! dir))))))

(deftest a-wake-moves-to-the-fallback-when-its-model-dies
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          dead (get-in (ws/request!! root :act {:class :model :action :create
                                                :args {:kind :model :format :openai-chat :residency :local :name "dead"
                                                       :model-id "dead" :base-url "http://127.0.0.1:1/v1"}})
                       [:result :object])
          stub (resolve-as root "root.model")
          {:keys [w ask]} (say! world root witness root-id "x")]
      (is (:ok (act! w "model.choose" {:principal (str root-id) :model (str dead)})))
      (is (:ok (act! w "model.choose" {:principal (str root-id) :model (str stub) :fallback true})))
      (let [q (ask "are you there?")]
        (is (until #(reply-to world q) 20000) "the wake still answered")
        (is (= :unreachable (get-in (objects/fields (act/db world) dead) [:model/status :field/value])))
        (is (str/includes? (str (get-in (objects/fields (act/db world) root-id) [:loop/model-note :field/value])) "dead"))
        (is (= stub (:model (models/for-wake (act/db world) (objects/state (act/db world) root-id) nil)))
            "the next wake passes it over")))))

(deftest a-plan-backed-model-answers-through-the-responses-route
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          seen (atom [])
          server (HttpServer/create (InetSocketAddress. "127.0.0.1" 0) 0)
          line (fn [m] (str "data: " (j/write-value-as-string m) "\n\n"))]
      (.createContext server "/v1/responses"
                      (reify HttpHandler
                        (handle [_ x]
                          (swap! seen conj {:auth (.getFirst (.getRequestHeaders ^HttpExchange x) "authorization")
                                            :body (j/read-value (slurp (.getRequestBody x)) j/keyword-keys-object-mapper)})
                          (let [bs (.getBytes (str (line {:type "response.output_text.delta" :delta "from the plan"})
                                                   (line {:type "response.completed"
                                                          :response {:model "gpt-a" :usage {:input_tokens 9 :output_tokens 3}}}))
                                              "UTF-8")]
                            (.add (.getResponseHeaders x) "content-type" "text/event-stream")
                            (.sendResponseHeaders x 200 (alength bs))
                            (with-open [o (.getResponseBody x)] (.write o bs))))))
      (.start server)
      (try
        (let [url (str "http://127.0.0.1:" (.getPort (.getAddress server)) "/v1")
              m (get-in (ws/request!! root :act {:class :model :action :create
                                                 :args {:kind :model :format :openai-chat :residency :external :name "plan"
                                                        :model-id "gpt-a" :base-url url}})
                        [:result :object])
              {:keys [w ask]} (say! world root witness root-id "x")]
          (#'chatgpt/write-record! (:data-dir world) m
                                   {:client_id "c" :access_token "the-access-token" :refresh_token "r" :expires_in 3600
                                    :saved_at (str (java.time.Instant/now))})
          (log/append!! (:log world) (log/pending :field-set m (:world-id world) {:name :model/api :type :keyword :value :chatgpt-plan :reader :both}))
          (is (nil? (get-in (objects/fields (act/db world) root-id) [:policy/model-residencies :field/value])))
          (is (= :external (get-in (objects/fields (act/db world) m) [:model/residency :field/value])))
          (is (:ok (act! w "model.choose" {:principal (str root-id) :model (str m) :id "gpt-a"}))
              "a ChatGPT plan is usable when no residency policy is set")
          (let [q (ask "hello")
                reply (until #(reply-to world q) 15000)
                call (first @seen)]
            (is reply)
            (is (= "from the plan" (some->> reply (text-of world))))
            (is (= "Bearer the-access-token" (:auth call)))
            (is (false? (get-in call [:body :store])))
            (is (true? (get-in call [:body :stream])))
            (is (= "gpt-a" (get-in call [:body :model])))
            (is (not (contains? (:body call) :max_output_tokens)))
            (is (not-any? #(= "system" (:role %)) (get-in call [:body :input])))
            (is (seq (get-in call [:body :instructions])))))
        (finally (.stop server 0))))))
