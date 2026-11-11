(ns knowledge.world.evaluate.cancel-test
  "Cancelling a running Evaluation (decisions 2026-10-30): `act {object action cancel}` on the
   Evaluation or on the Principal whose wake it is. The stub Model is paced with
   model/stub-delta-ms so the stream can be interrupted mid-way."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [c id k] (get-in (ws/request!! c :read {:object id}) [:result :object/fields k :field/value]))
(defn- events [c stream] (get-in (ws/request!! c :history {:stream stream :limit 1000}) [:result :events]))
(defn- evaluated-events [c stream] (filter #(= :evaluated (:event/kind %)) (events c stream)))

(defn- running-evaluation-after
  "The Evaluation id of the first `evaluated running` on root's stream after seq, or nil."
  [root root-id after]
  (some (fn [e] (when (and (> (:event/seq e) after) (= :running (get-in e [:event/body :state])))
                  (get-in e [:event/body :evaluation])))
        (evaluated-events root root-id)))

(defn- pace! [root model ms]
  (let [r (ws/request!! root :set-field {:object model :field :model/stub-delta-ms :type :number :value ms :reader :both})]
    (when-not (:ok r) (throw (ex-info "pace failed" {:response r})))))

(deftest a-wake-is-cancelled-mid-stream-and-the-loop-goes-on
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          model (resolve-as root "root.model")
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (pace! root model 150) ; the stub's reply is a few hundred chars: a dozen deltas, about two seconds
      (testing "root cancels its own wake by Principal id"
        (let [head (:seq (ws/request!! root :read {:object root-id}))
              sent (ws/request!! w :send {:to #{root-id} :route route :text "tell me everything, slowly"})
              evaluation (until #(running-evaluation-after root root-id head) 10000)
              _ (Thread/sleep 500) ; let a few deltas stream
              r (ws/request!! root :act {:object root-id :action :cancel :args {}})
              evs (until #(let [es (evaluated-events root evaluation)]
                            (when (some (fn [e] (= :cancelled (get-in e [:event/body :state]))) es) es)) 5000)
              cancelled (first (filter #(= :cancelled (get-in % [:event/body :state])) evs))
              states (map #(get-in % [:event/body :state]) evs)]
          (is (:ok sent) (pr-str sent))
          (is (uuid? evaluation) "root's loop started an Evaluation")
          (is (:ok r) (pr-str r))
          (is (= evaluation (get-in r [:result :result :evaluation])))
          (is (= "cancelled" (get-in r [:result :result :state])))
          (is (some? cancelled) "an evaluated cancelled event reached the log")
          (is (= root-id (get-in cancelled [:event/body :by])))
          (is (pos? (long (or (get-in cancelled [:event/body :partial-chars]) 0))) "some content had streamed")
          (is (= (get-in r [:result :seq]) (:event/seq cancelled)) "the act answers with the cancelled event's seq")
          (is (not-any? #{:done :failed} states) "no done follows a cancel")
          (is (= :cancelled (last states)))
          (is (= :cancelled (field root evaluation :evaluation/state)) "the Evaluation folds to cancelled")
          (is (str/starts-with? (str (field root root-id :loop/stalled)) "cancelled by root") (pr-str (field root root-id :loop/stalled)))
          (is (= 0 (field root root-id :loop/iteration)))
          (is (nil? (ws/await-push w "inbox" 1500)) "no reply was sent for the cancelled wake")
          (testing "the loop takes the next Message at once"
            (pace! root model 0)
            (let [sent2 (ws/request!! w :send {:to #{root-id} :route route :text "and now?"})
                  push (ws/await-push w "inbox" 15000)]
              (is (:ok sent2))
              (is (some? push) "root answered the next Message")))
          (testing "cancelling a finished Evaluation is a conflict"
            (let [r2 (ws/request!! root :act {:object evaluation :action :cancel :args {}})]
              (is (= :conflict (get-in r2 [:err :code])) (pr-str r2))
              (is (str/includes? (get-in r2 [:err :message]) "cancelled"))))
          (testing "a Principal with nothing running is a conflict too"
            (let [r3 (ws/request!! root :act {:object root-id :action :cancel :args {}})]
              (is (= :conflict (get-in r3 [:err :code])) (pr-str r3))))))
      (ws/close! w))))

(deftest the-asker-may-cancel-the-answer-and-nobody-else
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          model (resolve-as root "root.model")
          w (fx/attach-as world (:token witness))
          bystander (let [r (ws/request!! root :act {:class :principal :action :create :args {:driven-by :person :name "test/bystander"}})]
                      {:id (get-in r [:result :object]) :token (get-in r [:result :result :token])})
          b (fx/attach-as world (:token bystander))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (pace! root model 150)
      (let [head (:seq (ws/request!! root :read {:object root-id}))
            sent (ws/request!! w :send {:to #{root-id} :route route :text "a long answer please"})
            evaluation (until #(running-evaluation-after root root-id head) 10000)]
        (is (:ok sent))
        (is (uuid? evaluation))
        (Thread/sleep 300)
        (testing "a Principal without a Grant on root or the Evaluation gets unresolved"
          (is (= :unresolved (get-in (ws/request!! b :act {:object root-id :action :cancel :args {}}) [:err :code])))
          (is (= :unresolved (get-in (ws/request!! b :act {:object evaluation :action :cancel :args {}}) [:err :code]))))
        (testing "the witness has no Grant on root as a Principal"
          (is (= :unresolved (get-in (ws/request!! w :act {:object root-id :action :cancel :args {}}) [:err :code]))))
        (testing "but the witness sent the Message, so it resolves the Evaluation answering it (step 7)"
          (let [r (ws/request!! w :act {:object evaluation :action :cancel :args {}})
                cancelled (until #(some (fn [e] (when (= :cancelled (get-in e [:event/body :state])) e))
                                        (evaluated-events root evaluation)) 5000)]
            (is (:ok r) (pr-str r))
            (is (some? cancelled))
            (is (= (:id witness) (get-in cancelled [:event/body :by])))
            (is (str/starts-with? (str (field root root-id :loop/stalled)) "cancelled by test/witness")))))
      (ws/close! b)
      (ws/close! w))))
