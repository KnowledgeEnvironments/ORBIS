(ns knowledge.world.loop.queue
  "The queue in front of a Principal's lane (decisions 2026-10-31 'queue and steer'). A wake waits
   here until its Principal's previous wake has ended; the lane only says when to take the next one,
   so the order is data a person can see and change. loop/queue on the Principal is the record:
   {:message :from :at :kind :wake}, ids and the instant as strings, in lane order, the world as
   actor like every other bookkeeping Field. A wake that starts at once never shows up there, so an
   idle Principal adds nothing to the log; a wake that has to wait does, and that Field is what
   brings waiting wakes back after a restart.

   Also here, because it shares the atom and its atomic steps: :active, what the lane is doing now:
   the entry plus :pending and :applied steering, :evaluation (the Model call in flight) and
   :cancelled-by. This is process state; the log holds what came of it."
  (:require [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log])
  (:import [java.time Instant]))

;; a scratch session's child world has no lanes and no queue: its cycle reads an empty state
(defn- state [world] (or (:loop-state world) (atom {})))

(defn- f [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- field-entry [{:keys [message from at kind wake]}]
  (cond-> {:message (str message) :from (str from) :at at :kind kind}
    wake (assoc :wake (str wake))))

(defn- sync!
  "Write loop/queue when it changed. The one case skipped is a lone wake about to start on an idle
   lane: nothing is waiting, and two Field writes per wake would be noise."
  [world p db]
  (locking (state world)
    (let [{:keys [items active written]} (get @(state world) p)
          v (mapv field-entry items)
          about-to-start? (and (empty? written) (nil? active) (= 1 (count items)))]
      (when (and (not about-to-start?) (not= v (vec written)))
        (log/append!! (:log world) (log/pending :field-set p (:world-id world)
                                                {:name :loop/queue :type [:list :map] :value v :reader :both}))
        (swap! (state world) assoc-in [p :written] v)))))

(defn enqueue!
  "Queue a wake {:principal :message :route :wake}, last in line, or first with :head? (a steer that
   arrived as the wake ended is the very next thing the Principal does). Returns the entry."
  [world db {:keys [principal message route wake]} & {:keys [head?]}]
  (let [e {:message message :route route :wake wake
           :from (f db message :message/from)
           :at (str (Instant/now))
           :kind (some-> (or (f db message :message/kind) :message) name)}]
    (swap! (state world) update-in [principal :items]
           (fn [items] (if head? (into [e] items) (conj (vec items) e))))
    (sync! world principal db)
    e))

(defn take-next!
  "Take the next wake and mark the lane busy with it, or nil when nothing waits (it was dequeued)."
  [world db p]
  (let [[old _] (swap-vals! (state world)
                            (fn [s]
                              (if-let [e (first (get-in s [p :items]))]
                                (-> s
                                    (update-in [p :items] (comp vec rest))
                                    (assoc-in [p :active] (assoc e :pending [] :applied [])))
                                s)))
        e (first (get-in old [p :items]))]
    (when e (sync! world p db) (assoc e :principal p))))

(defn end!
  "The lane is free. Returns the steering that never landed, which becomes the next wake, unless
   the wake was cancelled: a cancel beats a steer."
  [world db p]
  (let [[old _] (swap-vals! (state world) update p dissoc :active)
        a (get-in old [p :active])]
    (sync! world p db)
    (when-not (:cancelled-by a) (:pending a))))

(defn dequeue!
  "Remove a waiting wake. True when it was waiting."
  [world db p message]
  (let [[old new] (swap-vals! (state world) update-in [p :items]
                              (fn [items] (vec (remove #(= message (:message %)) items))))
        hit? (not= (count (get-in old [p :items])) (count (get-in new [p :items])))]
    (when hit? (sync! world p db))
    hit?))

(defn reorder!
  "Move a waiting wake to position (0 is next). True when it was waiting."
  [world db p message position]
  (let [move (fn [items]
               (if-let [e (some #(when (= message (:message %)) %) items)]
                 (let [rest-items (vec (remove #(= message (:message %)) items))
                       at (-> (long position) (max 0) (min (count rest-items)))]
                   (vec (concat (subvec rest-items 0 at) [e] (subvec rest-items at))))
                 items))
        [old new] (swap-vals! (state world) update-in [p :items] move)
        hit? (some #(= message (:message %)) (get-in old [p :items]))]
    (when (and hit? (not= (get-in old [p :items]) (get-in new [p :items]))) (sync! world p db))
    (boolean hit?)))

(defn waiting
  "The wakes waiting for p, in order."
  [world p]
  (vec (get-in @(state world) [p :items])))

(defn restore!
  "Waiting wakes back from loop/queue at boot, in order; a Message that no longer exists is dropped.
   Returns [[p entry] ...], one per wake, for the lanes to pick up."
  [world db]
  (vec (for [p (sort-by str (objects/objects-with-field db :loop/queue))
             :let [v (vec (f db p :loop/queue))]
             :when (seq v)
             :let [items (vec (for [{:keys [message from at kind wake]} v
                                    :let [m (parse-uuid (str message))]
                                    :when (and m (objects/exists? db m))]
                                {:message m :from (parse-uuid (str from)) :at at :kind kind
                                 :route (f db m :message/route)
                                 :wake (some-> wake str parse-uuid)}))
                   _ (swap! (state world) assoc p {:items items :written v})
                   _ (sync! world p db)]
             e items]
         [p e])))

;; ---------------------------------------------------------------- the running wake

(defn active [world p] (get-in @(state world) [p :active]))

(defn attach-steer!
  "Add steering to the running wake. Returns the active wake, or nil when there was none: the
   compare is the atomic step that decides between joining and being the next wake."
  [world p steer]
  (let [[old new] (swap-vals! (state world)
                              (fn [s] (if (get-in s [p :active])
                                        (update-in s [p :active :pending] conj steer)
                                        s)))]
    (when (get-in old [p :active]) (get-in new [p :active]))))

(defn take-steers!
  "Move what is pending to applied. {:new [...] :all [...]}, in arrival order."
  [world p]
  (let [[old _] (swap-vals! (state world)
                            (fn [s]
                              (if (get-in s [p :active])
                                (update-in s [p :active]
                                           (fn [a] (-> a
                                                       (update :applied into (:pending a))
                                                       (assoc :pending []))))
                                s)))
        a (get-in old [p :active])]
    {:new (vec (:pending a)) :all (into (vec (:applied a)) (:pending a))}))

(defn pending? [world p] (boolean (seq (get-in @(state world) [p :active :pending]))))

(defn running! [world p evaluation]
  (swap! (state world) (fn [s] (if (get-in s [p :active])
                                 (assoc-in s [p :active :evaluation] evaluation)
                                 s)))
  nil)

(defn cancel-wake!
  "End the running wake at its next turn: a cancel that found no Model call in flight. True when
   there was a wake to end."
  [world p by]
  (let [[old _] (swap-vals! (state world)
                            (fn [s] (if (get-in s [p :active])
                                      (assoc-in s [p :active :cancelled-by] by)
                                      s)))]
    (boolean (get-in old [p :active]))))

(defn wake-cancelled [world p] (get-in @(state world) [p :active :cancelled-by]))

(defn promote!
  "Atomically move a waiting Message into steering, or to the head if idle."
  [world db p message]
  (let [[old new] (swap-vals! (state world)
                             (fn [s]
                               (if-let [e (some #(when (= message (:message %)) %) (get-in s [p :items]))]
                                 (let [items (vec (remove #(= message (:message %)) (get-in s [p :items])))]
                                   (if (get-in s [p :active])
                                     (-> s (assoc-in [p :items] items)
                                         (update-in [p :active :pending] conj e))
                                     (assoc-in s [p :items] (into [e] items))))
                                 s)))]
    (when (some #(= message (:message %)) (get-in old [p :items]))
      (sync! world p db)
      {:steered (some? (get-in old [p :active])) :active (get-in new [p :active])})))
