(ns knowledge.world.loop.resume
  "A person must never lose a message to a restart. loop/queue brings back wakes that were waiting;
   this brings back the ones a stopped process left unfinished, read from the log at boot:

   - an Evaluation left queued, running or streaming ends failed with reason restart, and its
     Message goes back to the front of the lane;
   - a Principal whose loop/iteration is above zero was mid-wake (between model calls, or running
     ops) and its loop/current goes back too;
   - a person's Message delivered in the last hour to a Principal that never started on it, which
     a crash between delivery and the lane can leave.

   An answered Message, a retired Principal and a dequeued Message are left alone, and a Message
   that has already been resumed resume-limit times is not resumed again, so a wake that takes the
   world down cannot do so for ever. Ops that ran before the stop stand; the repeat rule keeps a
   resumed wake from running them twice (decisions 2026-10-31)."
  (:require [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.authority :as authority]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.loop.queue :as queue]
            [knowledge.world.thread :as thread]
            [taoensso.timbre :as timbre])
  (:import [java.util Date]))

(def resume-limit 2)
(def unstarted-window-ms (* 60 60 1000))
(def ^:private unfinished #{:queued :running :streaming})
;; a Message the lane never saw: these kinds do not start a wake on their own
(def ^:private not-a-wake #{:directive :trigger :approval-result :approval :status})
;; a Message the loop had already started on is a wake whatever its kind, except a Trigger's, which
;; fires again by itself. The card path of a kernel swap is the case: the wake that asked ends at
;; approval-pending, the person's approval-result starts the wake that carries on, and the restart it
;; approved kills that one (bootstrap test, 2026-11-10: root stayed silent on its own new release)
(def ^:private never-resumed #{:trigger :status})

(defn- f [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- kw [v] (some-> v name keyword))

(defn- orphans
  "Evaluations nothing is running any more, oldest first."
  [db]
  (->> (objects/objects-with-field db :evaluation/state)
       (filter #(unfinished (kw (f db % :evaluation/state))))
       (sort-by #(:object/created-seq (objects/entity db %)))))

(defn- mid-wake
  "Principals whose wake never reached its end: [p message] from loop/iteration and loop/current."
  [db]
  (for [p (objects/objects-with-field db :loop/iteration)
        :when (pos? (long (or (f db p :loop/iteration) 0)))
        :let [m (f db p :loop/current)]
        :when m]
    [p m]))

(defn- recent? [db m now]
  (let [^Date at (get-in (objects/fields db m) [:message/text :field/set-at])]
    (and at (< (- now (.getTime at)) unstarted-window-ms))))

(defn- unstarted
  "A person's recent Messages to a process Principal on one of its Routes that no Evaluation names."
  [db]
  (let [now (System/currentTimeMillis)
        started (set (keep #(f db % :evaluation/message) (objects/objects-with-field db :evaluation/message)))]
    (for [m (objects/objects-with-field db :message/from)
          :when (and (not (started m)) (recent? db m now))
          :when (not (not-a-wake (kw (f db m :message/kind))))
          :when (authority/person? db (f db m :message/from))
          p (f db m :message/to)
          :let [state (objects/state db p)]
          :when (and (= :process (:principal/driven-by state))
                     (contains? (set (get-in state [:object/fields :principal/routes :field/value]))
                                (f db m :message/route)))]
      [p m])))

(defn- resumable?
  [db [p m]]
  (and (objects/exists? db m)
       (objects/exists? db p)
       (not (objects/retired? db p))
       (not (f db m :message/dequeued))
       (not (never-resumed (kw (f db m :message/kind))))
       (not (thread/answered-by? db m p))
       (< (long (or (f db m :message/resumed) 0)) resume-limit)))

(defn- clear-iteration!
  "The wake is over one way or another: loop/iteration stops claiming otherwise."
  [world p]
  (log/append!! (:log world) (log/pending :field-set p (:world-id world)
                                          {:name :loop/iteration :type :number :value 0 :reader :both})))

(defn- count-resume! [world db m]
  (log/append!! (:log world) (log/pending :field-set m (:world-id world)
                                          {:name :message/resumed :type :number
                                           :value (inc (long (or (f db m :message/resumed) 0))) :reader :both})))

(defn resume!
  "End what a stopped process left unfinished and put its wakes back at the front of their lanes, in
   the order they were started. Returns [[p entry] ...] for the lanes to pick up."
  [world db]
  (let [dead (vec (orphans db))
        from-evaluations (for [e dead :let [m (f db e :evaluation/message)] :when m]
                           [(f db e :evaluation/initiator) m])
        wakes (->> (concat from-evaluations (mid-wake db) (unstarted db))
                   distinct
                   (filter #(resumable? db %))
                   vec)
        waiting (set (for [[p _] wakes e (queue/waiting world p)] [p (:message e)]))]
    (doseq [e dead]
      (timbre/info "evaluation left by a stopped process" {:evaluation e})
      (evaluate/orphaned! world e))
    (doseq [[p _] (mid-wake db)] (clear-iteration! world p))
    (vec (for [[p m] (reverse wakes)
               :when (not (waiting [p m]))]
           (let [delivered (first (filter #(= m (f db % :evaluation/message)) dead))
                 e (queue/enqueue! world db {:principal p :message m :route (f db m :message/route)
                                             :wake (some->> delivered (#(f db % :evaluation/wake)))}
                                   :head? true)]
             (count-resume! world db m)
             (timbre/info "interrupted wake resumed" {:principal p :message m})
             [p e])))))
