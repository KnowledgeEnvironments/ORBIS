(ns knowledge.world.schedule.core-test
  "The scheduler and Triggers (plan §19.1 W8, decisions 2026-10-30 'triggers') on the stub Model.
   due? is pure and driven by a fake clock; the clock-driven cases call tick! with an explicit now;
   the firing cases watch the log through the protocol; the supervisor case stalls a created
   Principal's loop and sees root woken; a stopped and restarted world keeps firing an armed Trigger."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.schedule.core :as schedule]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws])
  (:import [java.util Date]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- field [c id k] (get-in (fields c id) [k :field/value]))
(defn- history [c stream] (get-in (ws/request!! c :history {:stream stream :limit 1000}) [:result :events]))
(defn- events-of-kind [c stream kind] (filter #(= kind (:event/kind %)) (history c stream)))
(defn- create-trigger! [c args]
  (let [r (ws/request!! c :act {:class :trigger :action :create :args args})]
    (when-not (:ok r) (throw (ex-info "trigger create failed" {:response r})))
    (get-in r [:result :object])))
(defn- at [^long ms] (Date. ms))

(deftest due-is-pure-and-driven-by-the-clock
  (let [t0 1000000
        interval {:kind :interval :every-s 2 :armed-at (at t0) :fired-count 0}]
    (testing "an interval Trigger is due once every-s has passed since its last slot, else armed-at"
      (is (not (schedule/due? interval (at (+ t0 1999)))))
      (is (schedule/due? interval (at (+ t0 2000))))
      (is (not (schedule/due? (assoc interval :last-tick (at (+ t0 3000))) (at (+ t0 4999)))))
      (is (schedule/due? (assoc interval :last-tick (at (+ t0 3000))) (at (+ t0 5000))))
      (testing "a re-arm later than the last slot counts from the re-arm"
        (is (not (schedule/due? (assoc interval :last-tick (at t0) :armed-at (at (+ t0 10000))) (at (+ t0 11000)))))))
    (testing "fractional seconds (the spec's every-ms 500) work"
      (is (schedule/due? {:kind :interval :every-s 0.5 :armed-at (at t0)} (at (+ t0 500)))))
    (testing "an at Trigger is due at its instant once, never again"
      (is (not (schedule/due? {:kind :at :at (at (+ t0 5000)) :fired-count 0} (at (+ t0 4999)))))
      (is (schedule/due? {:kind :at :at (at (+ t0 5000)) :fired-count 0} (at (+ t0 5000))))
      (is (not (schedule/due? {:kind :at :at (at (+ t0 5000)) :fired-count 1} (at (+ t0 9000))))))
    (testing "event-driven kinds and unarmed shapes are never clock-due"
      (is (not (schedule/due? {:kind :on-change :every-s 1 :armed-at (at t0)} (at (+ t0 99999)))))
      (is (not (schedule/due? {:kind :interval :armed-at (at t0)} (at (+ t0 99999))))))))

(deftest an-interval-heartbeat-fires-and-the-owner-answers-once
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          _ (ws/request!! root :subscribe {:sub "fired" :stream root-id :kinds #{:fired}})
          t (create-trigger! root {:kind :interval :every-s 0.4 :armed true :text "heartbeat" :name "test.heartbeat"})
          push (ws/await-push root "fired" 3000)
          fired (first (filter #(= :fired (:event/kind %)) (:events push)))]
      (is (some? fired) "the heartbeat fired within 3 s")
      (is (= t (get-in fired [:event/body :trigger])))
      (is (= root-id (get-in fired [:event/body :owner])))
      (is (= t (:event/via fired)) "the fired event is via the Trigger")
      (let [m (get-in fired [:event/body :message])
            mf (fields root m)]
        (testing "the Message: from the owner to itself on its Route, kind trigger, via the Trigger, hop 0"
          (is (= root-id (get-in mf [:message/from :field/value])))
          (is (= #{root-id} (set (get-in mf [:message/to :field/value]))))
          (is (= route (get-in mf [:message/route :field/value])))
          (is (= :trigger (get-in mf [:message/kind :field/value])))
          (is (= t (get-in mf [:message/trigger :field/value])))
          (is (= 0 (get-in mf [:message/hop :field/value])))
          (is (str/includes? (str (get-in mf [:message/text :field/value])) "heartbeat")))
        (testing "the owner's loop woke on it (the stub answers in prose, so a reply is sent) and the reply woke nobody"
          (let [reply (until #(some (fn [e] (let [r (get-in e [:event/body :message])]
                                              (when (= m (get-in (fields root r) [:message/in-reply-to :field/value])) r)))
                                    (events-of-kind root root-id :sent)) 10000)]
            (is (uuid? reply) "root answered the heartbeat")
            (is (= "" (field root root-id :loop/stalled)))
            (is (nil? (some (fn [e] (let [r (get-in e [:event/body :message])]
                                      (when (= reply (get-in (fields root r) [:message/in-reply-to :field/value])) r)))
                            (events-of-kind root root-id :sent)))
                "nothing answers the answer"))))
      (testing "the books on the Trigger, written by the world, and disarm stops it"
        (is (pos? (long (field root t :trigger/fired-count))))
        (is (inst? (field root t :trigger/last-fired)))
        (let [d (ws/request!! root :disarm {:trigger t})
              n (field root t :trigger/fired-count)]
          (is (:ok d) (pr-str d))
          (is (false? (field root t :trigger/armed)))
          (Thread/sleep 1000)
          (is (= n (field root t :trigger/fired-count)) "no fire after disarm"))))))

(deftest tick-skips-a-busy-owner-and-counts-it
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          t (create-trigger! root {:kind :interval :every-s 3600 :armed true :name "test.slow"})
          far (at (+ (System/currentTimeMillis) (* 2 3600 1000)))]
      (testing "the owner's loop is in a wake: the slot is skipped, not queued"
        ;; loop/iteration is the kernel's own book; the test sets it as root would never: through the same op
        (ws/request!! root :set-field {:object root-id :field :loop/iteration :type :number :value 2})
        (let [r (schedule/tick! world far)]
          (is (= [t] (:skipped r)) (pr-str r))
          (is (empty? (:fired r)))
          (is (= 1 (field root t :trigger/skipped)))
          (is (nil? (field root t :trigger/last-fired)))))
      (testing "once the owner is idle the next slot fires"
        (ws/request!! root :set-field {:object root-id :field :loop/iteration :type :number :value 0})
        (let [r (schedule/tick! world (at (+ (.getTime far) (* 3600 1000))))]
          (is (= [t] (:fired r)) (pr-str r))
          (is (= 1 (field root t :trigger/fired-count))))))))

(deftest an-at-trigger-fires-once-and-disarms-itself
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          when (at (- (System/currentTimeMillis) 1000))
          t (create-trigger! root {:kind :at :at when :armed true :text "it is time" :name "test.at"})
          fired (until #(seq (filter (fn [e] (= t (get-in e [:event/body :trigger]))) (events-of-kind root root-id :fired))) 3000)]
      (is (seq fired) "fired once its instant had passed")
      (is (until #(false? (field root t :trigger/armed)) 2000) "disarmed itself")
      (is (= :disarmed (field root t :trigger/state)))
      (schedule/tick! world (at (+ (System/currentTimeMillis) 60000)))
      (is (= 1 (count (filter (fn [e] (= t (get-in e [:event/body :trigger]))) (events-of-kind root root-id :fired)))) "never twice"))))

(deftest max-fires-disarms-after-the-count
  (fx/with-world [{:keys [world root]}]
    (let [t (create-trigger! root {:kind :interval :every-s 3600 :armed true :max-fires 2 :name "test.twice"})
          now (System/currentTimeMillis)]
      (schedule/tick! world (at (+ now (* 2 3600 1000))))
      (is (true? (field root t :trigger/armed)))
      ;; the first firing woke root's loop; a slot that falls during that wake is skipped, so wait for idle
      (is (until #(= 0 (field root (:principal root) :loop/iteration)) 10000) "root's loop went idle again")
      (schedule/tick! world (at (+ now (* 4 3600 1000))))
      (is (= 2 (field root t :trigger/fired-count)))
      (is (false? (field root t :trigger/armed)) "max-fires reached: disarmed"))))

(deftest a-supervisor-trigger-wakes-root-when-a-loop-it-created-stalls
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          made (ws/request!! root :act {:action :principal.create
                                        :args {:name "sub" :charter "You are sub, a Principal root made for a test."
                                               ;; write: a loop keeps its own books (loop/stalled) under its Grant
                                               :grant {:scope {:prefix "sub"} :ops ["read" "reference" "transmit" "write"]}}})
          sub (get-in made [:result :object])
          supervisor (get-in made [:result :result :supervisor])
          sub-route (get-in made [:result :result :route])]
      (is (:ok made) (pr-str made))
      (testing "principal.create left root an armed on-change Trigger on sub's loop/stalled"
        (is (uuid? supervisor))
        (is (= :on-change (field root supervisor :trigger/kind)))
        (is (= {:object sub :field :loop/stalled} (field root supervisor :trigger/watch)))
        (is (true? (field root supervisor :trigger/armed)))
        (is (= root-id (field root supervisor :trigger/owner))))
      (testing "sub stalls (no Model) when asked something; the watch fires a Message to root; root's loop wakes"
        (ws/request!! root :set-field {:object sub :field :principal/models :type [:list [:ref :model]] :value []})
        (ws/request!! root :subscribe {:sub "fired" :stream root-id :kinds #{:fired}})
        (let [sent (ws/request!! root :send {:to #{sub} :route sub-route :text "sub, are you there?"})
              _ (is (:ok sent) (pr-str sent))
              stalled (until #(let [v (field root sub :loop/stalled)] (when-not (str/blank? (str v)) v)) 10000)
              push (ws/await-push root "fired" 5000)
              fired (first (filter #(and (= :fired (:event/kind %)) (= supervisor (get-in % [:event/body :trigger]))) (:events push)))]
          (is (str/includes? (str stalled) "no Model") (pr-str stalled))
          (is (some? fired) "the supervisor fired")
          (let [m (get-in fired [:event/body :message])
                text (str (field root m :message/text))]
            (is (= :trigger (field root m :message/kind)))
            (is (str/includes? text "loop/stalled on sub") text)
            (is (str/includes? text "no Model") "the firing carries the new value")
            (is (until #(some (fn [e] (= m (get-in (fields root (get-in e [:event/body :message])) [:message/in-reply-to :field/value])))
                              (events-of-kind root root-id :sent)) 10000)
                "root's loop woke on the supervisor's Message and answered"))))
      (testing "clearing loop/stalled (the empty string) does not fire: unless-empty"
        (let [n (count (events-of-kind root root-id :fired))]
          (ws/request!! root :set-field {:object sub :field :loop/stalled :type :string :value ""})
          (Thread/sleep 800)
          (is (= n (count (events-of-kind root root-id :fired)))))))))

(deftest arm-and-disarm-are-ops-through-resolution
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          w (fx/attach-as world (:token witness))
          t (create-trigger! root {:kind :interval :every-s 3600 :name "test.ops"})]
      (testing "born disarmed; a Principal without :write on it may neither arm nor disarm"
        (is (false? (field root t :trigger/armed)))
        (is (= :unresolved (get-in (ws/request!! w :arm {:trigger t}) [:err :code])))
        (is (= :unresolved (get-in (ws/request!! w :disarm {:trigger t}) [:err :code]))))
      (testing "the owner arms it: armed, armed-at, state armed"
        (let [r (ws/request!! root :arm {:trigger t})]
          (is (:ok r) (pr-str r))
          (is (= t (get-in r [:result :trigger])))
          (is (true? (field root t :trigger/armed)))
          (is (inst? (field root t :trigger/armed-at)))
          (is (= :armed (field root t :trigger/state)))))
      (testing "the spec's form: arm with a schedule predicate creates an interval Trigger that fires"
        (ws/request!! root :subscribe {:sub "fired" :stream root-id :kinds #{:fired}})
        (let [r (ws/request!! root :arm {:predicate {:kind :schedule :every-ms 400} :evaluation {:model root-id}})
              t2 (get-in r [:result :trigger])
              push (ws/await-push root "fired" 3000)]
          (is (:ok r) (pr-str r))
          (is (= :interval (field root t2 :trigger/kind)))
          (is (= 0.4 (double (field root t2 :trigger/every-s))))
          (is (some (fn [e] (= t2 (get-in e [:event/body :trigger]))) (:events push)) "fired within 3 s")
          (ws/request!! root :disarm {:trigger t2})))
      (testing "arm with an unknown predicate kind answers not-implemented, arm with nothing answers invalid"
        (is (= :not-implemented (get-in (ws/request!! root :arm {:predicate {:kind :threshold :meter :tokens :over root-id :bound 1.0} :evaluation {}}) [:err :code])))
        (is (= :invalid (get-in (ws/request!! root :arm {}) [:err :code]))))
      (ws/close! w))))

(deftest an-armed-trigger-keeps-firing-across-a-restart
  (let [dir (fx/temp-dir)]
    (try
      (let [world (fx/start-world dir 0)
            root (fx/attach-root world)
            root-id (:principal root)
            t (create-trigger! root {:kind :interval :every-s 0.5 :armed true :name "test.persistent"})
            _ (until #(pos? (long (or (field root t :trigger/fired-count) 0))) 3000)
            before (long (field root t :trigger/fired-count))]
        (is (pos? before) "fired before the restart")
        (ws/close! root)
        (system/stop world)
        (let [world2 (fx/start-world dir 0)
              root2 (fx/attach-root world2)]
          (try
            (is (until #(> (long (or (field root2 t :trigger/fired-count) 0)) before) 4000)
                "the restarted world's scheduler picked the armed Trigger up from the log and fired it again")
            (is (= #{"test.persistent"} (set (filter #{"test.persistent"} (map second (objects/names (act/db world2) t))))) "the same Trigger, not a new one")
            (finally (ws/close! root2) (system/stop world2)))))
      (finally (fx/delete-tree! dir)))))
