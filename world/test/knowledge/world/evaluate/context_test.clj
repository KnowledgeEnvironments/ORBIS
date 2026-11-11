(ns knowledge.world.evaluate.context-test
  "Decisions 2026-10-30: a Model carries model/timeout-s and model/max-tokens, read by the
   adapter at evaluate time; every done Evaluation records evaluation/context, and
   evaluation/thinking when the Model returned reasoning (<think> blocks are split from the reply)."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.adapters.model-api :as model]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- v [r k] (get-in r [:result :object/fields k :field/value]))
(defn- events [c stream] (get-in (ws/request!! c :history {:stream stream :limit 1000}) [:result :events]))

(deftest split-thinking-is-pure
  (is (= ["answer" "why"] (model/split-thinking "<think>why</think>answer" nil)))
  (is (= ["a b" "one\n\ntwo"] (model/split-thinking "<think>one</think>a <think>two</think>b" nil)))
  (is (= ["plain" nil] (model/split-thinking "plain" nil)))
  (is (= ["plain" "r"] (model/split-thinking "plain" "r")))
  (is (= ["x" "r\n\nt"] (model/split-thinking "<think>t</think>x" "r"))))

(deftest model-fields-reach-the-adapter
  (fx/with-world [{:keys [root]}]
    (let [model (resolve-as root "root.model")
          e (ws/request!! root :read {:object model})]
      (is (= 600 (v e :model/timeout-s)) "kernel-zero sets the timeout Field on root.model")
      (is (= 16384 (v e :model/max-tokens)))
      (let [a (model/for-model (:result e))]
        (is (= 600 (:timeout-s a)))
        (is (= 16384 (:max-tokens a))))
      (is (:ok (ws/request!! root :set-field {:object model :field :model/timeout-s :type :number :value 42})))
      (is (= 42 (:timeout-s (model/for-model (:result (ws/request!! root :read {:object model}))))) "read at evaluate time, not at boot")
      (is (= model/default-timeout-s (:timeout-s (model/create {})))))))

(deftest done-evaluations-carry-context-and-thinking
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (let [head (:seq (ws/request!! root :read {:object root-id}))
            sent (ws/request!! w :send {:to #{root-id} :route route :text "think aloud, then tell me what you know"})
            push (ws/await-push w "inbox" 15000)
            reply (some-> push :events first :event/body :message)
            evaluation (until #(some (fn [e] (when (and (= :evaluated (:event/kind e)) (= :done (get-in e [:event/body :state])))
                                              (get-in e [:event/body :evaluation])))
                                     (filter (fn [e] (> (:event/seq e) head)) (events root root-id))) 15000)
            ev (ws/request!! root :read {:object evaluation})]
        (is (:ok sent) (pr-str sent))
        (is (some? reply) "root replied")
        (testing "evaluation/context: the Serialization's size, the token counts, the objects in view"
          (let [c (v ev :evaluation/context)]
            (is (map? c) (pr-str c))
            (is (pos? (:serialization-bytes c)))
            (is (pos? (:objects c)))
            (is (contains? c :prompt-tokens))
            (is (contains? c :completion-tokens))))
        (testing "evaluation/thinking holds the <think> block; the reply does not"
          (is (str/starts-with? (str (v ev :evaluation/thinking)) "stub thinking:"))
          (let [text (v (ws/request!! root :read {:object reply}) :message/text)]
            (is (not (str/includes? (str text) "<think>")))
            (is (str/starts-with? (str text) "stub model: I read"))))
        (testing "the Fields were set by root via the Evaluation, under its delegation"
          (let [fs (filter #(and (= :field-set (:event/kind %)) (= :evaluation/context (get-in % [:event/body :name]))) (events root evaluation))]
            (is (= 1 (count fs)))
            (is (= evaluation (:event/via (first fs))))
            (is (uuid? (:event/delegation (first fs)))))))
      (ws/close! w))))
