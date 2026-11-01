(ns knowledge.world.models
  "The Model catalogue (spec 5.6, 11.5). A Model is an Object with its locator
   (model/base-url), model (model/id), the name of the environment variable holding its
   key (model/key-env: the key itself is never a Field, never on the log), its prices per million
   tokens (model/price-in, model/price-out, 0 until someone says otherwise), model/context-tokens,
   a label people read, and model/status (ok | unreachable | unauthenticated | unknown) kept by a
   probe: GET <base-url>/models with the bearer, every probe-every-ms from the scheduler and on the
   action model.probe. The probe's books (status, models, probed-at) are written by the world, as
   the shipped Fields are; a person or Principal creates a Model with the action model.create,
   which refuses a literal credential in its args (err secret-in-args)."
  (:require [clojure.string :as str]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.chatgpt :as chatgpt]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [taoensso.timbre :as timbre])
  (:import [java.net URI]
           [java.net.http HttpClient HttpRequest HttpResponse$BodyHandlers]
           [java.time Duration]
           [java.util Date]))

(def statuses #{:ok :unreachable :unauthenticated :limit :unknown})
(def down "A Model in one of these is passed over for a fallback." #{:unreachable :unauthenticated :limit})
(def probe-every-ms 300000) ; the scheduler's job: every five minutes
(def probe-timeout-s 10)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(def env-var-name #"^[A-Z_][A-Z0-9_]*$")
(def secret-arg-names #{:key :api-key :apikey :api_key :token :secret :password :bearer :authorization})

(defn secret-in-args?
  "A literal credential in action args: a key named like one, or a key-env that is not an
   environment variable's name (a value with a dash, a dot or lowercase letters is a key, not a name)."
  [args]
  (boolean
   (or (some (fn [[k _]] (contains? secret-arg-names (keyword (name k)))) args)
       (let [v (:key-env args)] (and (string? v) (not (re-matches env-var-name v)))))))

(def types
  {:model/label :string :model/price-in :number :model/price-out :number :model/status :keyword
   :model/available [:list :string] :model/probed-at :instant :model/context-tokens :number :model/reported-window :number :model/images :boolean
   :model/api :keyword :model/catalogue [:list :map]})

(defn label-of
  "The name people read for a Model: its world-namespace name, else any name, else its model id."
  [db id]
  (or (some (fn [[ns n]] (when (= ns (objects/world-id db)) n)) (sort-by second (objects/names db id)))
      (some (fn [[_ n]] n) (sort-by second (objects/names db id)))
      (some-> (field db id :model/id) str)
      (subs (str id) 0 8)))

(defn catalogue-defaults
  "The catalogue Fields every Model carries: label and status unknown; prices only when someone
   names them (absent is unpriced, not free)."
  [db id]
  {:model/label (label-of db id) :model/status :unknown})

(defn missing
  "The catalogue defaults a Model lacks."
  [db id]
  (let [fields (objects/fields db id)]
    (into {} (remove (fn [[k _]] (contains? fields k))) (catalogue-defaults db id))))

(defn models-without-catalogue
  "Models born before the catalogue Fields existed."
  [db]
  (vec (sort-by str (filter #(seq (missing db %)) (objects/objects-of-class db :model)))))

(defn- models-url [base-url] (str (str/replace (str base-url) #"/+$" "") "/models"))

(defn reported-window
  "The context window in a server's Prometheus text, or nil: `tensorfold_health:context_length 1048576`,
   and any other server's metric ending in the same name. A server need not say in /models."
  [^String metrics]
  (some->> (re-find #"(?m)^[\w:]*context_length(?:\{[^}]*\})?\s+(\d+)(?:\.0+)?\s*$" (str metrics)) second parse-long))

(defn- metrics-url [base-url]
  (str (str/replace (str/replace (str base-url) #"/+$" "") #"/v\d+$" "") "/metrics"))

(defn- fetch-window
  "What the server at base-url says its window is, or nil. Asked beside the probe, and a server with
   no /metrics is an ordinary one."
  [base-url key]
  (try
    (let [req (-> (HttpRequest/newBuilder (URI. (metrics-url base-url)))
                  (.timeout (Duration/ofSeconds probe-timeout-s))
                  (cond-> key (.header "authorization" (str "Bearer " key)))
                  (.GET)
                  .build)
          resp (.send (HttpClient/newHttpClient) req (HttpResponse$BodyHandlers/ofString))]
      (when (= 200 (.statusCode resp)) (reported-window (.body resp))))
    (catch Exception _ nil)))

(defn probe-url
  "GET <base-url>/models with the bearer. Returns {:status :models :http :error}: 200 → :ok with the
   server's model ids, 401/403 → :unauthenticated, anything else or no answer → :unreachable."
  [base-url key]
  (try
    (let [req (-> (HttpRequest/newBuilder (URI. (models-url base-url)))
                  (.timeout (Duration/ofSeconds probe-timeout-s))
                  (.header "accept" "application/json")
                  (cond-> key (.header "authorization" (str "Bearer " key)))
                  (.GET)
                  .build)
          resp (.send (HttpClient/newHttpClient) req (HttpResponse$BodyHandlers/ofString))
          code (.statusCode resp)]
      (cond
        (#{401 403} code) {:status :unauthenticated :http code}
        (= 200 code) (let [parsed (try (j/read-value (.body resp) j/keyword-keys-object-mapper) (catch Exception _ nil))
                           ;; an account's catalogue says `models`, an OpenAI-shaped server says `data`
                           catalogue (when (contains? parsed :models) (chatgpt/catalogue parsed))
                           models (if catalogue
                                    (mapv :slug catalogue)
                                    (vec (keep (fn [m] (when (map? m) (some-> (:id m) str))) (:data parsed))))]
                       (cond-> {:status :ok :models models :http code}
                         catalogue (assoc :catalogue catalogue)
                         (not catalogue) (assoc :window (fetch-window base-url key))))
        :else {:status :unreachable :http code :error (str "HTTP " code)}))
    (catch Exception e
      {:status :unreachable :error (str (.getSimpleName (class e)) (when (ex-message e) (str ": " (ex-message e))))})))

(defn plan-token
  "The access token of a plan-backed Model, or nil. A temporary fault reads as nil here: the record
   stays, and the next call tries again."
  [world model]
  (try (chatgpt/access-token (:data-dir world) model)
       (catch Exception e (timbre/warn "plan token unavailable" {:model model :reason (ex-message e)}) nil)))

(defn- note! [world model k v]
  (log/append!! (:log world) (log/pending :field-set model (:world-id world) {:name k :type (types k) :value v :reader :both})))

(defn probe!
  "Probe one Model and record model/status, model/available and model/probed-at, the world as
   actor (bookkeeping, like the shipped Fields). A Model without model/base-url is the local stub:
   :ok, its model id the one model. Returns the probe result."
  [world model]
  (let [db (act/db world)
        url (field db model :model/base-url)
        plan? (= :chatgpt-plan (field db model :model/api))
        token (when plan? (plan-token world model))
        key (if plan? token (some-> (field db model :model/key-env) System/getenv))
        r (cond
            (and plan? (nil? token)) {:status :unauthenticated}
            url (probe-url url key)
            :else {:status :ok :models [(str (or (field db model :model/id) "stub"))]})]
    (note! world model :model/status (:status r))
    (when (:models r) (note! world model :model/available (vec (:models r))))
    (when (:catalogue r) (note! world model :model/catalogue (vec (:catalogue r))))
    (when (:window r) (note! world model :model/reported-window (long (:window r))))
    (note! world model :model/probed-at (Date.))
    r))

(defn probe-all!
  "Probe every Model that has a locator (the scheduler's job). Returns {model status}."
  [world]
  (let [db (act/db world)]
    (into {} (for [e (sort-by str (objects/objects-of-class db :model))
                   :when (field db e :model/base-url)]
               [e (:status (try (probe! world e)
                                (catch Throwable t (timbre/warn t "model probe failed" {:model e}) {:status :unreachable})))]))))

;; ---------------------------------------------------------------- which Model a wake uses

(defn- pick
  "A stored choice {:model id :id slug} as {:model uuid :model-id slug}, or nil. JSON clients write
   strings, and the Field keeps what they wrote."
  [v]
  (when (map? v)
    (let [m (or (:model v) (get v "model")) slug (or (:id v) (get v "id"))
          id (cond (uuid? m) m (string? m) (parse-uuid m))]
      (when id (cond-> {:model id} (seq slug) (assoc :model-id slug))))))

(defn- live? [db id]
  (and id (objects/exists? db id) (not (objects/retired? db id))
       (= :model (field db id :model/kind))))

(defn- up? [db id] (not (down (field db id :model/status))))

(defn- first-of
  "The first of the Principal's Models, as a pick."
  [db p-state]
  (some (fn [e] (when (live? db e) {:model e}))
        (sort-by str (get-in p-state [:object/fields :principal/models :field/value]))))

(defn fallback-for
  "The Principal's fallback, if it can be used and is not `not-this`."
  [db p-state not-this]
  (let [f (pick (get-in p-state [:object/fields :principal/model-fallback :field/value]))]
    (when (and f (live? db (:model f)) (up? db (:model f)) (not= not-this (:model f))) f)))

(defn for-wake
  "What a wake runs on: the conversation's choice, else the Principal's, else its first Model. A
   chosen Model that is down gives way to the fallback, and says so in :fell-back-from.
   {:model :model-id? :fell-back-from?}, or nil when the Principal has no Model."
  [db p-state thread]
  (let [conversation (when thread (pick (field db thread :conversation/model)))
        principal (pick (get-in p-state [:object/fields :principal/model-choice :field/value]))
        chosen (or (when (and conversation (live? db (:model conversation))) (assoc conversation :source :conversation))
                   (when (and principal (live? db (:model principal))) (assoc principal :source :principal))
                   (some-> (first-of db p-state) (assoc :source :first)))]
    (when chosen
      (if (up? db (:model chosen))
        chosen
        (if-let [f (fallback-for db p-state (:model chosen))]
          (assoc f :fell-back-from (:model chosen) :source :fallback)
          chosen)))))

(defn window
  "A Model's context window in tokens: what the provider's catalogue says for this id, else what its
   server reports, else model/context-tokens, else nil, when nothing has said."
  [db model model-id]
  (let [listed (some (fn [e]
                       (when (= (str model-id) (str (or (:slug e) (get e "slug"))))
                         (or (:context-window e) (get e "context-window"))))
                     (when model (field db model :model/catalogue)))
        reported (when model (field db model :model/reported-window))
        set-by-hand (when model (field db model :model/context-tokens))]
    (some #(when (and (number? %) (pos? %)) (long %)) [listed reported set-by-hand])))

(defn window-of
  "The window of what p would run on now, in this conversation, or nil."
  [db p thread]
  (let [{:keys [model model-id]} (for-wake db (objects/state db p) thread)]
    (window db model (or model-id (when model (field db model :model/id))))))

(defn failure-status
  "What a failed call says about its Model: :limit, :unauthenticated, :unreachable, or nil when the
   fault is the request's, not the Model's."
  [^String error]
  (let [e (str error)]
    (cond
      (re-find #"usage_limit_exceeded" e) :limit
      (re-find #"HTTP 40[13]|user_not_eligible|invalid_user|chatpass_v2" e) :unauthenticated
      (re-find #"HTTP 5\d\d|HTTP 429|usage_unavailable|user_unavailable|ConnectException|HttpTimeoutException|ClosedChannel|No route|Connection refused" e)
      :unreachable)))

(defn note-failure!
  "Record a failed call on its Model, so the next wake passes it over until a probe finds it well."
  [world model error]
  (when-let [s (failure-status error)]
    (note! world model :model/status s)
    s))

(defn for-call
  "The adapter a call uses: the chosen model id, and a plan-backed Model's token. No token reaches the
   log or a Field: it goes straight into the request header."
  [world model adapter model-id]
  (cond-> adapter
    (seq model-id) (assoc :model model-id)
    (:responses? adapter) (assoc :key (plan-token world model))))

(defn adopt-plan!
  "A signed-in Model's books: it speaks the Responses route, and its catalogue comes from a probe."
  [world model]
  (note! world model :model/api :chatgpt-plan)
  (let [r (probe! world model)
        db (act/db world)]
    (when-let [first-slug (first (:models r))]
      (when-not (some #{(field db model :model/id)} (:models r))
        (log/append!! (:log world) (log/pending :field-set model (:world-id world)
                                                {:name :model/id :type :string :value first-slug :reader :both}))))
    r))

