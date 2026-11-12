(ns knowledge.world.loop.queue-test
  "The queue in front of a Principal's lane (decisions 2026-10-31): visible on loop/queue, in order;
   a waiting wake can be dequeued or reordered; waiting wakes survive a restart. The stub Model is
   paced so a wake lasts long enough for others to queue behind it."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.queue :as queue]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [c id k] (get-in (ws/request!! c :read {:object id}) [:result :object/fields k :field/value]))
(defn- queue-of [c p] (vec (field c p :loop/queue)))
(defn- pace! [root model ms]
  (assert (:ok (ws/request!! root :set-field {:object model :field :model/stub-delta-ms :type :number :value ms :reader :both}))))
(defn- say! [c to route text]
  (let [r (ws/request!! c :send {:to #{to} :route route :text text})]
    (assert (:ok r) (pr-str r))
    (get-in r [:result :message])))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- evaluated-for?
  "Has an Evaluation run for this Message?"
  [world message]
  (let [db (act/db world)]
    (some #(= message (get-in (objects/fields db %) [:evaluation/message :field/value]))
          (objects/objects-with-field db :evaluation/message))))

(deftest the-queue-is-visible-and-can-be-changed
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          model (resolve-as root "root.model")
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (pace! root model 150)
      (let [a (say! w root-id route "first, and it takes a while")
            _ (until #(pos? (long (or (field root root-id :loop/iteration) 0))) 5000)
            b (say! w root-id route "second")
            c (say! w root-id route "third")
            d (say! w root-id route "fourth")
            waiting (until #(let [q (queue-of root root-id)] (when (= 3 (count q)) q)) 5000)]
        (testing "the wakes behind the running one are on loop/queue, in order"
          (is (= [(str b) (str c) (str d)] (mapv :message waiting)) (pr-str waiting))
          (is (every? #(= (str (:id witness)) (:from %)) waiting))
          (is (every? #(= "message" (:kind %)) waiting))
          (is (every? (comp string? :at) waiting))
          (is (not-any? #(= (str a) (:message %)) waiting) "the running wake is not waiting"))
        (testing "reorder: anyone who can write the Principal"
          (let [r (act! root "loop.reorder" {:principal (str root-id) :message (str d) :position 0})]
            (is (:ok r) (pr-str r))
            (is (= [(str d) (str b) (str c)] (mapv :message (queue-of root root-id))))))
        (testing "dequeue: the Message's sender, though it cannot write the Principal"
          (fx/grant! root (:id witness) #{(resolve-as root "action/loop.dequeue")} #{:evaluate :read})
          (let [r (act! w "loop.dequeue" {:principal (str root-id) :message (str b)})]
            (is (:ok r) (pr-str r))
            (is (true? (get-in r [:result :result :dequeued])))
            (is (= [(str d) (str c)] (mapv :message (queue-of root root-id))))
            (is (true? (field root b :message/dequeued)) "the Message stays on the log, marked")))
        (testing "a wake that has started cannot be dequeued"
          (let [r (act! w "loop.dequeue" {:principal (str root-id) :message (str a)})]
            (is (= :conflict (get-in r [:err :code])) "it is not waiting any more")))
        (testing "the queue drains in the new order, without the dequeued one"
          (pace! root model 0)
          (is (until #(empty? (queue-of root root-id)) 15000))
          (is (until #(and (evaluated-for? world d) (evaluated-for? world c)) 15000))
          (is (not (evaluated-for? world b)) "a dequeued Message never ran"))))))

(deftest waiting-wakes-survive-a-restart
  (let [dir (fx/temp-dir)]
    (try
      (let [world (fx/start-world dir 0)
            root (fx/attach-root world)
            root-id (:principal root)
            route (resolve-as root "root.route")
            model (resolve-as root "root.model")
            _ (pace! root model 400)
            a (say! root root-id route "a long one")
            _ (until #(pos? (long (or (field root root-id :loop/iteration) 0))) 5000)
            b (say! root root-id route "behind it")
            c (say! root root-id route "and behind that")
            waiting (until #(let [q (queue-of root root-id)] (when (= 2 (count q)) q)) 5000)]
        (is (uuid? a))
        (is (= [(str b) (str c)] (mapv :message waiting)))
        (ws/close! root)
        (system/stop world)
        (let [world2 (fx/start-world dir 0)
              root2 (fx/attach-root world2)]
          (try
            (pace! root2 model 0)
            (is (until #(and (evaluated-for? world2 b) (evaluated-for? world2 c)) 15000)
                "both waiting wakes ran after the restart")
            (is (until #(empty? (queue-of root2 root-id)) 5000) "and the queue drained")
            (finally (ws/close! root2) (system/stop world2)))))
      (finally (fx/delete-tree! dir)))))

(deftest promotion-rechecks-the-winning-queue-state
  ;; Use protocol-created Messages and the real queue log writes. The person's
  ;; fixture lane has no resident process, so only this test drives its transitions.
  (fx/with-world [{:keys [world root witness]}]
    (doseq [transition [:consume :end]]
      (let [p (:id witness)
            route (resolve-as root "root.route")
            running (say! root p route "running race fixture")
            m (say! root p route "waiting race fixture")
            db (act/db world)
            lane-world (assoc world :loop-state (atom {}))
            _ (queue/enqueue! lane-world db {:principal p :message running :route route})
            _ (queue/take-next! lane-world db p)
            _ (queue/enqueue! lane-world db {:principal p :message m :route route})
            real-swap clojure.core/swap-vals!
            once (atom false)
            result (with-redefs [clojure.core/swap-vals!
                                 (fn [a f & args]
                                   (when (and (identical? a (:loop-state lane-world))
                                              (compare-and-set! once false true))
                                     (case transition
                                       :consume (queue/take-next! lane-world db p)
                                       :end (queue/end! lane-world db p)))
                                   (apply real-swap a f args))]
                     (queue/promote! lane-world db p m))]
        (case transition
          :consume (do (is (nil? result) "a Message already taken cannot become its own steer")
                       (is (= m (:message (queue/active lane-world p))))
                       (is (empty? (:pending (queue/active lane-world p)))))
          :end (do (is (false? (:steered result)) "an ended lane keeps the original waiting wake")
                   (is (nil? (queue/active lane-world p)))
                   (is (= [m] (mapv :message (queue/waiting lane-world p))))))))))
