(ns knowledge.world.index.snapshot
  "Snapshots of the index (spec 7.2: an optimization, never authoritative):
   snapshots/snap-<seq>.snap = Transit of {:seq N :schema-version 1 :datascript (d/serializable db)}.
   Boot loads the newest parseable one and replays the log from (inc N).

   On a pruned log that stops being an optimization: the events below the first segment are gone, so
   the fold needs a snapshot at or past them. anchor/ holds the one the retention sweep verified
   before it dropped any: a copy, outside snapshots/, because `prune!` keeps only the newest three
   and the supervisor moves aside whatever a failed release wrote. Nothing but the sweep touches it."
  (:require [clojure.java.io :as io]
            [cognitect.transit :as transit]
            [datascript.core :as d]
            [taoensso.timbre :as log])
  (:import [java.io ByteArrayInputStream ByteArrayOutputStream File FileInputStream FileOutputStream]
           [java.nio.file Files StandardCopyOption]))

(def schema-version 1)

(defn- freeze [x] (let [o (ByteArrayOutputStream.)] (transit/write (transit/writer o :json) x) (.toString o "UTF-8")))
(defn- thaw [^String s] (transit/read (transit/reader (ByteArrayInputStream. (.getBytes s "UTF-8")) :json)))

(defn anchor-dir ^File [data-dir] (io/file data-dir "anchor"))

(defn seq-of
  "The seq a snapshot file's name carries."
  ^long [^File f]
  (parse-long (second (re-matches #"snap-0*(\d+)\.snap" (.getName f)))))

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

(defn- read-file
  "{:seq :db} of a snapshot this release reads, or nil."
  [^File f]
  (let [m (with-open [in (FileInputStream. f)] (transit/read (transit/reader in :json)))]
    (if (= schema-version (:schema-version m))
      {:seq (:seq m) :db (d/from-serializable (:datascript m) {:thaw-fn thaw})}
      (log/warn "snapshot in another format, skipping" {:file (str f) :format (:schema-version m) :reads schema-version}))))

(defn load-newest
  "{:seq N :db db} from the newest parseable snapshot at or below max-seq, or nil. dirs may be one
   directory or several; min-seq (default 0) is the oldest that is any use, on a pruned log."
  ([dirs max-seq] (load-newest dirs max-seq 0))
  ([dirs max-seq min-seq]
   (loop [files (->> (if (sequential? dirs) dirs [dirs])
                     (mapcat snapshot-files)
                     (filter #(<= (long min-seq) (seq-of %) (long max-seq)))
                     (sort-by seq-of >))]
     (when-let [^File f (first files)]
       (or (try (let [r (read-file f)]
                  (when (and r (<= (long min-seq) (:seq r) (long max-seq))) r))
                (catch Exception e (log/warn "snapshot unreadable, skipping" {:file (str f) :error (ex-message e)}) nil))
           (recur (rest files)))))))

(defn verified
  "{:seq :datoms} when this file really loads as this world: read whole by this release, every datom
   walked, the seq in it the seq in its name, the world's id in it. Else nil. Memory for one
   snapshot, for as long as a boot would take it."
  [^File f world-id]
  (try (let [{at :seq db :db} (read-file f)
             datoms (when db (count (d/datoms db :eavt)))]
         (when (and db (= at (seq-of f)) (pos? datoms) (seq (d/datoms db :avet :world/id world-id)))
           {:seq at :datoms datoms}))
       (catch Exception e (log/warn "snapshot failed its check" {:file (str f) :error (ex-message e)}) nil)))

(defn anchor-seq
  "The seq of the snapshot that holds the world below the log's first segment, or nil."
  [data-dir]
  (some-> (last (snapshot-files (anchor-dir data-dir))) seq-of))

(defn anchor!
  "Make a verified snapshot the anchor: copied, forced to disk and renamed before the old one
   goes, and before the caller drops anything the old one was covering."
  [data-dir ^File f]
  (let [dir (doto (anchor-dir data-dir) (.mkdirs))
        dest (io/file dir (.getName f))
        tmp (io/file dir (str (.getName f) ".tmp"))]
    (Files/copy (.toPath f) (.toPath tmp) (into-array java.nio.file.CopyOption [StandardCopyOption/REPLACE_EXISTING]))
    (with-open [out (FileOutputStream. tmp true)] (.force (.getChannel out) false))
    (Files/move (.toPath tmp) (.toPath dest) (into-array java.nio.file.CopyOption [StandardCopyOption/ATOMIC_MOVE]))
    (doseq [^File old (butlast (snapshot-files dir))] (.delete old))
    dest))

(defn prune!
  "Keep the newest `keep` snapshots."
  [dir keep]
  (doseq [^File f (drop-last keep (snapshot-files dir))] (.delete f)))
