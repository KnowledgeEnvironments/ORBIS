(ns knowledge.world.gateway.core
  "Message gateways: an Adapter (adapter/binds :gateway) joining a person's phone or chat account to a
   world. BlueBubbles first, every other service behind knowledge.world.gateway.transport.

   What keeps it safe is who a text is from. A handle reaches nobody until a person's own session binds
   it to a person and an agent (gateway.bind); an unknown sender is counted and dropped, unread. A bound
   text arrives from the gateway Adapter, not from the person: a Principal driven by nothing, so the
   sender is no person, a line of it is no directive, and neither a card nor unsafe mode takes it for a
   person's word. It wakes an agent and carries no authority: what comes in over a phone cannot approve,
   arm or grant, whoever it claims to be. Going out is gateway.send, a gated action, on a card.

   gateway/* Fields are the kernel's (knowledge.world.authority): write on the Adapter must not add a handle or
   turn the password's variable at another secret. The password itself is never a Field and never on
   the log: gateway/key-env names the environment variable that holds it.

   The poll runs off the scheduler's thread, and does nothing for an Adapter with gateway/enabled false."
  (:require [clojure.string :as str]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.gateway.bluebubbles]
            [knowledge.world.gateway.transport :as transport]
            [knowledge.world.models :as models]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]
            [taoensso.timbre :as timbre]))

(def kinds #{"bluebubbles"})
(def default-poll-s 5)
(def min-poll-s 2)
(def text-cap 4000)
(def out-cap 2000)
(def per-minute 20)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- fail! [code message] (throw (ex-info message {:err {:code code :message message}})))

(defn secret [key-env] (when key-env (System/getenv key-env)))

(defn gateway? [db id] (= :gateway (some-> (field db id :adapter/binds) keyword)))

(defn gateways [db] (filter #(gateway? db %) (objects/objects-of-class db :adapter)))

(defn- conn [db a]
  {:kind (keyword (field db a :gateway/kind)) :url (field db a :adapter/locator)
   :password (secret (field db a :gateway/key-env))})

(defn handle-key
  "One spelling of a handle: an email in lower case, a phone number as + and digits."
  [h]
  (let [s (str/lower-case (str/trim (str h)))]
    (if (re-matches #"[+\d\s().-]+" s) (str/replace s #"[^+\d]" "") s)))

(defn- put! [world a k type value]
  (act/set-field! world (objects/root-id (act/db world)) {:object a :field k :type type :value value :reader :both}))

(defn create!
  "The Adapter, made by root for a person: enabled, reading from now, so no history comes in."
  [world {:keys [name kind url key-env poll-s] :as args}]
  (when (models/secret-in-args? args)
    (fail! :secret-in-args "never put a credential in args: key-env names the environment variable that holds it"))
  (when-not (kinds (str kind)) (fail! :invalid-args (str "gateways know: " (str/join ", " (sort kinds)))))
  (when-not (re-find #"^https?://" (str url)) (fail! :invalid-args "url is the service's http(s) address"))
  (let [root (objects/root-id (act/db world))
        {:keys [ok err]} (act/create! world root :adapter {:binds :gateway :locator url :sync :both :name name})]
    (when err (fail! (:code err) (:message err)))
    (let [a (:object ok)]
      (put! world a :gateway/kind :string (str kind))
      (when key-env (put! world a :gateway/key-env :string key-env))
      (put! world a :gateway/poll-s :number (max min-poll-s (long (or poll-s default-poll-s))))
      (put! world a :gateway/cursor :map {:at (System/currentTimeMillis) :ids []})
      (put! world a :gateway/bindings [:list [:map :string]] [])
      (put! world a :gateway/dropped :number 0)
      (put! world a :gateway/enabled :boolean true)
      (put! world a :gateway/status :keyword (transport/ping (conn (act/db world) a)))
      {:gateway a :status (field (act/db world) a :gateway/status)})))

(defn- named!
  "The gateway a name or id denotes for p, or unresolved."
  [db p gateway]
  (let [a (or (->uuid gateway) (resolve/lookup db p (str gateway)))]
    (if (and a (gateway? db a)) a (throw (ex-info "does not resolve" {:err (:err act/unresolved)})))))

(defn- bindings [db a] (vec (field db a :gateway/bindings)))

(defn- binding-for [db a handle]
  (let [k (handle-key handle)] (some #(when (= k (:handle %)) %) (bindings db a))))

(defn- name-of [db id]
  (or (some (fn [[_ n]] n) (objects/names db id)) (str id)))

(defn- route-for!
  "The Route between the gateway and agent, kept from an earlier binding or made by root."
  [world a agent]
  (let [db (act/db world)]
    (or (some #(when (= (str agent) (:agent %)) (->uuid (:route %))) (bindings db a))
        (let [handle! (requiring-resolve 'knowledge.world.act.actions/handle!)
              {:keys [ok err]} (handle! :route.create world (objects/root-id db)
                                        {:name (str (name-of db a) "." (name-of db agent) "." (subs (str (random-uuid)) 0 4))
                                         :members [(str a) (str agent)]} {})]
          (when err (fail! (:code err) (:message err)))
          (get-in ok [:result :route])))))

(defn bind!
  "A handle, a person and an agent, by that person's own session (the handler checks it)."
  [world person {:keys [gateway handle agent]}]
  (let [db (act/db world)
        a (named! db person gateway)
        ag (or (->uuid agent) (resolve/lookup db person (str agent)))
        k (handle-key handle)]
    (when (str/blank? k) (fail! :invalid-args "handle is a phone number or an address"))
    (when-not (and ag (= :process (:principal/driven-by (objects/state db ag))))
      (fail! :invalid-args "agent names a process-driven Principal"))
    (let [route (route-for! world a ag)
          row {:handle k :person (str person) :agent (str ag) :route (str route)}
          rows (conj (vec (remove #(= k (:handle %)) (bindings (act/db world) a))) row)]
      (put! world a :gateway/bindings [:list [:map :string]] rows)
      {:gateway a :handle k :agent ag :route route})))

(defn unbind! [world person {:keys [gateway handle]}]
  (let [db (act/db world)
        a (named! db person gateway)
        k (handle-key handle)
        rows (bindings db a)]
    (when-not (some #(= k (:handle %)) rows) (fail! :conflict (str k " is not bound here")))
    (put! world a :gateway/bindings [:list [:map :string]] (vec (remove #(= k (:handle %)) rows)))
    {:gateway a :handle k :bound false}))

(defn enable! [world person {:keys [gateway on]}]
  (let [a (named! (act/db world) person gateway)]
    (put! world a :gateway/enabled :boolean (boolean on))
    {:gateway a :enabled (boolean on)}))

(defn list-of
  "What a Principal may see: every gateway it can read, status and bindings."
  [world p]
  (let [db (act/db world)]
    {:gateways (vec (for [a (gateways db)
                          :when (resolve/resolves? db p (act/id-name a) :read)]
                      {:gateway a :name (name-of db a) :kind (field db a :gateway/kind)
                       :status (field db a :gateway/status) :enabled (true? (field db a :gateway/enabled))
                       :dropped (long (or (field db a :gateway/dropped) 0))
                       :bindings (mapv #(select-keys % [:handle :person :agent]) (bindings db a))}))}))

;; ---------------------------------------------------------------- in

(defonce ^:private windows (atom {}))

(defn- admit?
  "One text from this handle in its minute. A phone must not be able to run an agent's turns dry."
  [a handle now]
  (let [k [a handle]
        {:keys [since n]} (get @windows k)
        fresh? (or (nil? since) (>= (- now since) 60000))
        n' (if fresh? 1 (inc n))]
    (swap! windows assoc k {:since (if fresh? now since) :n n'})
    (<= n' per-minute)))

(defn- clip [s n] (let [s (str s)] (if (> (count s) n) (subs s 0 n) s)))

(defn- deliver!
  "A bound handle's text as a Message from the Adapter. true when it went."
  [world a {:keys [person agent route]} {:keys [handle text id chat]}]
  (let [{:keys [err]} (route/send! world a {:to #{(->uuid agent)} :route (->uuid route)
                                            :text (clip text text-cap) :tags [] :kind :gateway
                                            :gateway {:handle (handle-key handle) :person person :id id :chat chat}})]
    (when err (timbre/warn "a gateway text was not delivered" {:gateway a :code (:code err)}))
    (nil? err)))

(defn- seen? [{:keys [at ids]} {m-at :at id :id}]
  (or (< m-at (long (or at 0))) (and (= m-at at) (contains? (set ids) id))))

(defn poll-one!
  "One read of one gateway: new texts in order, status kept, what nobody bound counted and dropped."
  [world a]
  (let [db (act/db world)
        cursor (field db a :gateway/cursor)
        {:keys [status messages]} (transport/fetch (conn db a) {:after (:at cursor)})
        fresh (->> messages (remove #(seen? cursor %)) (sort-by :at))]
    (when (not= status (some-> (field db a :gateway/status) keyword))
      (put! world a :gateway/status :keyword status))
    (when (and (= :ok status) (seq fresh))
      (let [now (System/currentTimeMillis)
            in (remove :from-me? fresh)
            went (reduce (fn [n m]
                           (let [b (binding-for (act/db world) a (:handle m))]
                             (if (and b (admit? a (:handle b) now) (deliver! world a b m)) (inc n) n)))
                         0 in)
            at (:at (last fresh))
            dropped (- (count in) went)]
        (put! world a :gateway/cursor :map {:at at :ids (mapv :id (filter #(= at (:at %)) fresh))})
        (when (pos? dropped)
          (put! world a :gateway/dropped :number (+ dropped (long (or (field (act/db world) a :gateway/dropped) 0)))))))
    status))

(defonce ^:private polling (atom {}))

(defn poll-all!
  "From the scheduler: each enabled gateway when due, on a thread of its own, one at a time."
  [world]
  (let [db (act/db world)
        now (System/currentTimeMillis)]
    (doseq [a (gateways db)
            :when (true? (field db a :gateway/enabled))
            :let [every (* 1000 (long (or (field db a :gateway/poll-s) default-poll-s)))
                  {:keys [busy at]} (get @polling a)]
            :when (and (not busy) (>= (- now (long (or at 0))) every))]
      (swap! polling assoc a {:busy true :at now})
      (.start (Thread/ofVirtual)
              (fn []
                (try (poll-one! world a)
                     (catch Exception e (timbre/warn "gateway poll failed" {:gateway a :type (type e)}))
                     (finally (swap! polling assoc-in [a :busy] false))))))))

;; ---------------------------------------------------------------- out

(defn- wake-text
  "The gateway and handle of the Message p is answering, when it came in through one."
  [db message]
  (when message
    (let [f (objects/fields db message)]
      (when (= :gateway (some-> (get-in f [:message/kind :field/value]) name keyword))
        {:gateway (get-in f [:message/from :field/value])
         :in (get-in f [:message/gateway :field/value])}))))

(defn send-out!
  "gateway.send: a bound handle only, by its agent or a person. The action is gated, so a loop's call
   came through a card."
  [world p {:keys [gateway handle text]} {:keys [message session?]}]
  (let [db (act/db world)
        {wake-gateway :gateway in :in} (wake-text db message)
        a (if gateway (named! db p gateway) wake-gateway)
        h (or handle (:handle in))
        b (when (and a h) (binding-for db a h))
        person? (authority/person-session? db p session?)]
    (when-not (and b (or person? (= (str p) (:agent b))))
      (throw (ex-info "does not resolve" {:err (:err act/unresolved)})))
    (when-not (true? (field db a :gateway/enabled)) (fail! :conflict "that gateway is turned off"))
    (let [{:keys [status]} (transport/push (conn db a)
                                           {:handle (:handle b) :text (clip text out-cap)
                                            :chat (when (= (:handle b) (:handle in)) (:chat in))})]
      (when-not (= :ok status) (fail! status (str "the gateway answered " (name status))))
      {:gateway a :handle (:handle b) :sent true})))

