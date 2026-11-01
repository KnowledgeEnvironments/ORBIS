(ns knowledge.world.index.snapshot
  "Snapshots of the index (spec 7.2: an optimization, never authoritative):
   snapshots/snap-<seq>.snap = Transit of {:seq N :schema-version 1 :datascript (d/serializable db)}.
   Boot loads the newest parseable one and replays the log from (inc N)."
  (:require [clojure.java.io :as io]
            [cognitect.transit :as transit]
            [datascript.core :as d]
            [taoensso.timbre :as log])
  (:import [java.io ByteArrayInputStream ByteArrayOutputStream File FileInputStream FileOutputStream]))

(def schema-version 1)

(defn- freeze [x] (let [o (ByteArrayOutputStream.)] (transit/write (transit/writer o :json) x) (.toString o "UTF-8")))
(defn- thaw [^String s] (transit/read (transit/reader (ByteArrayInputStream. (.getBytes s "UTF-8")) :json)))

(defn snapshot-files [dir]
  (->> (.listFiles (io/file dir))
       (filter #(re-matches #"snap-\d{20}\.snap" (.getName ^File %)))
       (sort-by #(.getName ^File %))
       vec))

(defn write!
  "Write the snapshot for db at seq. Returns the file."
  [dir ^long seq db]
  (.mkdirs (io/file dir))
  (let [f (io/file dir (format "snap-%020d.snap" seq))
        tmp (io/file dir (str (.getName f) ".tmp"))]
    (with-open [out (FileOutputStream. tmp)]
      (transit/write (transit/writer out :json)
                     {:seq seq :schema-version schema-version :datascript (d/serializable db {:freeze-fn freeze})}))
    (.renameTo tmp f)
    (log/info "snapshot written" {:file (str f) :seq seq})
    f))

(defn load-newest
  "{:seq N :db db} from the newest parseable snapshot at or below max-seq, or nil."
  [dir max-seq]
  (loop [files (reverse (snapshot-files dir))]
    (when-let [^File f (first files)]
      (or (try (let [m (with-open [in (FileInputStream. f)] (transit/read (transit/reader in :json)))]
                 (when (and (= schema-version (:schema-version m)) (<= (:seq m) max-seq))
                   {:seq (:seq m) :db (d/from-serializable (:datascript m) {:thaw-fn thaw})}))
               (catch Exception e (log/warn "snapshot unreadable, skipping" {:file (str f) :error (ex-message e)}) nil))
          (recur (rest files))))))

(defn prune!
  "Keep the newest `keep` snapshots."
  [dir keep]
  (doseq [^File f (drop-last keep (snapshot-files dir))] (.delete f)))
