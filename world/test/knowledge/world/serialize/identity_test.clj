(ns knowledge.world.serialize.identity-test
  "Who a Principal is opens its prompt; what it did not do it cannot claim."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(deftest the-soul-comes-first-and-only-once
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          soul (apply str "You are Ivy. " (repeat 40 "A line about who you are. "))]
      (is (:ok (ws/request!! root :set-field {:object p :field :instructions/soul :type :string :value soul})))
      (let [r (ser/render (act/db world) {:principal p :working-set #{p}})
            system (:content (first (ser/chat-messages r)))]
        (is (str/starts-with? system "This is who you are; the rest describes the world you live in."))
        (is (< (.indexOf ^String system "You are Ivy.") (.indexOf ^String system "# Acting")))
        (is (= 1 (count (re-seq #"You are Ivy\." system))) "not again among the Fields")))))

(deftest no-identity-no-block
  (fx/with-world [{:keys [world root]}]
    (is (nil? (ser/identity-block (act/db world) (:principal root))))))

(deftest a-person-authored-instruction-survives-the-ship
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)]
      (is (:ok (ws/request!! root :set-field {:object p :field :instructions/charter :type :string :value "my own charter"})))
      (kz/ensure-shipped-instructions! world)
      (is (= "my own charter" (get-in (objects/fields (act/db world) p) [:instructions/charter :field/value]))))))

(deftest a-gated-op-names-its-task-or-none
  (let [line #'ser/gated-line]
    (is (= "gate: act stopped; Task abc" (line {:op "act" :err {:code "approval-pending" :task "abc"}})))
    (is (= "gate: act stopped; Task none, so nothing is pending" (line {:op "act" :err {:code :gated}})))
    (is (nil? (line {:op "act" :err {:code :unresolved}})))
    (is (nil? (line {:op "act" :ok true})))))
