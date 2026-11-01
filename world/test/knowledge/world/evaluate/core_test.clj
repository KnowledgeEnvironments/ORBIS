(ns knowledge.world.evaluate.core-test
  "evaluate on the :stub Model (spec 9.3, ground rule 7): queued, running, done with an outcome
   and a cost, on the Evaluation's, the Model's and the initiator's streams; residency refusal."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- events [c stream] (get-in (ws/request!! c :history {:stream stream :limit 1000}) [:result :events]))

(defn until
  "Poll f every 50 ms up to timeout-ms for a truthy value."
  [f timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop [] (or (f) (when (< (System/currentTimeMillis) deadline) (Thread/sleep 50) (recur))))))

(deftest evaluate-on-the-stub-model
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          r (ws/request!! root :evaluate {:working-set #{root-id} :model model :policy :one-shot})
          ev (get-in r [:result :evaluation])
          done (until #(some (fn [e] (and (= :evaluated (:event/kind e)) (= :done (get-in e [:event/body :state])) e)) (events root ev)) 5000)
          states (map #(get-in % [:event/body :state]) (filter #(= :evaluated (:event/kind %)) (events root ev)))
          state (:result (ws/request!! root :read {:object ev}))]
      (is (:ok r) (pr-str r))
      (is (some? done) "the stub finishes")
      (is (= [:queued :running] (take 2 states)))
      (is (= :done (last states)))
      (is (every? #{:streaming} (drop 2 (butlast states))) "streaming events (decisions 2026-10-30) sit between running and done")
      (testing "the done event carries outcome and cost; the Evaluation folds them"
        (is (string? (get-in done [:event/body :outcome :text])))
        (is (re-find #"instructions/charter" (get-in done [:event/body :outcome :text])) "the stub read root's Fields")
        (is (= #{:tokens-in :tokens-out} (set (keys (get-in done [:event/body :cost])))))
        (is (= :done (get-in state [:object/fields :evaluation/state :field/value])))
        (is (uuid? (get-in state [:object/fields :evaluation/serialization :field/value])))
        (is (= :one-shot (get-in state [:object/fields :evaluation/policy :field/value]))))
      (testing "the Serialization is named and resolvable; it is the Evaluation's exact input"
        (let [ser (:result (ws/request!! root :read {:object (get-in state [:object/fields :evaluation/serialization :field/value])}))]
          (is (= :serialization (:object/class ser)))
          (is (re-find (re-pattern (get-in ser [:object/fields :serialization/hash :field/value])) (get-in done [:event/body :outcome :text])))))
      (testing "evaluated events reach the Model's and the initiator's streams"
        (is (some #(= ev (get-in % [:event/body :evaluation])) (events root model)))
        (is (some #(= ev (get-in % [:event/body :evaluation])) (events root root-id))))
      (testing "an external Model is refused with err residency; a Model the witness does not resolve is nothing"
        (let [ext (get-in (ws/request!! root :act {:class :model :action :create :args {:kind :model :residency :external :base-url "http://x.test/v1"}}) [:result :object])
              w (fx/attach-as world (:token witness))]
          (is (= :residency (get-in (ws/request!! root :evaluate {:working-set #{root-id} :model ext}) [:err :code])))
          (is (= :unresolved (get-in (ws/request!! w :evaluate {:working-set #{root-id} :model model}) [:err :code])))
          (is (= :unresolved (get-in (ws/request!! root :evaluate {:working-set #{root-id} :model root-id}) [:err :code])) "not a Model")
          (ws/close! w))))))
