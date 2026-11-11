(ns knowledge.world.protocol.codec-test
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.protocol.codec :as codec]))

(def u #uuid "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0001")

(deftest both-forms-round-trip
  (doseq [format [:transit :json]]
    (testing (name format)
      (let [req {:id "r1" :op :set-field :args {:object u :field :notes/first :type :string :value "hi" :reader :both}}
            ack {:ack "h" :seq 3}
            ;; a response result is :any in the schema: in the JSON form ids inside it stay strings
            resp {:id "r1" :ok true :result {:session "s" :principal (if (= format :json) (str u) u) :resumed false :seq 1} :seq 1}
            err {:id "r1" :ok false :err {:code :not-implemented :message "x"} :seq 1}
            push {:sub "h" :as-of 2 :events [{:event/id u :event/stream u :event/seq 2 :event/kind :field-set :event/actor u
                                              :event/at #inst "2026-10-29T06:00:00.000Z"
                                              :event/body {:name :n :type :string :value "v" :reader :both}}]}]
        (doseq [f [req ack resp err push]]
          (is (= f (codec/decode format (codec/encode format f))) (pr-str f)))))))

(deftest json-form-shapes
  (let [s (codec/encode :json {:id "r1" :op :set-field :args {:object u :field :notes/first :type :string :value "hi" :reader :both}})]
    (is (re-find #"\"op\":\"set-field\"" s))
    (is (re-find #"\"type\":\"string\"" s))
    (is (re-find #"\"object\":\"5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0001\"" s)))
  (testing "invalid frames throw, raw-id still finds the id"
    (is (thrown? clojure.lang.ExceptionInfo (codec/decode :json "{\"id\":\"zz\",\"op\":\"nope\",\"args\":{}}")))
    (is (= "zz" (codec/raw-id :json "{\"id\":\"zz\",\"op\":\"nope\"}")))
    (is (thrown? clojure.lang.ExceptionInfo (codec/decode :transit "[\"^ \",\"~:id\",\"zz\",\"~:op\",\"~:nope\"]")))
    (is (= "zz" (codec/raw-id :transit "[\"^ \",\"~:id\",\"zz\",\"~:op\",\"~:nope\"]"))))
  (testing "plain walks untyped results"
    (is (= {"object/id" "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0001" "k" "a/b" "s" [1] "t" "2026-10-29T06:00:00Z"}
           (let [m (codec/plain {:object/id u :k :a/b :s #{1} :t #inst "2026-10-29T06:00:00.000Z"})]
             (into {} (map (fn [[k v]] [(if (keyword? k) (subs (str k) 1) k) v])) m))))))
