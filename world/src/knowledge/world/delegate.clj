(ns knowledge.world.delegate
  "Short-lived subagents, and work handed to them (decisions 2026-10-31).

   principal/ephemeral on a Principal is {for until parent}: the Task it exists for, if any, when it
   ends at the latest, and who made it. Only the kernel writes it. Such a Principal is retired when
   its Task ends, when until passes or when its parent goes, whichever is first; its Grants go with
   it and its record stays on the log. It cannot outlive its parent, and its Grants are never
   delegable, so it cannot outrank its parent or make anything that lasts.

   A delegated Task separates what a parent must hear from what it may read:
     task/progress   written by the child with `task.progress`, wakes nobody
     the final reply the result, stored in task/result; it wakes the parent
     `task.ask`      a question; it wakes the parent and holds the Task open
     task/due        the deadline; a parent hears once when it passes

   The sweep runs on the scheduler's thread. All of it reads the log's fold, so a restarted world
   carries on where it was."
  (:require [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route])
  (:import [java.util Date]))

(def default-ttl-s 3600)
(def max-ttl-s 86400)
(def default-deadline-s 900)
(def max-result-chars 4000)
(def max-progress-chars 500)

(def ^:private terminal #{:done :failed :cancelled})

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- kw [v] (cond (keyword? v) v (string? v) (keyword v) :else nil))
(defn- clip [s n] (let [s (str s)] (if (> (count s) n) (subs s 0 n) s)))

(defn state
  "principal/ephemeral of p, or nil for a Principal that lasts."
  [db p]
  (field db p :principal/ephemeral))

(defn parent-of [db p] (:parent (state db p)))
(defn task-of [db p] (:for (state db p)))
(defn child-of? [db parent p] (and parent p (= parent (parent-of db p))))

(defn- open-task?
  [db task]
  (and task (not (terminal (kw (field db task :task/state))))))

(defn until-for
  "When a Principal made now by parent ends at the latest: ttl-s from now, at most max-ttl-s, and never
   after the parent's own end."
  ^Date [db parent ttl-s ^Date now]
  (let [ttl (min max-ttl-s (max 1 (long (or ttl-s default-ttl-s))))
        mine (Date. (+ (.getTime now) (* 1000 ttl)))
        theirs (:until (state db parent))]
    (if (and (inst? theirs) (.before ^Date theirs mine)) theirs mine)))

(defn- note!
  "A Field written by the kernel, the world as actor."
  ([world object k type v] (note! world object k type v nil))
  ([world object k type v via]
   (log/append!! (:log world) (log/pending :field-set object (:world-id world)
                                           {:name k :type type :value v :reader :both} :via via))))

(defn mark!
  "p is short-lived: {:for :until :parent}. supervisor is its stall Trigger, retired with it."
  [world p {:keys [for until parent supervisor route]}]
  (note! world p :principal/ephemeral :map
         (cond-> {:until until :parent parent}
           for (assoc :for for)
           supervisor (assoc :supervisor supervisor)
           route (assoc :route route))))

(defn- move!
  "The Task's state, the world as actor: the child may be gone."
  [world task to]
  (let [db (act/db world)
        owner (field db task :task/owner)]
    (log/append!! (:log world) (log/pending :task-moved task (:world-id world)
                                            {:task task :from (or (kw (field db task :task/state)) :open) :to to}
                                            :streams (if owner #{owner} #{})))))

(defn- end-task!
  [world task to result]
  (when (open-task? (act/db world) task)
    (note! world task :task/result :map result)
    (move! world task to)))

(defn waiting?
  "Did this wake of p ask its parent something? Then its final text is not the result."
  [db p message]
  (when-let [task (task-of db p)]
    (and message (= message (field db task :task/waiting)))))

(defn result!
  "p's final reply of this wake, the result of its Task. A wake that asked its parent leaves it open."
  [world p text message]
  (let [db (act/db world)
        task (task-of db p)]
    (when (and task (open-task? db task) (not (waiting? db p message)))
      (end-task! world task :done {:text (clip text max-result-chars) :by (str p)}))))

(defn progress!
  "task/progress on a Task p may write, read by anyone who can read the Task. It wakes nobody."
  [world p task text]
  (let [db (act/db world)]
    (if-let [{:keys [grant]} (when (and task (objects/exists? db task))
                               (resolve/resolve! world db p (act/id-name task) :write))]
      (do (log/append!! (:log world) (log/pending :field-set task p {:name :task/progress :type :string
                                                                     :value (clip text max-progress-chars) :reader :both}
                                                  :delegation grant))
          {:ok {:result {:task task :progress true}}})
      act/unresolved)))

(defn ask!
  "A question from p to its parent, a Message that wakes it. The Task stays open, and this wake ends
   with no reply: the answer is the next wake."
  [world p question {:keys [via message hop]}]
  (let [db (act/db world)
        {:keys [parent route for]} (state db p)]
    (if (and parent route for (open-task? db for))
      (let [sent (route/send! world p {:to #{parent} :route route :text (str question) :via via
                                       :hop (or hop 1) :kind :request})]
        (or (when (:err sent) sent)
            (do (when message (note! world for :task/waiting :ref message))
                {:ok {:result {:asked true :task for :message (get-in sent [:ok :message])}}})))
      (act/err :conflict "only a subagent with an open Task asks its parent"))))

(defn- tell-parent!
  "Something p's parent should hear, on p's Route, while p can still speak."
  [world p text]
  (let [db (act/db world)
        {:keys [parent route for]} (state db p)]
    (when (and parent route (not (objects/retired? db parent)))
      (route/send! world p {:to #{parent} :route route :text text :via for :hop 0 :kind :status}))))

(defn- retire-object!
  "One Object out of the world's lists: names unbound, `retired` on the log. Its history stays."
  [world id reason]
  (let [db (act/db world)]
    (when (and (objects/exists? db id) (not (objects/retired? db id)))
      (let [names (vec (sort (objects/names db id)))
            actor (:world-id world)]
        (log/append!! (:log world) (log/pending :field-set id actor {:name :object/last-names :type [:list :string]
                                                                     :value (mapv second names) :reader :both}))
        (doseq [[ns n] names]
          (log/append!! (:log world) (log/pending :unbound ns actor {:namespace ns :name n :object id} :streams #{id})))
        (log/append!! (:log world) (log/pending :retired id actor {:names names :reason reason}))))))

(defn- drop-route!
  "The Route goes from every Principal that held it, or a retired Route stays on their lists."
  [world route]
  (let [db (act/db world)]
    (doseq [p (objects/objects-with-field db :principal/routes)
            :let [have (field db p :principal/routes)]
            :when (some #{route} have)]
      (note! world p :principal/routes [:list [:ref :route]] (vec (remove #{route} have))))))

(defn- revoke-all!
  "Every Grant p holds, revoked by the world: an expired Grant must not come back with a restore."
  [world p reason]
  (let [db (act/db world)]
    (doseq [g (resolve/active-grants db #{p} (Date.))
            :let [id (:object/id g)]]
      (log/append!! (:log world) (log/pending :revoked id (:world-id world) {:grant id :reason reason}
                                              :streams #{(:grant/grantor g) p})))))

(defn retire!
  "p is gone from every list. Its Grants go with it; its Route and stall Trigger have no other use."
  [world p reason]
  (let [db (act/db world)
        {:keys [route supervisor]} (state db p)]
    (revoke-all! world p reason)
    (when supervisor
      (note! world supervisor :trigger/armed :boolean false)
      (retire-object! world supervisor reason))
    (when route
      (drop-route! world route)
      (retire-object! world route reason))
    (retire-object! world p reason)))

(defn- fail!
  "The Task ended without a result, and p's parent hears why, once."
  [world p task reason]
  (when (open-task? (act/db world) task)
    (let [db (act/db world)
          progress (field db task :task/progress)]
      (tell-parent! world p (str "Task \"" (field db task :task/title) "\" failed: " reason
                                 (when (seq (str progress)) (str " Last progress: " progress))))
      (end-task! world task :failed {:reason reason :by (str p)}))))

(defn- overdue!
  "The deadline passed with the Task open: the parent hears once, and may let it run."
  [world p task]
  (let [db (act/db world)]
    (note! world task :task/overdue-told :boolean true)
    (tell-parent! world p (str "Task \"" (field db task :task/title) "\" passed its deadline and is still open."
                               (when-let [m (field db task :task/progress)] (str " Last progress: " m))))))

(defn- idle? [db p] (zero? (long (or (field db p :loop/iteration) 0))))

(defn sweep!
  "Every short-lived Principal, against the clock and its Task. Returns those retired."
  [world ^Date now]
  (let [db (act/db world)]
    (vec
     (for [p (objects/objects-with-field db :principal/ephemeral)
           :when (not (objects/retired? db p))
           :let [{:keys [for until parent]} (state db p)
                 stalled (str (field db p :loop/stalled))
                 due (when for (field db for :task/due))
                 ended? (and for (not (open-task? db for)))
                 orphan? (and parent (objects/retired? db parent))
                 expired? (and (inst? until) (not (.after ^Date until now)))
                 failed? (and for (seq stalled) (idle? db p))
                 reason (cond ended? "its Task ended"
                              orphan? "its parent is gone"
                              expired? "its time ran out"
                              failed? stalled)]
           ;; the deadline is news even when nothing ends
           :let [_ (when (and (nil? reason) (inst? due) (not (.after ^Date due now))
                              (not (true? (field db for :task/overdue-told))))
                     (overdue! world p for))]
           :when reason]
       (do (when (and for (not ended?)) (fail! world p for reason))
           (retire! world p reason)
           p)))))

(defn charter
  "What a subagent is told it is."
  [nm parent-name task-text]
  (str "You are " nm ", a temporary subagent of " parent-name " for one Task. Do it and stop.\n"
       "Your final text is the result. It goes back to " parent-name ", and you are retired when it lands, "
       "so put everything worth keeping in it. If you cannot finish, say why as your final text.\n"
       "`progress` records how it is going and wakes nobody. `ask` puts a question to " parent-name
       "; only ask when you cannot go on.\n\n"
       "The Task: " (str/trim (str task-text))))
