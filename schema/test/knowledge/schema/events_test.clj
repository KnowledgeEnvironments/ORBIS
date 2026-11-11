(ns knowledge.schema.events-test
  (:require [clojure.test :refer [deftest is]]
            [knowledge.schema.registry :as r]
            [knowledge.schema.events :as ev]
            [malli.core :as m]
            [malli.generator :as mg]))

(deftest twenty-four-kinds
  (is (= 24 (count ev/kinds)) "created bound unbound field-set field-read resolved granted revoked sent delivered task-moved evaluated fired attached detached retired pane-opened pane-input pane-output pane-edit pane-closed"))

(deftest every-kind-generates-and-validates
  (let [s (r/schema :ke/Event)]
    (doseq [k ev/kinds]
      (let [ks (m/schema (ev/event-of k) r/opts)
            v (mg/generate ks {:size 4})]
        (is (m/validate ks v) (str k))
        (is (m/validate s v) (str "multi " k))))))
