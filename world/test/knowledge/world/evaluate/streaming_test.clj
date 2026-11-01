(ns knowledge.world.evaluate.streaming-test
  "Streaming evaluations (spec 8.4 partial output; decisions 2026-10-30): the Model's content
   deltas reach the log as `evaluated` streaming events, coalesced, before the unchanged done event."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- events [c stream] (get-in (ws/request!! c :history {:stream stream :limit 1000}) [:result :events]))

(deftest the-streamer-coalesces
  (let [out (atom [])
        {:keys [on-delta flush!]} (evaluate/streamer (fn [text k] (swap! out conj [k text])))]
    (doseq [d (repeat 30 "0123456789")] (on-delta d))
    (is (= [[1 (apply str (repeat 20 "0123456789"))]] @out) "200 chars flushes once, the rest waits")
    (flush!)
    (is (= 2 (count @out)))
    (is (= (apply str (repeat 10 "0123456789")) (second (second @out))))
    (testing "time flushes too"
      (reset! out [])
      (on-delta "a") (Thread/sleep (+ 20 evaluate/stream-flush-ms)) (on-delta "b")
      (is (= [[3 "ab"]] @out))
      (flush!)
      (is (= 1 (count @out)) "nothing left"))))

(deftest a-stub-evaluation-streams-before-done
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          r (ws/request!! root :evaluate {:working-set #{root-id} :model model :policy :one-shot})
          ev (get-in r [:result :evaluation])
          done (until #(some (fn [e] (and (= :evaluated (:event/kind e)) (= :done (get-in e [:event/body :state])) e)) (events root ev)) 5000)
          evs (filter #(= :evaluated (:event/kind %)) (events root ev))
          states (map #(get-in % [:event/body :state]) evs)
          deltas (filter #(= :streaming (get-in % [:event/body :state])) evs)]
      (is (:ok r) (pr-str r))
      (is (some? done))
      (is (= [:queued :running] (take 2 states)))
      (is (= :done (last states)))
      (is (every? #(= :streaming %) (drop 2 (butlast states))) "streaming events sit between running and done")
      (is (seq deltas) "the stub streams")
      (is (= (range 1 (inc (count deltas))) (map #(get-in % [:event/body :seq-in-stream]) deltas)) "seq-in-stream counts from 1")
      (is (= (get-in done [:event/body :outcome :text]) (apply str (map #(get-in % [:event/body :delta]) deltas)))
          "the deltas concatenate to the done text")
      (testing "the Evaluation's state folds to done, and streaming events reached the initiator's stream"
        (is (= :done (get-in (ws/request!! root :read {:object ev}) [:result :object/fields :evaluation/state :field/value])))
        (is (some #(= :streaming (get-in % [:event/body :state])) (events root root-id)))))))

(deftest thinking-streams-before-the-reply
  ;; decisions 2026-10-30: reasoning deltas are `evaluated` streaming events with
  ;; thinking-delta and a cumulative thinking-chars, on the same stream as the content deltas
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
            evaluation (until #(some (fn [e] (when (and (= :evaluated (:event/kind e)) (= :done (get-in e [:event/body :state])))
                                              (get-in e [:event/body :evaluation])))
                                     (filter (fn [e] (> (:event/seq e) head)) (events root root-id))) 15000)
            evs (filter #(= :evaluated (:event/kind %)) (events root evaluation))
            bodies (map :event/body evs)
            thinking (filter :thinking-delta bodies)
            content (filter :delta bodies)
            idx (fn [pred] (count (take-while (complement pred) bodies)))]
        (is (:ok sent) (pr-str sent))
        (is (some? push) "root replied")
        (is (seq thinking) "the stub's thinking streamed")
        (is (every? #(= :streaming (:state %)) thinking))
        (is (= (map :thinking-chars thinking) (reductions + (map (comp count :thinking-delta) thinking))) "thinking-chars is cumulative")
        (is (< (idx :thinking-delta) (idx :delta)) "thinking events come before content events")
        (is (not-any? #(and (:thinking-delta %) (:delta %)) bodies) "an event carries one kind of delta")
        (testing "the whole reasoning still lands on the Evaluation, and the done text excludes it"
          (let [ev (ws/request!! root :read {:object evaluation})
                whole (get-in ev [:result :object/fields :evaluation/thinking :field/value])]
            (is (= whole (apply str (map :thinking-delta thinking))))
            (is (not (clojure.string/includes? (get-in (last evs) [:event/body :outcome :text]) whole))))))
      (ws/close! w))))
