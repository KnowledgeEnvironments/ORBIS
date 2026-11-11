(ns knowledge.world.act.unsafe
  "Unsafe mode (decisions 2026-10-31): a person lets one Principal's gated ops run without a card, for
   a bounded time, and the record stays. policy/unsafe on the Principal is {on until except by at}: it
   is the only place the mode lives, it belongs to that Principal alone, and nothing here widens it.
   world/unsafe on the world is derived from those Fields, so a client can show every armed Principal
   from one subscription; the gate never reads it.

   A Field that fails any check here counts as off: `by` must be a person, `at` must have happened,
   and the span must fit max-minutes, so a forged or stale value fails towards a card. Arming and
   ending are ordinary field-set events on the Principal; the arming person is the actor, the world
   when time ran out."
  (:require [clojure.string :as str]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.rules :as rules]
            [knowledge.world.adapters.confine :as confine]
            [knowledge.world.adapters.ssh :as ssh]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve])
  (:import [java.time Duration Instant]
           [java.util Date]))

(def field-key authority/unsafe-field)
(def index-key authority/unsafe-index)
(def rule-id "What task/rule says on a Task this mode admitted." "unsafe")
(def max-minutes authority/max-minutes)
(def default-except ["grant" "object.retire" "principal.create" "project.add" "delegate" "gateway.*" "token.*" "policy/*"])

(defonce ^:private writes
  ;; world/unsafe is recomputed after each change; the session and the sweep must not cross
  (Object.))

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))

(defn state
  "The policy/unsafe value of p, or nil."
  [db p]
  (let [v (field db p field-key)] (when (map? v) v)))

(defn live?
  "Is p in unsafe mode at now?"
  [db p ^Date now]
  (let [s (state db p)]
    (boolean (and s (true? (:on s))
                  (not (objects/retired? db p))
                  (authority/person? db (->uuid (:by s)))
                  (authority/in-time? s now)))))

(defn- fits? [pattern word]
  (let [p (str pattern)]
    (if (str/ends-with? p "*")
      (str/starts-with? word (subs p 0 (dec (count p))))
      (rules/word-fits? p word))))

(defn excepted?
  "Does the except list of state s name any of these words? A pattern ending in * is a prefix."
  [s words]
  (boolean (some (fn [pattern] (some #(fits? pattern %) words)) (:except s))))

;; ---------------------------------------------------------------- an Adapter out of its box

(def confined-key
  "On an Adapter, while a person has lifted confinement: {:on false until by at}. Absent means confined,
   and a value that fails any check counts as absent, so a forged or stale one fails towards the box."
  :adapter/confined)

(defn unconfined?
  "Has a person lifted a's confinement, and is that still in time?"
  [db a ^Date now]
  (let [s (field db a confined-key)]
    (boolean (and (map? s) (false? (:on s)) (authority/person? db (->uuid (:by s))) (authority/in-time? s now)))))

(defn box-for
  "How adapter's commands run now: {:box spec}, {:box nil} when a person lifted confinement, or {:err}
   when no shell may start at all: this host has no sandbox, or the root is too wide to box."
  ([world adapter] (box-for world adapter nil))
  ([world adapter p]
  (let [db (act/db world)
        root (field db adapter :adapter/locator)]
    (cond
      (unconfined? db adapter (Date.)) {:box nil}
      ;; root's leash: its own shell and no one else's, so a Grant on shell.local carries no way out
      (and p (= p (objects/root-id db)) (true? (field db adapter authority/leash-field))) {:box nil}
      (nil? (confine/primitive))
      {:err {:code :unconfined :message "this host has no sandbox, so no shell starts; a person can lift that with adapter.unconfine"}}
      (confine/too-wide? root)
      {:err {:code :unconfined :message "the Adapter's root holds the home directory, so it cannot be confined"}}
      :else
      {:box (confine/spec {:root root :data-dir (:data-dir world) :adapter adapter
                           :read (field db adapter :adapter/read) :network (field db adapter :adapter/network)
                           :ports (confine/model-ports db)
                           :ssh (when-not (false? (field db adapter :adapter/ssh)) (ssh/for-world (:data-dir world)))})}))))

(defn live-shares
  "The shares of pane that hold at now."
  [db pane ^Date now]
  (let [v (field db pane authority/pane-shares)]
    (filter #(and (map? %) (authority/pane-share db pane (:principal %) now)) (when (sequential? v) v))))

(defn entries
  "world/unsafe at now: every armed Principal, then every Adapter out of its box, then every person's
   terminal an agent shares, which a client shows the same way. `principal` names the one to end."
  [db ^Date now]
  (vec (concat
        (for [p (sort-by str (objects/objects-with-field db field-key))
              :when (live? db p now)
              :let [{:keys [until by]} (state db p)]]
          {:principal (str p) :until (str until) :by (str by)})
        (for [a (sort-by str (objects/objects-with-field db confined-key))
              :when (unconfined? db a now)
              :let [{:keys [until by]} (field db a confined-key)]]
          {:principal (str a) :until (str until) :by (str by) :kind "unconfined"})
        (for [pane (sort-by str (objects/objects-with-field db authority/pane-shares))
              {:keys [principal mode until by]} (live-shares db pane now)]
          {:principal (str pane) :agent (str principal) :mode (str mode) :until (str until) :by (str by)
           :kind "shared-terminal"}))))

(defn- publish!
  "world/unsafe, the world as actor, when it changed."
  [world ^Date now]
  (let [db (act/db world)
        wid (:world-id world)
        v (entries db now)]
    (when (not= v (or (field db wid index-key) []))
      (log/append!! (:log world) (log/pending :field-set wid wid {:name index-key :type [:list :map] :value v :reader :both})))))

(defn- clear!
  [world target actor grant previous]
  (act/append! world (log/pending :field-set target actor
                                  {:name field-key :type :string :value "" :reader :both :cleared true :previous previous}
                                  :delegation grant)))

(defn arm!
  "person arms target for minutes. person's write on target is the caller's to have checked."
  [world person target {:keys [minutes except]}]
  (locking writes
    (let [now (Instant/ofEpochMilli (System/currentTimeMillis))
          value {:on true :until (str (.plus now (Duration/ofMinutes (long minutes))))
                 :except (mapv str (if (some? except) except default-except))
                 :by (str person) :at (str now)}
          {:keys [err ok]} (act/set-field! world person {:object target :field field-key :type :map :value value :reader :both})]
      (if err
        {:err err}
        (do (publish! world (Date/from now))
            {:ok {:result (assoc value :principal target) :object target :seq (:seq ok)}})))))

(defn disarm!
  "person ends target's mode. Already off answers the same, with no event."
  [world person target]
  (locking writes
    (let [db (act/db world)
          prev (state db target)]
      (when prev
        (clear! world target person (:grant (resolve/resolve! world db person (act/id-name target) :write)) prev)
        (publish! world (Date.)))
      {:ok {:result {:principal target :on false} :object target}})))

(defn unconfine!
  "person lifts a's confinement for minutes. The caller has checked who may."
  [world person a {:keys [minutes]}]
  (locking writes
    (let [now (Instant/ofEpochMilli (System/currentTimeMillis))
          value {:on false :until (str (.plus now (Duration/ofMinutes (long minutes)))) :by (str person) :at (str now)}
          seq-no (act/append! world (log/pending :field-set a person {:name confined-key :type :map :value value :reader :both}))]
      (publish! world (Date/from now))
      {:ok {:result (assoc value :adapter a) :object a :seq seq-no}})))

(defn reconfine!
  "a goes back in its box. Already there answers the same, with no event."
  [world person a]
  (locking writes
    (when-let [prev (field (act/db world) a confined-key)]
      (act/append! world (log/pending :field-set a person {:name confined-key :type :string :value "" :reader :both
                                                           :cleared true :previous prev}))
      (publish! world (Date.)))
    {:ok {:result {:adapter a :confined true} :object a}}))

(defn- write-shares!
  [world pane actor shares]
  (let [prev (field (act/db world) pane authority/pane-shares)]
    (act/append! world (log/pending :field-set pane actor
                                    (if (seq shares)
                                      {:name authority/pane-shares :type [:list :map] :value (vec shares) :reader :both}
                                      {:name authority/pane-shares :type :string :value "" :reader :both :cleared true
                                       :previous prev})))))

(defn share!
  "person gives agent a person's terminal for minutes, to read or to type. One share per agent. The
   caller has checked who may."
  [world person pane agent {:keys [mode minutes]}]
  (locking writes
    (let [now (Instant/ofEpochMilli (System/currentTimeMillis))
          entry {:principal (str agent) :mode (str mode) :until (str (.plus now (Duration/ofMinutes (long minutes))))
                 :by (str person) :at (str now)}
          others (remove #(= (str agent) (str (:principal %))) (live-shares (act/db world) pane (Date/from now)))
          seq-no (write-shares! world pane person (conj (vec others) entry))]
      (publish! world (Date/from now))
      {:ok {:result (assoc entry :pane pane) :object pane :seq seq-no}})))

(defn unshare!
  "person ends agent's share of pane, or every share of it when agent is nil. Already gone answers the
   same, with no event."
  [world person pane agent]
  (locking writes
    (let [v (field (act/db world) pane authority/pane-shares)]
      (when (sequential? v)
        (write-shares! world pane person (if agent (remove #(= (str agent) (str (:principal %))) v) []))
        (publish! world (Date.)))
      {:ok {:result {:pane pane :shared false} :object pane}})))

(defn expire!
  "Every policy/unsafe past its time, or no longer valid: cleared, the world as actor. Returns
   [{:principal :by}] so the caller can tell whoever armed each."
  [world ^Date now]
  (locking writes
    (let [db (act/db world)
          due (vec (for [p (sort-by str (objects/objects-with-field db field-key))
                         :let [s (state db p)]
                         :when (and s (not (live? db p now)))]
                     {:principal p :by (->uuid (:by s)) :state s}))]
      (doseq [{:keys [principal state]} due]
        (clear! world principal (:world-id world) nil state))
      (when (seq due) (publish! world now))
      (let [boxed (vec (for [a (sort-by str (objects/objects-with-field db confined-key))
                             :let [s (field db a confined-key)]
                             :when (and (map? s) (not (unconfined? db a now)))]
                         {:principal a :by (->uuid (:by s)) :state s}))]
        (doseq [{:keys [principal state]} boxed]
          (act/append! world (log/pending :field-set principal (:world-id world)
                                          {:name confined-key :type :string :value "" :reader :both :cleared true :previous state})))
        (when (seq boxed) (publish! world now))
        (let [ended (vec (for [pane (sort-by str (objects/objects-with-field db authority/pane-shares))
                               :let [v (field db pane authority/pane-shares)
                                     live (live-shares db pane now)]
                               :when (and (sequential? v) (< (count live) (count v)))]
                           {:principal pane :live live
                            :gone (remove (set live) v)}))]
          (doseq [{:keys [principal live]} ended]
            (write-shares! world principal (:world-id world) live))
          (when (seq ended) (publish! world now))
          (mapv #(dissoc % :state :live :gone)
                (concat due boxed
                        (for [{:keys [principal gone]} ended
                              {:keys [by]} gone]
                          {:principal principal :by (->uuid by)}))))))))
