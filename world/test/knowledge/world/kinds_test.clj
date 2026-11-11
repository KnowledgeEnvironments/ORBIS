(ns knowledge.world.kinds-test
  "What each kind of thing is lives on its schema Object; the model gets it once per kind present."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.kinds :as kinds]
            [knowledge.world.serialize.core :as ser]))

(deftest the-words-live-on-the-schema-objects
  (fx/with-world [{:keys [world]}]
    (let [db (act/db world)
          wid (:world-id world)]
      (doseq [[k d] kinds/descriptions]
        (is (= d (get-in (objects/fields db (objects/lookup db wid (str "schema." (name k)))) [:class/description :field/value])) (str k)))
      (is (empty? (kz/kinds-undescribed db wid))))))

(deftest a-glossary-names-each-kind-once
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          wid (:world-id world)
          model (objects/lookup db (:principal root) "root.model")
          shell (objects/lookup db wid "shell.local")
          harness (objects/lookup db wid "harness")
          g (kinds/glossary db [model shell harness shell])]
      (is (= ["shell adapter" "Runs commands on a machine, from a starting directory."] (kinds/kind-of db shell)))
      (is (= "fs adapter" (first (kinds/kind-of db harness))))
      (is (= 1 (count (re-seq #"shell adapter:" g))))
      (is (str/includes? g "model: A model or program"))
      (is (str/includes? g "only a convention"))
      (is (nil? (kinds/glossary db []))))
    (testing "the wake's known list carries it"
      (let [p (:principal root)
            db (act/db world)
            body (:body (ser/render db {:principal p :working-set #{p} :known [(objects/lookup db (:world-id world) "shell.local")]}))]
        (is (str/includes? body "kinds: shell adapter:") body)))))
