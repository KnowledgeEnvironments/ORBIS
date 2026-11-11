(ns knowledge.world.boot.kinds-test
  "class/description and class/kinds reach an older world: the later step's events are valid, and an
   Adapter schema that has its description but not its kinds is finished rather than skipped."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]))

(deftest the-kinds-step-finishes-what-a-failed-run-left
  (fx/with-world [{:keys [world]}]
    (let [id (objects/lookup (act/db world) (:world-id world) (act/schema-name :adapter))
          kinds (fn [] (get-in (objects/fields (act/db world) id) [:class/kinds :field/value]))]
      (log/append!! (:log world) (log/pending :field-set id (:world-id world)
                                              {:name :class/kinds :type [:map :string] :value {} :reader :both :cleared true}))
      (is (nil? (kinds)))
      (is (= 1 (:described (kz/step! world :kinds))) "a description alone does not finish an Adapter")
      (is (string? (:fs (kinds))) "keyword keys, as a map Field needs")
      (is (zero? (:described (kz/step! world :kinds)))))))
