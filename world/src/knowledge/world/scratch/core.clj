(ns knowledge.world.scratch.core
  "Off-the-record sessions: run under a Principal, read what it can read, keep nothing.

   A scratch is an in-memory fork. Its index starts from the world's db at the head, its log holds
   its own events in an atom after that seq and reads the parent's before it, its blobs live in an
   atom, so the model, the loop, the serializer and resolution run unchanged and nothing reaches a
   disk. No resident loop or scheduler sees it; scratch.send runs the Principal's cycle itself.

   Reached only from a session of a person-driven Principal (err session-only otherwise), through
   `act` with no object and no class. These are not named actions: they are never bound and never
   listed, so a model is not told they exist.

   What a scratch may do is knowledge.world.scratch.policy. Its events go to the opening session
   alone, on push frames carrying `scratch`, and its log lines are silenced: an exception's data can
   hold text. The Model still receives the prompt: the world keeps nothing, but what a model
   server keeps is outside it.

   The one thing written: scratch-began and scratch-ended on the person's stream, naming who, which
   Principal, when and how much, with no text, Field names or paths. World Field policy/scratch-audit
   false turns them off only when a person set it: nobody, root included, reads through a Principal
   invisibly."
  (:require [clojure.string :as str]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.index.core :as index]
            [knowledge.world.log.core :as log]
            [knowledge.world.loop.core :as rloop]
            [knowledge.world.protocol.codec :as codec]
            [knowledge.world.protocol.frames :as frames]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]
            [org.httpkit.server :as hk]
            [taoensso.timbre :as timbre]))

(def default-max-s 7200)
(def outbox-cap 2000)

(defn registry [] (atom {}))

(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- driven-by [db p] (:principal/driven-by (objects/state db p)))
(defn- field [state k] (get-in state [:object/fields k :field/value]))

(defn- audit-on?
  "On unless a person turned it off: a loop or an agent's session setting the Field changes nothing."
  [world db]
  (let [f (get (objects/fields db (:world-id world)) :policy/scratch-audit)
        by (or (:field/set-by f) (:field/author f))]
    (not (and f (false? (:field/value f)) (authority/person? db by)))))

(defn- audit! [world kind person body]
  (when (audit-on? world (act/db world))
    (log/append!! (:log world) (log/pending kind person person body))))

;; ---------------------------------------------------------------- delivery

(defn- noise? [{:keys [event/kind]}] (contains? #{:field-read :resolved} kind))

(defn- tally [entry {:keys [event/kind event/body]}]
  (cond
    (= kind :sent) (swap! entry update :messages inc)
    (and (= kind :evaluated) (= :done (:state body)))
    (let [{:keys [prompt-tokens completion-tokens]} (get-in body [:outcome :usage])]
      (swap! entry #(-> % (update :tokens-in + (long (or prompt-tokens 0)))
                        (update :tokens-out + (long (or completion-tokens 0))))))))

(defn- deliver!
  "Push a scratch's events to the session that opened it; held while its socket is down."
  [entry batch]
  (doseq [e batch] (tally entry e))
  (when-let [events (seq (remove noise? batch))]
    (swap! entry update :outbox #(let [o (into % events)] (subvec o (max 0 (- (count o) outbox-cap)))))
    (let [{:keys [session id outbox child]} @entry
          {:keys [channel format]} @session]
      (when (and channel (seq outbox))
        (swap! entry assoc :outbox [])
        (hk/send! channel (codec/encode format
                                        (assoc (frames/push (str "scratch-" id) outbox (log/head (:log child)) false
                                                            (dec (:event/seq (first outbox))))
                                               :scratch (str id))))))))

;; ---------------------------------------------------------------- begin, send, end

(defn- child-world
  "The world as of now, in memory: reads the real one up to the fork seq, writes only to itself."
  [world id person]
  (let [idx (:index world)
        db (index/db idx)
        base (index/applied-seq idx)
        clog (log/open-in-memory! (:log world) base)]
    {:config (:config world) :world-id (:world-id world) :sessions (session/registry)
     :log clog :blobs (log/blobs clog) :index (index/at clog (:world-id world) db base)
     :scratch id :scratch-person person}))

(defn- target-of [db person v]
  (or (->uuid v) (when (string? v) (resolve/lookup db person v))))

(defn- route-for
  "A Route the two already share, else the Principal's first."
  [person-state target-state]
  (let [theirs (field target-state :principal/routes)
        mine (set (field person-state :principal/routes))]
    (or (first (filter mine theirs)) (first theirs))))

(defn- begin! [world s person {:keys [principal]}]
  (let [db (act/db world)
        t (target-of db person principal)
        ts (when t (objects/state db t))
        route (when ts (route-for (objects/state db person) ts))]
    (cond
      (not= :principal (:object/class ts)) act/unresolved
      (not (resolve/resolves? db person (act/id-name t) :evaluate)) act/unresolved
      (not= :process (driven-by db t)) (act/err :invalid "an off-the-record session runs under a process-driven Principal")
      (nil? (rloop/first-model db ts)) (act/err :invalid "that Principal has no Model")
      (nil? route) (act/err :invalid "that Principal has no Route")
      :else
      (let [id (random-uuid)
            child (child-world world id person)
            base (log/head (:log child))
            entry (atom {:id id :person person :principal t :route route :session s :child child
                         :started (System/currentTimeMillis) :base base :messages 0 :tokens-in 0 :tokens-out 0
                         :busy false :last-reply nil :outbox []})]
        (log/subscribe! (:log child) ::push (fn [batch] (deliver! entry batch)))
        (swap! (:scratches world) assoc id entry)
        (audit! world :scratch-began person {:scratch id :principal t :forked-at base})
        {:ok {:result {:scratch id :principal t :forked-at base :route route}}}))))

(defn- wake!
  "The Principal's cycle on its own virtual thread, logging nothing."
  [entry message]
  (let [{:keys [child principal route]} @entry
        run (bound-fn []
              (timbre/with-min-level :report
                (try
                  (when-let [reply (:reply (rloop/cycle! child principal {:message message :route route}))]
                    (swap! entry assoc :last-reply reply))
                  (catch Throwable _ nil)
                  (finally (swap! entry assoc :busy false)))))]
    (Thread/startVirtualThread run)))

(defn- send!
  "One line of the session. reasoning (on or off) is the session's own and stays with it until the
   next line changes it, default off: it never reaches the real Principal's loop/reasoning."
  [entry {:keys [text reasoning]}]
  (let [_ (when (#{"on" "off" :on :off} reasoning) (swap! entry assoc :reasoning (keyword (name reasoning))))
        {:keys [child person principal route last-reply]} @entry
        mode (get @entry :reasoning :off)
        [before _] (swap-vals! entry assoc :busy true)]
    (cond
      (str/blank? (str text)) (do (swap! entry assoc :busy (:busy before)) (act/err :invalid "text is required"))
      (:busy before) (act/err :conflict "the Principal is still answering")
      :else
      ;; an exception here must not reach the server's log line: its data can hold the text
      (let [{:keys [ok err]} (timbre/with-min-level :report
                               (try (route/send! child person (cond-> {:to #{principal} :route route :text (str text) :reasoning mode}
                                                                last-reply (assoc :in-reply-to last-reply)))
                                    (catch Throwable _ (act/err :invalid "the session could not take that Message"))))]
        (if err
          (do (swap! entry assoc :busy false) {:err err})
          (do (wake! entry (:message ok))
              {:ok {:result {:scratch (:id @entry) :message (:message ok)}}}))))))

(defn end!
  "Finish a scratch: abort its Model call, forget its log and blobs, write the audit stub. Returns
   what it spent, or nil when it was already gone."
  [world id reason]
  (let [[before _] (swap-vals! (:scratches world) dissoc id)]
    (when-let [entry (get before id)]
      (let [{:keys [person principal child started messages tokens-in tokens-out]} @entry
            spent {:scratch id :principal principal :messages messages :tokens-in tokens-in :tokens-out tokens-out
                   :duration-ms (- (System/currentTimeMillis) started) :reason (name reason)}]
        (evaluate/cancel-scratch! id)
        (log/close-in-memory! (:log child))
        (audit! world :scratch-ended person spent)
        spent))))

(defn- owned
  "The entry for scratch id if this session opened it."
  [world s id]
  (when-let [entry (get @(:scratches world) (->uuid id))]
    (when (= (:id @(:session @entry)) (:id @s)) entry)))

(defn perform!
  "The scratch.* actions, from protocol.ops: a person's session only."
  [ctx p action args]
  (let [world (:world ctx)
        s (:session @(:conn ctx))]
    (if (or (:loop? ctx) (not (authority/person? (act/db world) p)))
      (act/err :session-only "an off-the-record session is opened by a person, from a client")
      (case action
        :scratch.begin (begin! world s p args)
        :scratch.send (if-let [entry (owned world s (:scratch args))] (send! entry args) act/unresolved)
        ;; the model call stops, the session stays: partial text is in what was already pushed
        :scratch.stop (if-let [entry (owned world s (:scratch args))]
                        (let [running? (:busy @entry)]
                          (evaluate/cancel-scratch! (:id @entry))
                          {:ok {:result {:scratch (:id @entry) :stopped (boolean running?)}}})
                        act/unresolved)
        :scratch.end (if (owned world s (:scratch args))
                       {:ok {:result (end! world (->uuid (:scratch args)) :ended)}}
                       act/unresolved)))))

(defn sweep!
  "End scratches past their lifetime, or whose session is gone past its TTL."
  [world]
  (let [now (System/currentTimeMillis)
        max-ms (* 1000 (long (get-in world [:config :scratch-max-s] default-max-s)))]
    (doseq [[id entry] @(:scratches world)
            :let [{:keys [started session]} @entry
                  live (session/lookup (:sessions world) (:id @session))]]
      (cond (> (- now started) max-ms) (end! world id :lifetime)
            (or (nil? live) (session/expired? live now)) (end! world id :session-gone)))))

(defn end-all! [world reason]
  (when-let [r (:scratches world)]
    (doseq [id (keys @r)] (end! world id reason))))
