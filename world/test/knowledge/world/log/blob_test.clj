(ns knowledge.world.log.blob-test
  (:require [clojure.test :refer [deftest is]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.log.blob :as blob]))

(deftest put-get-exists
  (let [dir (fx/temp-dir)
        store (blob/open dir)
        bs (.getBytes "hello blob" "UTF-8")
        {:keys [blob/sha256 blob/size]} (blob/put! store bs)]
    (is (= 64 (count sha256)))
    (is (= 10 size))
    (is (blob/exists? store sha256))
    (is (= "hello blob" (String. (blob/get store sha256) "UTF-8")))
    (is (= sha256 (:blob/sha256 (blob/put! store bs))) "idempotent")
    (is (nil? (blob/get store (apply str (repeat 64 "0")))))
    (let [[v hit?] (blob/externalize store {:a (byte-array 10) :b (byte-array 5000)})]
      (is hit?)
      (is (bytes? (:a v)))
      (is (blob/receipt? (:b v))))
    (fx/delete-tree! dir)))
