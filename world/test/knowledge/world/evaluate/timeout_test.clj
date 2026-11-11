(ns knowledge.world.evaluate.timeout-test
  "No Evaluation stays running past its deadline: the Model call fails when
   model/timeout-s passes or when no delta arrives for model/idle-timeout-s, with the reason and
   the chars received; fail! ends a running Evaluation from outside; the loop stalls with the reason."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [c id k] (get-in (ws/request!! c :read {:object id}) [:result :object/fields k :field/value]))
(defn- put! [c o k t v] (is (:ok (ws/request!! c :set-field {:object o :field k :type t :value v :reader :both}))))
(defn- states [c ev] (->> (get-in (ws/request!! c :history {:stream ev :limit 100}) [:result :events])
                          (filter #(= :evaluated (:event/kind %)))
                          (map #(get-in % [:event/body :state]))
                          (remove #{:streaming}))) ; deltas before the timeout are fine; the terminal states are the point
(defn- failure [c ev] (->> (get-in (ws/request!! c :history {:stream ev :limit 100}) [:result :events])
                           (filter #(= :failed (get-in % [:event/body :state])))
                           first :event/body :outcome))
(defn- evaluate-sync [c root-id model]
  (let [r (ws/request!! c :evaluate {:working-set #{root-id} :model model :policy :one-shot})
        ev (get-in r [:result :evaluation])]
    (is (:ok r) (pr-str r))
    (is (until #(let [s (set (states c ev))] (when (some s [:done :failed :cancelled]) s)) 20000) "the Evaluation ended")
    ev))

(deftest a-silent-stream-fails-at-the-idle-timeout
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")]
      ;; a delta every 700 ms against an idle timeout of a quarter second
      (put! root model :model/stub-delta-ms :number 700)
      (put! root model :model/idle-timeout-s :number 0.25)
      (let [ev (evaluate-sync root root-id model)
            f (failure root ev)]
        (is (= [:queued :running :failed] (vec (states root ev))) "failed, never done, after running")
        (is (str/starts-with? (str (:error f)) "no output for 0 s") (pr-str f))
        (is (str/includes? (str (:error f)) "model/idle-timeout-s"))
        (is (<= 600 (:elapsed-ms f)) (pr-str f))))))

(deftest a-slow-stream-fails-at-the-deadline-with-the-chars-so-far
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")]
      ;; the stub's reply streams in 24-char pieces 400 ms apart: well over a one second deadline
      (put! root model :model/stub-delta-ms :number 400)
      (put! root model :model/timeout-s :number 1)
      (put! root model :model/idle-timeout-s :number 120)
      (let [ev (evaluate-sync root root-id model)
            f (failure root ev)]
        (is (= [:queued :running :failed] (vec (states root ev))))
        (is (str/starts-with? (str (:error f)) "timeout after 1 s, ") (pr-str f))
        (is (re-find #", [1-9][0-9]* chars received" (str (:error f))) "some content had streamed before the deadline")
        (is (< (:elapsed-ms f) 3000) "it did not run to the end of the stub's reply")))))

(deftest fail-from-outside-ends-the-evaluation-once
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")]
      (put! root model :model/stub-delta-ms :number 500)
      (let [r (ws/request!! root :evaluate {:working-set #{root-id} :model model :policy :one-shot})
            ev (get-in r [:result :evaluation])]
        (is (until #(some #{:running} (states root ev)) 5000))
        (let [out (evaluate/fail! world ev "the loop gave up")]
          (is (= :failed (:state out)))
          (is (= "the loop gave up" (:error out))))
        (is (until #(some #{:failed} (states root ev)) 5000))
        (Thread/sleep 1500)
        (is (= [:queued :running :failed] (vec (states root ev))) "one terminal event, from whichever side won")
        (is (nil? (evaluate/fail! world ev "again")) "nothing running any more")))))

(deftest the-loop-stalls-with-the-timeout-reason
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (put! root model :model/stub-delta-ms :number 400)
      (put! root model :model/timeout-s :number 1)
      (is (:ok (ws/request!! w :send {:to #{root-id} :route route :text "take your time"})))
      (let [v (until #(let [s (field root root-id :loop/stalled)] (when (seq s) s)) 20000)]
        (is (string? v))
        (is (str/includes? (str v) "timeout after 1 s") v)
        (is (= 0 (field root root-id :loop/iteration))))
      (ws/close! w))))
