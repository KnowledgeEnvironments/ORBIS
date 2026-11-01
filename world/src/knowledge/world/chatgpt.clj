(ns knowledge.world.chatgpt
  "Sign in with ChatGPT for plan usage, the flow OpenAI documents for open-source and locally hosted
   apps (developers.openai.com/siwc/token-sharing-open-source). No client secret and no partner key:
   the first sign-in registers a client for this person, later ones reuse it.

   The browser is where the person is, not where the world runs, and a 127.0.0.1 callback reaches the
   browser's own computer. So the listener owns the loopback callback and the world owns everything
   else: it makes the state, nonce and PKCE verifier, exchanges the code, validates the ID token and
   keeps the tokens. The listener only ever carries a one-time code the verifier protects.

   Tokens are never Fields: a Field is on the log, in exports and in prompts. They live in one file per
   Model under <data>/credentials, written atomically, owner-only, inside the directory the box denies.
   Nothing here logs a token, and refreshes of one Model are serialized because the refresh token rotates."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [jsonista.core :as j])
  (:import [java.math BigInteger]
           [java.net URI URLEncoder]
           [java.net.http HttpClient HttpRequest HttpRequest$BodyPublishers HttpResponse$BodyHandlers]
           [java.nio.file Files LinkOption StandardCopyOption]
           [java.nio.file.attribute PosixFilePermissions]
           [java.security KeyFactory MessageDigest SecureRandom Signature]
           [java.security.spec RSAPublicKeySpec]
           [java.time Duration Instant]
           [java.util Base64 Date UUID]))

(defn- at
  "OpenAI's auth origin. KE_OPENAI_AUTH points a test world at a stub; only the operator sets it."
  [path]
  (str (or (System/getenv "KE_OPENAI_AUTH") "https://auth.openai.com") path))

(def endpoints
  "Where OpenAI lives."
  (atom {:issuer (at "")
         :authorize (at "/api/accounts/authorize")
         :token (at "/api/accounts/oauth/token")
         :discovery (at "/.well-known/openid-configuration")}))

(def resource "https://api.openai.com/v1")
(def api-base "Where requests go: the resource, unless a test world says otherwise."
  (or (System/getenv "KE_OPENAI_API") resource))
(def dynamic-client "dynamic_agent_client")
(def agent-name "ORBIS")
(def plan-scope "chatgpt.tokens.use.direct")
(def scopes "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct")
(def usage-url "https://chatgpt.com/settings/usage")
(def default-port 1455)
(def attempt-ms (* 10 60 1000))
(def refresh-margin-s 300)

(def unusable-refresh
  "Refresh errors that mean the token set is gone: clear it and sign in again."
  #{"invalid_grant" "invalid_refresh_token" "token_expired" "refresh_token_expired"
    "refresh_token_invalidated" "refresh_token_reused"})

;; ---------------------------------------------------------------- the credential store

(defn- dir ^java.io.File [data-dir] (io/file data-dir "credentials" "chatgpt"))
(defn- file-of ^java.io.File [data-dir model] (io/file (dir data-dir) (str model ".json")))

(defn- write-private!
  "Atomically, owner-only: the temp file is private before the first byte goes in."
  [^java.io.File f ^String text]
  (let [parent (.toPath (doto (.getParentFile f) .mkdirs))
        owner (PosixFilePermissions/fromString "rw-------")
        tmp (Files/createTempFile parent ".new" ".tmp"
                                  (into-array [(PosixFilePermissions/asFileAttribute owner)]))]
    (Files/writeString tmp text (into-array java.nio.file.OpenOption []))
    (Files/move tmp (.toPath f) (into-array java.nio.file.CopyOption
                                            [StandardCopyOption/ATOMIC_MOVE StandardCopyOption/REPLACE_EXISTING]))))

(defn read-record [data-dir model]
  (let [f (file-of data-dir model)]
    (when (.exists f) (j/read-value (slurp f) j/keyword-keys-object-mapper))))

(defn- write-record! [data-dir model record]
  (write-private! (file-of data-dir model) (j/write-value-as-string record)))

(defn host-id
  "This world's stable agent host id: opaque, kept for the host's life, and not a credential."
  [data-dir]
  (let [f (io/file (dir data-dir) "host-id")]
    (if (.exists f)
      (str/trim (slurp f))
      (let [id (str "urn:uuid:" (UUID/randomUUID))]
        (write-private! f id)
        id))))

(defn client-for
  "The issued client of a Model this world has signed in before, or nil."
  [data-dir model]
  (when model (:client_id (read-record data-dir model))))

;; ---------------------------------------------------------------- bytes

(defn- b64url [^bytes bs] (.encodeToString (.withoutPadding (Base64/getUrlEncoder)) bs))
(defn- unb64url ^bytes [^String s] (.decode (Base64/getUrlDecoder) s))
(defn- random-token [] (let [bs (byte-array 32)] (.nextBytes (SecureRandom.) bs) (b64url bs)))
(defn- enc [v] (URLEncoder/encode (str v) "UTF-8"))
(defn- form [m] (str/join "&" (for [[k v] m :when (some? v)] (str (name k) "=" (enc v)))))

(defn challenge
  "The PKCE S256 challenge of a verifier."
  [^String verifier]
  (b64url (.digest (MessageDigest/getInstance "SHA-256") (.getBytes verifier "US-ASCII"))))

;; ---------------------------------------------------------------- http

(defn- post-form [url params]
  (let [req (-> (HttpRequest/newBuilder (URI. url)) (.timeout (Duration/ofSeconds 30))
                (.header "content-type" "application/x-www-form-urlencoded")
                (.POST (HttpRequest$BodyPublishers/ofString (form params))) .build)
        r (.send (HttpClient/newHttpClient) req (HttpResponse$BodyHandlers/ofString))]
    {:status (.statusCode r) :body (.body r)}))

(defn- get-json [url]
  (let [req (-> (HttpRequest/newBuilder (URI. url)) (.timeout (Duration/ofSeconds 20)) .GET .build)
        r (.send (HttpClient/newHttpClient) req (HttpResponse$BodyHandlers/ofString))]
    (when (= 200 (.statusCode r)) (j/read-value (.body r) j/keyword-keys-object-mapper))))

(defn- parsed [^String body]
  (try (j/read-value body j/keyword-keys-object-mapper) (catch Exception _ nil)))

(defn- oauth-error [{:keys [status body]}]
  (let [p (parsed body)]
    (or (:error p) (:code p) (str "http-" status))))

;; ---------------------------------------------------------------- the ID token

(defn- rsa-key [{:keys [n e]}]
  (.generatePublic (KeyFactory/getInstance "RSA")
                   (RSAPublicKeySpec. (BigInteger. 1 (unb64url n)) (BigInteger. 1 (unb64url e)))))

(defn- signing-keys []
  (let [{:keys [jwks_uri]} (get-json (:discovery @endpoints))]
    (:keys (get-json jwks_uri))))

(defn validate-id-token
  "The ID token's claims when its signature, issuer, audience, expiry and nonce all hold, else nil."
  [^String token client nonce]
  (let [[head body sig :as parts] (str/split (str token) #"\.")]
    (when (= 3 (count parts))
      (let [h (j/read-value (unb64url head) j/keyword-keys-object-mapper)
            c (j/read-value (unb64url body) j/keyword-keys-object-mapper)
            key (some #(when (= (:kid %) (:kid h)) %) (signing-keys))
            aud (:aud c)]
        (when (and key (= "RS256" (:alg h))
                   (let [s (doto (Signature/getInstance "SHA256withRSA") (.initVerify (rsa-key key))
                                 (.update (.getBytes (str head "." body) "US-ASCII")))]
                     (.verify s (unb64url sig)))
                   (= (:issuer @endpoints) (:iss c))
                   (if (sequential? aud) (some #{client} aud) (= client aud))
                   (> (long (:exp c)) (quot (System/currentTimeMillis) 1000))
                   (= nonce (:nonce c)))
          c)))))

;; ---------------------------------------------------------------- signing in

(defonce ^:private attempts (atom {}))

(defn begin!
  "A new attempt for person, on the listener's loopback port. Returns {:attempt :url}. model, when
   given, is a Model this world signed in before: its issued client is reused."
  [data-dir person {:keys [model port]}]
  (let [port (long (or port default-port))
        attempt (str (UUID/randomUUID))
        state (random-token) nonce (random-token) verifier (str (random-token) (random-token))
        redirect (str "http://127.0.0.1:" port "/auth/callback")
        saved (client-for data-dir model)
        record (when model (read-record data-dir model))
        params (cond-> {:client_id (or saved dynamic-client)
                        :ext_agent_host_id (host-id data-dir)
                        :response_type "code" :redirect_uri redirect :scope scopes :resource resource
                        :state state :nonce nonce
                        :code_challenge_method "S256" :code_challenge (challenge verifier)}
                 (nil? saved) (assoc :agent_name_hint agent-name)
                 (:email record) (assoc :login_hint (:email record)))
        now (System/currentTimeMillis)]
    (swap! attempts (fn [m] (assoc (into {} (filter (fn [[_ a]] (< (- now (:at a)) attempt-ms))) m)
                                   attempt {:person person :state state :nonce nonce :verifier verifier
                                            :redirect redirect :client saved :model model :at now})))
    {:attempt attempt :url (str (:authorize @endpoints) "?" (form params))}))

(defn cancel! [attempt] (swap! attempts dissoc attempt) nil)

(defn- fail [code message] {:err {:code code :message message}})

(defn- record-of [{:keys [access_token refresh_token id_token token_type expires_in scope earliest_refresh_at]}
                  claims client host]
  (cond-> {:email (:email claims) :issuer (:iss claims) :subject (:sub claims)
           :client_id client :ext_agent_host_id host :id_token id_token
           :access_token access_token :refresh_token refresh_token :token_type token_type
           :expires_in expires_in :scopes (vec (str/split (str scope) #" "))
           :saved_at (str (Instant/now))}
    earliest_refresh_at (assoc :earliest_refresh_at earliest_refresh_at)))

(defn finish!
  "The callback's {:attempt :state :code :client-id :error}, from the person who began it. On success
   the record is returned for a store! once the Model exists: {:ok {:record}}. state is checked before
   anything else is believed, a declined consent stops here, and a token without the plan scope keeps
   nothing: it is reported, not used."
  [person {:keys [attempt state code client-id error]}]
  (let [a (get @attempts attempt)]
    (cond
      (nil? a) (fail :invalid-args "no sign-in in progress: start again")
      (not= person (:person a)) (fail :person-only "another person began this sign-in")
      (not= state (:state a)) (do (cancel! attempt) (fail :invalid-args "the callback's state does not match: nothing was exchanged"))
      (= "access_denied" error) (do (cancel! attempt) (fail :consent-declined "ChatGPT plan use was declined"))
      error (do (cancel! attempt) (fail :sign-in-failed (str error)))
      (nil? code) (do (cancel! attempt) (fail :invalid-args "no code in the callback"))
      (and (:client a) client-id (not= client-id (:client a)))
      (do (cancel! attempt) (fail :sign-in-failed "the callback named a different client: refused"))
      :else
      (let [client (or (:client a) client-id)]
        (cancel! attempt)
        (if (or (nil? client) (= dynamic-client client))
          (fail :sign-in-failed "registration did not issue a client: sign in again")
          (let [r (post-form (:token @endpoints)
                             {:grant_type "authorization_code" :client_id client :code code
                              :code_verifier (:verifier a) :redirect_uri (:redirect a) :resource resource})
                t (when (= 200 (:status r)) (parsed (:body r)))]
            (cond
              (nil? t) (fail :sign-in-failed (str "code exchange refused: " (oauth-error r)))
              :else
              (if-let [claims (validate-id-token (:id_token t) client (:nonce a))]
                (cond
                  (not (some #{plan-scope} (str/split (str (:scope t)) #" ")))
                  (fail :plan-not-granted "signed in, but ChatGPT plan use was not granted")
                  :else {:ok {:model (:model a) :client client
                              :account (:email claims) :subject (:sub claims)
                              :tokens t :claims claims}})
                (fail :sign-in-failed "the ID token did not validate: nothing was kept")))))))))

(defn store!
  "Keep a validated sign-in as model's credential record."
  [data-dir model {:keys [tokens claims client]}]
  (write-record! data-dir model (record-of tokens claims client (host-id data-dir))))

;; ---------------------------------------------------------------- using it

(defn- locks-for [m model] (if (contains? m model) m (assoc m model (Object.))))
(defonce ^:private locks (atom {}))
(defn- lock-of [model] (get (swap! locks locks-for model) model))

(defn- expires-at ^long [{:keys [saved_at expires_in]}]
  (+ (.getEpochSecond (Instant/parse saved_at)) (long (or expires_in 0))))

(defn- fresh? [record]
  (> (- (expires-at record) (quot (System/currentTimeMillis) 1000)) refresh-margin-s))

(defn- clear-tokens!
  "The token set is gone. The issued client and the identity stay, for a later sign-in."
  [data-dir model record]
  (write-record! data-dir model (dissoc record :access_token :refresh_token :id_token)))

(defn access-token
  "A valid access token for model, refreshing near expiry, or nil when a person must sign in again.
   A temporary failure keeps the credentials and throws, so a network fault never signs anyone out."
  [data-dir model]
  (locking (lock-of model)
    (let [record (read-record data-dir model)]
      (cond
        (nil? (:access_token record)) nil
        (fresh? record) (:access_token record)
        (nil? (:refresh_token record)) nil
        :else
        (let [r (post-form (:token @endpoints)
                           {:grant_type "refresh_token" :client_id (:client_id record)
                            :refresh_token (:refresh_token record) :resource resource})]
          (if (= 200 (:status r))
            (let [t (parsed (:body r))
                  next-record (cond-> (assoc record :access_token (:access_token t)
                                             :refresh_token (or (:refresh_token t) (:refresh_token record))
                                             :expires_in (:expires_in t) :saved_at (str (Instant/now)))
                                (:scope t) (assoc :scopes (vec (str/split (str (:scope t)) #" ")))
                                (:id_token t) (assoc :id_token (:id_token t)))]
              (write-record! data-dir model next-record)
              (:access_token next-record))
            (let [code (oauth-error r)]
              (if (unusable-refresh (str code))
                (do (clear-tokens! data-dir model record) nil)
                (throw (ex-info "token refresh failed for now" {:code code}))))))))))

(defn sign-out!
  "End the renewable session, then clear the tokens. The client mapping and host id stay. Returns
   {:revoked bool}: false means the session could not be confirmed ended, and the person should
   disconnect the app in ChatGPT settings."
  [data-dir model]
  (locking (lock-of model)
    (when-let [record (read-record data-dir model)]
      (let [revoked (try
                      (when (:refresh_token record)
                        (let [{:keys [revocation_endpoint]} (get-json (:discovery @endpoints))
                              r (post-form revocation_endpoint
                                           {:token (:refresh_token record) :token_type_hint "refresh_token"
                                            :client_id (:client_id record)})]
                          (= 200 (:status r))))
                      (catch Exception _ false))]
        (clear-tokens! data-dir model record)
        {:revoked (boolean revoked)}))))

(defn catalogue
  "The account's models, the ones OpenAI means to be listed, in the server's order: [{:slug :name}],
   from a parsed GET /models body."
  [body]
  (vec (for [m (:models body) :when (and (map? m) (= "list" (:visibility m)))]
         (cond-> {:slug (:slug m) :name (or (:display_name m) (:slug m))}
           (number? (:context_window m)) (assoc :context-window (long (:context_window m)))))))

(defn signed-in? [data-dir model] (boolean (:access_token (read-record data-dir model))))
