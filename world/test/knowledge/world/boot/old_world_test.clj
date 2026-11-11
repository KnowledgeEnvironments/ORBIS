(ns knowledge.world.boot.old-world-test
  "What an older world gets on start: the gate only tightens, and the template only where it is ours."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.ws-client :as ws]))

(defn- put! [root id k t v] (is (:ok (ws/request!! root :set-field {:object id :field k :type t :value v}))))
(defn- policy! [world p v]
  ;; policy/* is a person's to write: the world stands in for how an older world came to hold it
  (log/append!! (:log world) (log/pending :field-set p (:world-id world) {:name :policy/approval :type [:list :string] :value v :reader :both})))
(defn- value [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))

(deftest an-old-default-policy-gains-shell
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)]
      (policy! world p ["task.run" "grant" "principal.create"])
      (kz/upgrade! world)
      (is (= ["task.run" "shell" "grant" "principal.create"] (value world p :policy/approval)))
      (testing "a policy a person chose is left alone"
        (policy! world p ["grant"])
        (kz/upgrade! world)
        (is (= ["grant"] (value world p :policy/approval)))))))

(deftest the-template-keeps-what-a-person-wrote
  (fx/with-world [{:keys [world root]}]
    (let [r (get-in (ws/request!! root :resolve {:name "root.route" :op :read}) [:result :object])]
      (put! root r :route/template :string
            "Mine above.\n\nAnswer the Message in plain prose, briefly. Every claim about yourself or the world must cite the Field.")
      (kz/upgrade! world)
      (let [t (value world r :route/template)]
        (is (str/starts-with? t "Mine above."))
        (is (str/includes? t "Answer in plain prose"))
        (is (not (str/includes? t "must cite")))))))
