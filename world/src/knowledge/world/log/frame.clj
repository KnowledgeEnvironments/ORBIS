(ns knowledge.world.log.frame
  "One log frame (ADR 0006): [u32 payload-len][u32 crc32(payload)][payload], payload is the
   Transit-JSON of an Event map. Pure functions over bytes and positional reads on a FileChannel."
  (:require [cognitect.transit :as transit])
  (:import [java.io ByteArrayInputStream ByteArrayOutputStream]
           [java.nio ByteBuffer]
           [java.nio.channels FileChannel]
           [java.util.zip CRC32]))

(def header-size 8)
(def max-payload (* 64 1024 1024))

(defn encode-payload ^bytes [event]
  (let [out (ByteArrayOutputStream. 512)]
    (transit/write (transit/writer out :json) event)
    (.toByteArray out)))

(defn decode-payload [^bytes payload]
  (transit/read (transit/reader (ByteArrayInputStream. payload) :json)))

(defn crc32 ^long [^bytes payload]
  (let [c (CRC32.)] (.update c payload) (.getValue c)))

(defn frame-bytes
  "The full frame for one payload."
  ^bytes [^bytes payload]
  (let [buf (ByteBuffer/allocate (+ header-size (alength payload)))]
    (.putInt buf (alength payload))
    (.putInt buf (unchecked-int (crc32 payload)))
    (.put buf payload)
    (.array buf)))

(defn- read-fully
  "Positional read of n bytes at offset; returns the bytes or nil on a short read."
  ^bytes [^FileChannel ch ^long offset ^long n]
  (let [buf (ByteBuffer/allocate n)]
    (loop [pos offset]
      (if (.hasRemaining buf)
        (let [r (.read ch buf pos)]
          (if (neg? r) nil (recur (+ pos r))))
        (.array buf)))))

(defn read-at
  "Read the frame at offset, not past `end`. Returns
   {:status :ok :event e :next offset'} | {:status :short} (not enough bytes for a whole frame)
   | {:status :bad-crc :next offset'} | {:status :bad-payload :next offset'} (decoded but not a map)."
  [^FileChannel ch ^long offset ^long end]
  (if (> (+ offset header-size) end)
    {:status :short}
    (let [hdr (ByteBuffer/wrap (read-fully ch offset header-size))
          len (.getInt hdr)
          crc (bit-and (.getInt hdr) 0xFFFFFFFF)
          next (+ offset header-size len)]
      (cond
        (or (neg? len) (> len max-payload) (> next end)) {:status :short}
        :else
        (let [payload (read-fully ch (+ offset header-size) len)]
          (if (not= crc (crc32 payload))
            {:status :bad-crc :next next}
            (let [e (try (decode-payload payload) (catch Exception _ ::bad))]
              (if (map? e)
                {:status :ok :event e :next next}
                {:status :bad-payload :next next}))))))))
