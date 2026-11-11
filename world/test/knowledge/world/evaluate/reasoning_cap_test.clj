(ns knowledge.world.evaluate.reasoning-cap-test
  "model/reasoning-max-chars. A stream whose reasoning passes the cap is cut fast, and the loop
   retries once with reasoning off and the partial reasoning in the prompt, so a long /think ends
   in an answer rather than a stall."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is]]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [c id k] (get-in (ws/request!! c :read {:object id}) [:result :object/fields k :field/value]))

(deftest reasoning-past-the-cap-is-cut-and-answered-from
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          model (resolve-as root "root.model")
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      ;; the stub thinks aloud in 24-char pieces 50 ms apart when asked; ten chars of reasoning is the cap
      (is (:ok (ws/request!! root :set-field {:object model :field :model/stub-delta-ms :type :number :value 50 :reader :both})))
      (is (:ok (ws/request!! root :set-field {:object model :field :model/reasoning-max-chars :type :number :value 10 :reader :both})))
      (let [head (:seq (ws/request!! root :read {:object root-id}))
            sent (ws/request!! w :send {:to #{root-id} :route route :text "think aloud, then answer"})
            events #(get-in (ws/request!! root :history {:stream root-id :after head :limit 1000}) [:result :events])
            of-state (fn [state] (filter (fn [e] (and (= :evaluated (:event/kind e)) (= state (get-in e [:event/body :state])))) (events)))
            done (until #(first (of-state :done)) 20000)
            failed (first (of-state :failed))
            cut (get-in failed [:event/body :evaluation])
            retry (get-in done [:event/body :evaluation])]
        (is (:ok sent))
        (is (some? failed) "the cut call failed on the log")
        (is (str/includes? (str (get-in failed [:event/body :outcome :error])) "model/reasoning-max-chars"))
        (is (< (long (or (get-in failed [:event/body :outcome :elapsed-ms]) 0)) 5000) "cut fast, not at the deadline")
        (is (seq (str (field root cut :evaluation/thinking))) "what was reasoned stays on the cut Evaluation")
        (is (= cut (field root retry :evaluation/retry-of)) "the answer names the call it retried")
        (is (= :off (field root retry :evaluation/reasoning)))
        (is (= 1 (count (of-state :failed))) "one retry, no second cut")
        (is (= "" (str (field root root-id :loop/stalled))) "the wake ended in an answer, not a stall"))
      (ws/close! w))))
