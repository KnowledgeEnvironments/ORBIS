(ns knowledge.world.loop.steer
  "Steering a running wake (decisions 2026-10-31 'queue and steer'). `send {steer true}` to a
   Principal with a wake running does not queue: the Message (message/kind :steer) joins that wake.
   A Model call in flight is cancelled through the cancel path, as state cancelled with reason
   steer and no stall; ops already run stand; the next iteration starts at once, carrying the text
   at the top of its Serialization, and does not count against loop/max-iterations.

   Rights: transmit on the Route, and the wake's asker, or :evaluate or :write on the running
   Evaluation or the Principal (the cancel rule). Otherwise err not-allowed, so a client can offer
   to queue instead. With no wake running a steer is an ordinary Message, `steered false`. When the
   wake ends before the steer lands it becomes the Principal's next wake (the lane does that, from
   queue/end!). A cancel beats a steer: evaluate/interrupt! only takes an unclaimed reason."
  (:require [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.loop.queue :as queue]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]))

(defn- f [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- allowed?
  "May p steer the wake `active` of target?"
  [db p target {:keys [message evaluation]}]
  (or (= p (f db message :message/from))
      (= p (f db message :message/asked-by))
      (some (fn [object]
              (and object (or (resolve/resolves? db p (act/id-name object) :evaluate)
                              (resolve/resolves? db p (act/id-name object) :write))))
            [evaluation target])))

(defn- interrupt-soon!
  "Cancel the Model call for steering. The Evaluation may have been created a moment before its
   call is in the running set, so try for a little while, for as long as it is still the wake's."
  [world target evaluation by]
  (future
    (loop [n 150]
      (let [r (evaluate/interrupt! evaluation by :steer)]
        (when (and (nil? r) (pos? n) (= evaluation (:evaluation (queue/active world target))))
          (Thread/sleep 20)
          (recur (dec n)))))))

(defn settle!
  "After an Evaluation starts: steering that landed while it was being prepared interrupts it."
  [world p evaluation]
  (when (queue/pending? world p)
    (interrupt-soon! world p evaluation (:from (peek (:pending (queue/active world p)))))))

(defn applied!
  "message/steer-applied on each steer an Evaluation carried, so a client can show it landed."
  [world evaluation steers]
  (doseq [{:keys [message]} steers]
    (log/append!! (:log world) (log/pending :field-set message (:world-id world)
                                            {:name :message/steer-applied :type [:ref :evaluation]
                                             :value evaluation :reader :both}
                                            :via evaluation))))

(defn cancel-wake!
  "A cancel that found no Model call in flight, with a wake running on `object` (a Principal, or
   an Evaluation a steer just interrupted): the wake ends at its next turn. Nil when there is none."
  [world p object]
  (let [db (act/db world)
        s (objects/state db object)
        target (case (:object/class s)
                 :principal object
                 :evaluation (when (= :cancelled (f db object :evaluation/state)) (f db object :evaluation/initiator))
                 nil)]
    (when (and target (queue/cancel-wake! world target p))
      {:ok {:result {:principal target :state "cancelling"} :object object}})))

(defn send!
  "The wire op send with steer true. Returns route/send!'s answer with :steered, or {:err}."
  [world p {:keys [to route] :as args}]
  (let [db (act/db world)
        to (set to)
        target (first to)
        active (when (= 1 (count to)) (queue/active world target))
        ordinary #(let [r (route/send! world p args)]
                    (cond-> r (:ok r) (assoc-in [:ok :steered] false)))]
    (cond
      (not= 1 (count to)) (act/err :invalid "a steer names one recipient")
      (= p target) (act/err :not-allowed "a Principal does not steer itself")
      (:message args)
      (let [message (:message args)
            original (some #(when (= message (:message %)) %) (queue/waiting world target))]
        (cond
          (nil? original) (act/err :conflict "that Message is not waiting")
          (not= p (:from original)) (act/err :not-allowed "only the original sender may promote a queued Message")
          (not (resolve/resolves? db p (act/id-name message) :read)) (act/err :unresolved "Message does not resolve")
          (not (resolve/resolves? db p (act/id-name route) :transmit)) (act/err :unresolved "Route does not resolve")
          (not= route (:route original)) (act/err :invalid "use the original Message's Route")
          (and active (not (allowed? db p target active))) (act/err :not-allowed "you may not steer this wake")
          :else
          (if-let [{:keys [steered active]} (queue/promote! world db target message)]
            (do (when-let [e (and steered (:evaluation active))] (interrupt-soon! world target e p))
                (when-not steered ((:pump! @(:loop-hooks world)) target))
                {:ok {:message message :steered steered}})
            (act/err :conflict "that Message has already started"))))
      (some #(and (not= p (:from %)) (= (:text args) (f db (:message %) :message/text)))
            (queue/waiting world target))
      (act/err :not-allowed "steering must not copy another sender's queued Message")
      (nil? active) (ordinary)
      (not (allowed? db p target active))
      (act/err :not-allowed "you neither asked this wake nor can evaluate or write its Principal; send without steer to queue")
      :else
      ;; in the wake's conversation, so the thread shows who changed course and when
      (let [r (route/send! world p (assoc args :kind :steer :thread (f db (:message active) :message/thread)))]
        (if-let [{:keys [message seq]} (:ok r)]
          (if-let [now (queue/attach-steer! world target {:message message :seq seq :from p :route route})]
            (do (when-let [e (:evaluation now)] (interrupt-soon! world target e p))
                (assoc-in r [:ok :steered] true))
            ;; the wake ended between the look and the send: this is the next wake
            (let [db (act/db world)]
              (queue/enqueue! world db {:principal target :message message :route route} :head? true)
              ((:pump! @(:loop-hooks world)) target)
              (-> r (assoc-in [:ok :steered] false) (assoc-in [:ok :next-wake] true))))
          r)))))
