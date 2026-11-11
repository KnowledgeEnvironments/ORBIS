(ns knowledge.world.log.segment-test
  "Recovery (ADR 0006): a truncated tail is discarded, a bad frame elsewhere refuses to start."
  (:require [clojure.java.io :as io]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.log.core :as log]
            [knowledge.world.log.segment :as seg])
  (:import [java.io FileOutputStream RandomAccessFile]))

(defn- fill! [dir n]
  (let [l (log/open! dir {:fsync :never})
        a (random-uuid)]
    (dotimes [i n]
      (log/append!! l (log/pending :field-set a a {:name :x :type :string :value (str i) :reader :both})))
    (log/close! l)
    a))

(deftest truncated-tail-is-recovered
  (let [dir (fx/temp-dir)]
    (fill! dir 5)
    (let [f (first (seg/segment-files (io/file dir "log")))
          size (.length f)]
      (testing "garbage after the last frame"
        (with-open [o (FileOutputStream. f true)] (.write o (byte-array [0 0 0 40 9 9 9 9 1 2 3])))
        (let [l (log/open! dir {:fsync :never})]
          (is (= 5 (log/head l)))
          (is (= size (.length f)) "file truncated back to the last good frame")
          (is (= 6 (log/append!! l (log/pending :retired (random-uuid) (random-uuid) {}))))
          (log/close! l)))
      (testing "a frame cut in half"
        (with-open [raf (RandomAccessFile. f "rw")] (.setLength raf (- (.length f) 3)))
        (let [l (log/open! dir {:fsync :never})]
          (is (= 5 (log/head l)) "the half frame (seq 6) is discarded")
          (log/close! l))))
    (fx/delete-tree! dir)))

(deftest bad-crc-mid-file-is-refused
  (let [dir (fx/temp-dir)]
    (fill! dir 10)
    (let [f (first (seg/segment-files (io/file dir "log")))]
      (with-open [raf (RandomAccessFile. f "rw")]
        (.seek raf 60)
        (.write raf (int 0x7a)))
      (is (thrown-with-msg? clojure.lang.ExceptionInfo #"corrupt frame inside segment"
                            (log/open! dir {:fsync :never}))))
    (fx/delete-tree! dir)))
