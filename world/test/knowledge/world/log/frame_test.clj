(ns knowledge.world.log.frame-test
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.log.frame :as frame])
  (:import [java.io RandomAccessFile]
           [java.nio.file Files]
           [java.nio.file.attribute FileAttribute]))

(defn- temp-file [] (.toFile (Files/createTempFile "frame" ".bin" (make-array FileAttribute 0))))

(deftest frame-round-trip
  (let [ev {:event/kind :field-set :event/seq 7 :event/at (java.util.Date.) :event/body {:value (byte-array [1 2 3]) :s #{:a}}}
        bs (frame/frame-bytes (frame/encode-payload ev))
        f (temp-file)]
    (with-open [raf (RandomAccessFile. f "rw")]
      (.write raf bs)
      (let [ch (.getChannel raf)
            {:keys [status event next]} (frame/read-at ch 0 (alength bs))]
        (is (= :ok status))
        (is (= (alength bs) next))
        (is (= 7 (:event/seq event)))
        (is (= #{:a} (get-in event [:event/body :s])))
        (is (= [1 2 3] (vec (get-in event [:event/body :value]))))
        (testing "short read when the end is inside the frame"
          (is (= :short (:status (frame/read-at ch 0 (dec (alength bs)))))))))))

(deftest frame-crc-mismatch
  (let [bs (frame/frame-bytes (frame/encode-payload {:a 1}))
        f (temp-file)]
    (aset-byte bs (dec (alength bs)) (byte 0x21))
    (with-open [raf (RandomAccessFile. f "rw")]
      (.write raf bs)
      (is (= :bad-crc (:status (frame/read-at (.getChannel raf) 0 (alength bs))))))))
