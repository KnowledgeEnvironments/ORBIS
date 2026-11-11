(ns knowledge.schema.protocol-test
  (:require [clojure.test :refer [deftest is testing]]
            [clojure.java.io :as io]
            [knowledge.schema.registry :as r]
            [knowledge.schema.protocol :as p]
            [knowledge.schema.wire :as wire]
            [malli.core :as m]))

(def examples-dir (io/file "../docs/protocol-examples"))

(deftest all-ops-have-args-and-results
  (doseq [op p/ops]
    (is (contains? p/args op) (str op " args"))
    (is (contains? p/results op) (str op " result"))))

(deftest protocol-examples-decode-and-validate
  (let [files (->> (.listFiles examples-dir) (filter #(.endsWith (.getName %) ".json")) sort)]
    (is (seq files) "examples present")
    (doseq [f files]
      (testing (.getName f)
        (let [v (wire/decode (slurp f))]
          (is (m/validate (r/schema :ke/Frame) v))
          (is (#{:request :response :push :ack} (wire/frame-kind v))))))))

(deftest json-round-trip
  (let [v (wire/decode (slurp (io/file examples-dir "push.json")))
        s (wire/encode v)
        v2 (wire/decode s)]
    (is (= (dissoc v :events) (dissoc v2 :events)))
    (is (= (count (:events v)) (count (:events v2))))
    (is (= (map :event/seq (:events v)) (map :event/seq (:events v2))))
    (is (= :field-set (:event/kind (second (:events v2)))))
    (is (inst? (:event/at (first (:events v2)))))))
