(ns knowledge.world.notify
  "A word to a person from any Principal (decisions 2026-10-31). `notify {level text object?}` needs no
   Route: a Message needs one, and a Principal must be able to ask for attention before anyone has
   joined it to one. It reaches the person(s) who own the sender, by the approvers walk.

   A notification is an Object in the world's notifications collection, its events on the collection's
   stream, so a client follows one stream. It carries no authority: nothing reads it back into a prompt,
   it wakes no loop, it grants nothing, and it can name an Object only if the sender can read it; the
   recipient gets no access to that Object from it. Read state is per person, on the notification.

   The limit lives in memory, per Principal: a loop cannot drown a person, and what it drops is
   counted and arrives as one summary. A restart forgets the window, which costs at most one window."
  (:require [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve])
  (:import [java.util Date]))

(def collection-field :world/notifications)
(def text-cap 500)
(def per-window 6)
(def window-ms 60000)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- now-ms ^long [world] (long (if-let [f (:notify-clock world)] (f) (System/currentTimeMillis))))

(defn collection [db] (field db (objects/world-id db) collection-field))

(defn- clip [s] (let [s (str s)] (if (> (count s) text-cap) (str (subs s 0 text-cap) "…") s)))

(defn- store!
  "The notification Object, on the collection's stream. Actor p; the collection's Grant carries it, the
   way a recipient reads a Message under the Grant it was sent with."
  [world p to {:keys [level text object dropped]}]
  (let [db (act/db world)
        coll (collection db)
        id (random-uuid)
        fields (cond-> {:notification/to (vec to) :notification/from p
                        :notification/level (str level) :notification/text (clip text)
                        :notification/at (str (.toInstant (Date.)))}
                 object (assoc :notification/object object)
                 dropped (assoc :notification/dropped dropped))
        seq-no (act/append! world (log/pending :created id p {:class :object :fields fields}
                                               :streams #{coll}
                                               :delegation (:object/created-under (objects/entity db coll))))]
    {:notification id :seq seq-no}))

(defn- admit
  "Counts one call in p's window. true when it may go."
  [state p now]
  (let [{:keys [since n] :or {n 0}} (get state p)
        fresh? (or (nil? since) (>= (- now since) window-ms))
        n' (if fresh? 1 (inc n))
        over? (> n' per-window)]
    (update state p (fn [s] (cond-> (assoc s :since (if fresh? now since) :n n' :over over?)
                              over? (update :dropped (fnil inc 0)))))))

(defn send!
  "One notification from p. Answers {:ok {:result ... :seq}} or {:err}."
  [world p {:keys [level text object]}]
  (let [db (act/db world)
        target (when object (or (->uuid object) (resolve/lookup db p (str object))))]
    (cond
      (nil? (collection db)) (act/err :invalid "this world has no notifications collection yet")
      (str/blank? (str text)) (act/err :invalid-args "text is empty")
      (and object (not (and target (resolve/resolves? db p (str "/" target) :read)))) act/unresolved
      :else
      (let [to (gates/approvers db p nil)
            over? (:over (get (swap! (:notify-state world) admit p (now-ms world)) p))]
        (cond
          (empty? to) (act/err :invalid "no person in this world to tell")
          over? {:ok {:result {:dropped true :note (str "over " per-window " a minute; they are counted and sent as one summary")}}}
          :else (let [{:keys [notification seq]} (store! world p to {:level (or level "info") :text text :object target})]
                  {:ok {:result {:notification notification :to (vec to)} :object notification :seq seq}}))))))

(defn flush!
  "The summary of what each Principal dropped, once its window has passed. Returns the Principals told."
  [world]
  (let [now (now-ms world)
        due (for [[p {:keys [since dropped]}] @(:notify-state world)
                  :when (and dropped (pos? dropped) (>= (- now since) window-ms))]
              [p dropped])]
    (vec (for [[p dropped] due
               :let [db (act/db world) to (gates/approvers db p nil)]
               :when (seq to)]
           (do (swap! (:notify-state world) update p dissoc :dropped)
               (store! world p to {:level "attention" :dropped dropped
                                   :text (str dropped " notification" (when (not= 1 dropped) "s") " dropped: over the limit")})
               p)))))

(defn- addressed [db p]
  (->> (objects/objects-with-field db :notification/to)
       (filter #(contains? (set (field db % :notification/to)) p))
       (sort-by #(- (long (or (:object/created-seq (objects/entity db %)) 0))))))

(defn- read-by? [db id p] (contains? (set (field db id :notification/read-by)) p))

(defn list-of
  "p's notifications, newest first."
  [world p {:keys [unread limit]}]
  (let [db (act/db world)
        mine (addressed db p)
        rows (cond->> mine unread (remove #(read-by? db % p)))
        row (fn [id] (cond-> {:notification id :level (field db id :notification/level) :text (field db id :notification/text)
                              :from (field db id :notification/from) :at (field db id :notification/at)
                              :read (read-by? db id p)}
                       (field db id :notification/object) (assoc :object (field db id :notification/object))
                       (field db id :notification/dropped) (assoc :dropped (field db id :notification/dropped))))]
    {:ok {:result {:collection (collection db)
                   :unread (count (remove #(read-by? db % p) mine))
                   :rows (mapv row (take (long (or limit 50)) rows))}}}))

(defn mark-read!
  "Marks p's own notifications read: the ids named, or all of them. Someone else's are left alone."
  [world p {:keys [notifications]}]
  (let [db (act/db world)
        mine (set (addressed db p))
        ids (if (seq notifications) (filter mine (keep ->uuid notifications)) mine)
        coll (collection db)
        todo (remove #(read-by? db % p) ids)
        seqs (vec (for [id todo]
                    (act/append! world (log/pending :field-set id p
                                                    {:name :notification/read-by :type [:list [:ref :principal]]
                                                     :value (vec (conj (vec (field db id :notification/read-by)) p)) :reader :both}
                                                    :streams #{coll}
                                                    :delegation (:object/created-under (objects/entity db id))))))]
    {:ok {:result {:read (count seqs)} :seq (peek seqs)}}))
