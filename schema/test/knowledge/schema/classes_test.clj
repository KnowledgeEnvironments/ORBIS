(ns knowledge.schema.classes-test
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.schema.registry :as r]
            [knowledge.schema.classes :as cls]
            [malli.core :as m]
            [malli.generator :as mg]))

(deftest every-named-schema-compiles
  (doseq [[k v] r/named]
    (is (m/schema v r/opts) (str k))))

(deftest fourteen-classes
  (is (= 14 (count (rest cls/ClassName))))
  (is (= 5 (count cls/kernel-classes)))
  (is (= 9 (count cls/arrangements))))

(deftest generated-values-validate
  (doseq [k [:ke/Field :ke/Object :ke/Principal :ke/Grant :ke/Task :ke/Message :ke/Model :ke/Route :ke/Trigger]]
    (testing (str k)
      (let [s (r/schema k)]
        (dotimes [_ 5]
          (is (m/validate s (mg/generate s {:size 5}))))))))
