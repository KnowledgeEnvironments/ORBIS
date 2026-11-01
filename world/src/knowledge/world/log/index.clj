(ns knowledge.world.log.index
  "Per-segment sidecar index (ADR 0006), a cache:
   {:first-seq :last-seq :count :file-size :streams {stream-id [[seq offset] ...]} :sparse [[seq offset] ...]}
   with a sparse entry every 256th frame. Missing or stale (file-size differs) -> rebuilt by scan."
  (:refer-clojure :exclude [read])
  (:require [clojure.java.io :as io]
            [cognitect.transit :as transit]
            [knowledge.world.log.segment :as seg])
  (:import [java.io File FileInputStream FileOutputStream]))

(def sparse-every 256)

(defn empty-index [first-seq]
  {:first-seq first-seq :last-seq (dec first-seq) :count 0 :file-size seg/header-size :streams {} :sparse []})

(defn streams-of
  "Every stream an event belongs on: event/stream plus event/streams."
  [event]
  (into #{(:event/stream event)} (:event/streams event)))

(defn add-frame
  "Index one frame at offset; `end` is the segment end after the frame."
  [idx event ^long offset ^long end]
  (let [s (:event/seq event)
        idx (-> idx
                (assoc :last-seq s :file-size end)
                (update :count inc))
        idx (reduce (fn [i sid] (update-in i [:streams sid] (fnil conj []) [s offset])) idx (streams-of event))]
    (if (zero? (mod (:count idx) sparse-every))
      (update idx :sparse conj [s offset])
      idx)))

(defn write!
  "Persist the index beside its segment (tmp + rename)."
  [^File segment-file idx]
  (let [f (seg/idx-file segment-file)
        tmp (io/file (.getParentFile f) (str (.getName f) ".tmp"))]
    (with-open [out (FileOutputStream. tmp)]
      (transit/write (transit/writer out :json) idx))
    (.renameTo tmp f)))

(defn read
  "The persisted index for a segment if present, parseable and matching the file size; else nil."
  [^File segment-file]
  (let [f (seg/idx-file segment-file)]
    (when (.exists f)
      (try (let [idx (with-open [in (FileInputStream. f)] (transit/read (transit/reader in :json)))]
             (when (and (map? idx) (= (:file-size idx) (.length segment-file)))
               idx))
           (catch Exception _ nil)))))

(defn start-offset
  "The greatest sparse offset at or before seq s, or the header for a cold start."
  ^long [idx ^long s]
  (or (->> (:sparse idx) (take-while (fn [[sq _]] (<= sq s))) last second)
      seg/header-size))

(defn stream-positions
  "[[seq offset] ...] for events of stream after seq `after`."
  [idx stream ^long after]
  (->> (get-in idx [:streams stream]) (drop-while (fn [[sq _]] (<= sq after)))))
