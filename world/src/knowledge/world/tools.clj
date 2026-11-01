(ns knowledge.world.tools
  "Worlds as files (sprint item W9; KEE's worlds on top of the log, spec 7, ADR 0006):

   export  <data-dir> <archive.tgz>        the log, blobs, snapshots and world.edn with a manifest
   import  <archive.tgz> <data-dir>        restore and verify; tokens are never in an archive
   fork    <parent-dir> <child-dir> [--at N]  a child that reads the parent's events 1..N in place
   diff    <child-dir>                      the child's own events, summarised
   promote <child-dir> <parent-dir>         append the child's own events to the parent (unmoved parent only)

   Run as `clojure -M -m knowledge.world.tools <cmd> ...` from world/, or through scripts/world-*.sh.
   Nothing here is state the log does not hold: an archive is the log plus hashes, a fork is a
   reference plus the child's own segments, a promotion is an append."
  (:require [clojure.edn :as edn]
            [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.fold.core :as fold]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.index.snapshot :as snap])
  (:import [java.io File]
           [java.nio.file Files StandardCopyOption]
           [java.util Date]))

(def manifest-version 1)

(defn- rel [^File root ^File f] (str (.relativize (.toPath root) (.toPath f))))

(defn- files-under
  "Every regular file under root, relative paths, excluding token files and temp files."
  [^File root]
  (->> (file-seq root)
       (filter #(.isFile ^File %))
       (remove #(or (str/ends-with? (.getName ^File %) ".token")
                    (str/ends-with? (.getName ^File %) ".tmp")
                    (str/includes? (rel root %) "blobs/tmp/")))
       (map #(rel root %))
       sort
       vec))

(defn- sha256-file ^String [^File f] (blob/sha256-hex (Files/readAllBytes (.toPath f))))

(defn- git-head []
  (try (let [p (.start (ProcessBuilder. ["git" "rev-parse" "--short" "HEAD"]))
             out (slurp (.getInputStream p))]
         (.waitFor p)
         (str/trim out))
       (catch Exception _ nil)))

(defn- log-head
  "The head seq of a world directory, read without a writer or any write (the world may be live)."
  [^File data-dir]
  (log/peek-head data-dir))

(defn- sh!
  "Run a command, throw with its output on failure."
  [& cmd]
  (let [p (.start (doto (ProcessBuilder. ^java.util.List (vec cmd)) (.redirectErrorStream true)))
        out (slurp (.getInputStream p))
        code (.waitFor p)]
    (when-not (zero? code) (throw (ex-info "command failed" {:cmd (vec cmd) :code code :out out})))
    out))

(defn manifest
  "The manifest of a world directory: id, head, every file with its sha256."
  [^File data-dir]
  (let [edn-file (io/file data-dir "world.edn")
        wedn (edn/read-string (slurp edn-file))]
    {:manifest-version manifest-version
     :world/id (:world/id wedn)
     :world/edn wedn
     :head (log-head data-dir)
     :created-at (Date.)
     :code (git-head)
     :files (into {} (for [r (files-under data-dir)] [r (sha256-file (io/file data-dir r))]))}))

;; ------------------------------------------------------------------ export / import

(defn export!
  "Write archive (a .tgz) holding world.edn, log/, blobs/, snapshots/ and manifest.edn. Tokens and
   temp files are left out. The world should be stopped; with :live true the active segment is
   copied as it stands and a torn tail, if any, is cut on the first boot of the import."
  [data-dir archive & {:keys [live]}]
  (let [root (.getCanonicalFile (io/file data-dir))
        m (manifest root)
        out (.getCanonicalFile (io/file archive))
        stage (.toFile (Files/createTempDirectory "ke-export" (make-array java.nio.file.attribute.FileAttribute 0)))]
    (.mkdirs (.getParentFile out))
    (try
      ;; stage the exact files the manifest names (hashes were taken from these bytes), then tar
      (doseq [r (keys (:files m))]
        (let [src (io/file root r) dst (io/file stage r)]
          (.mkdirs (.getParentFile dst))
          (Files/copy (.toPath src) (.toPath dst) (into-array java.nio.file.CopyOption [StandardCopyOption/REPLACE_EXISTING]))))
      (spit (io/file stage "manifest.edn") (pr-str (assoc m :live (boolean live))))
      (sh! "tar" "-czf" (str out) "-C" (str stage) ".")
      {:archive (str out) :files (count (:files m)) :head (:head m) :world (:world/id m)}
      (finally
        (doseq [f (reverse (file-seq stage))] (.delete ^File f))))))

(defn import!
  "Restore an archive into data-dir (which must not already hold a world). Every file's sha256 is
   checked against the manifest. Returns the manifest. Tokens are re-minted on the world's first
   boot (knowledge.world.boot.kernel-zero/remint-tokens!)."
  [archive data-dir]
  (let [root (io/file data-dir)]
    (when (.exists (io/file root "world.edn"))
      (throw (ex-info "data dir already holds a world" {:data-dir (str root)})))
    (.mkdirs root)
    (sh! "tar" "-xzf" (str (.getCanonicalFile (io/file archive))) "-C" (str root))
    (let [mf (io/file root "manifest.edn")
          _ (when-not (.exists mf) (throw (ex-info "archive has no manifest" {:archive (str archive)})))
          m (edn/read-string (slurp mf))
          bad (for [[r h] (:files m)
                    :let [f (io/file root r)]
                    :when (or (not (.exists f)) (not= h (sha256-file f)))]
                r)]
      (when (seq bad)
        (throw (ex-info "hash mismatch" {:files (vec bad)})))
      ;; tokens are never in an archive; the first boot mints new ones
      (doseq [^File f (.listFiles root) :when (str/ends-with? (.getName f) ".token")] (.delete f))
      (.delete mf)
      m)))

;; ------------------------------------------------------------------ fork

(defn fork!
  "Make child-dir a fork of parent-dir at seq N (default: the parent's head). Writes world.edn with
   the same world/id, a fork id and the parent reference, copies the parent's newest snapshot at or
   below N (a cache), and nothing else: the child reads the parent's log in place."
  [parent-dir child-dir & {:keys [at]}]
  (let [parent (.getCanonicalFile (io/file parent-dir))
        child (.getCanonicalFile (io/file child-dir))
        pedn (edn/read-string (slurp (io/file parent "world.edn")))
        head (log-head parent)
        at (long (or at head))]
    (when (.exists (io/file child "world.edn"))
      (throw (ex-info "child dir already holds a world" {:child (str child)})))
    (when (> at head) (throw (ex-info "fork seq beyond the parent's head" {:at at :head head})))
    (.mkdirs child)
    (let [fork-id (random-uuid)
          wedn {:world/id (:world/id pedn) :format-version 1 :created-at (Date.)
                :fork/id fork-id
                :parent {:data-dir (str parent) :world (:world/id pedn) :seq at}}]
      (spit (io/file child "world.edn") (pr-str wedn))
      (.mkdirs (io/file child "log"))
      (.mkdirs (io/file child "blobs" "sha256"))
      (.mkdirs (io/file child "snapshots"))
      ;; the nearest parent snapshot at or below N, copied as the child's own cache
      (when-let [^File sf (->> (snap/snapshot-files (io/file parent "snapshots"))
                               (filter #(<= (parse-long (second (re-matches #"snap-0*(\d+)\.snap" (.getName ^File %)))) at))
                               last)]
        (Files/copy (.toPath sf) (.toPath (io/file child "snapshots" (.getName sf)))
                    (into-array java.nio.file.CopyOption [StandardCopyOption/REPLACE_EXISTING])))
      {:child (str child) :parent (str parent) :world (:world/id pedn) :fork fork-id :at at})))

;; ------------------------------------------------------------------ diff / promote

(defn own-events
  "A fork's own events (seq > the fork seq), from its directory, without a world running."
  [child-dir]
  (let [child (.getCanonicalFile (io/file child-dir))
        wedn (edn/read-string (slurp (io/file child "world.edn")))
        parent (:parent wedn)
        _ (when-not parent (throw (ex-info "not a fork" {:child (str child)})))
        l (log/open! child {:fsync :never :parent (update parent :data-dir #(let [f (io/file %)] (if (.isAbsolute f) f (io/file child %))))})]
    (try
      (loop [acc [] from (inc (long (:seq parent)))]
        (let [batch (log/read-own-from l from 1000)]
          (if (empty? batch) {:events acc :fork-id (:fork/id wedn) :at (:seq parent) :parent parent}
              (recur (into acc batch) (inc (:event/seq (peek batch)))))))
      (finally (log/close! l)))))

(defn diff
  "A summary of what a fork changed."
  [child-dir]
  (let [{:keys [events at fork-id]} (own-events child-dir)]
    {:forked-at at :fork fork-id :events (count events)
     :by-kind (frequencies (map :event/kind events))
     :objects-created (count (filter #(= :created (:event/kind %)) events))
     :fields-set (count (filter #(= :field-set (:event/kind %)) events))}))

(def not-promoted
  "Fields a fork sets about itself that must not reach the parent: credentials (the child minted
   its own tokens; the parent keeps its own) and the fork marks."
  #{:principal/token-hash :world/parent :world/forked-at :world/fork-id})

(defn promotable?
  "Every event of a fork travels except a field-set of a Field in `not-promoted`."
  [e]
  (not (and (= :field-set (:event/kind e)) (contains? not-promoted (get-in e [:event/body :name])))))

(defn promote!
  "Append the child's own events to the parent's log as new seqs, each marked event/from-fork and
   event/fork-seq; the child's token hashes and fork marks stay behind. Refuses when the parent
   moved past the fork seq (no three-way merge in v1) or when a parent world is running (its
   directory must be quiet: stop it first)."
  [child-dir parent-dir]
  (let [{:keys [events at fork-id]} (own-events child-dir)
        events (filterv promotable? events)
        parent (.getCanonicalFile (io/file parent-dir))
        head (log-head parent)]
    (when (not= head (long at))
      (throw (ex-info "parent moved since the fork; promotion refused (no three-way merge in v1)"
                      {:forked-at at :parent-head head})))
    (let [l (log/open! parent {:fsync :batch})]
      (try
        (let [seqs (mapv (fn [e] (log/append!! l (-> e (dissoc :event/seq)
                                                      (assoc :event/from-fork (str fork-id) :event/fork-seq (:event/seq e)))))
                         events)]
          {:promoted (count seqs) :from-fork fork-id :first-seq (first seqs) :last-seq (peek seqs) :parent (str parent)})
        (finally (log/close! l))))))

(defn fold-at
  "The fold of a world directory at its head: a db value, for equality checks in tests."
  [data-dir]
  (let [root (.getCanonicalFile (io/file data-dir))
        wedn (edn/read-string (slurp (io/file root "world.edn")))
        parent (some-> (:parent wedn) (update :data-dir #(let [f (io/file %)] (if (.isAbsolute f) f (io/file root %)))))
        l (log/open! root (cond-> {:fsync :never} parent (assoc :parent parent)))]
    (try
      (loop [db (fold/empty-db) from 1]
        (let [batch (log/read-from l from 1000)]
          (if (empty? batch) db (recur (fold/apply-events db batch) (inc (:event/seq (peek batch)))))))
      (finally (log/close! l)))))

(defn -main [& [cmd & args]]
  (let [opt (fn [k] (let [i (.indexOf ^java.util.List (vec args) k)] (when (>= i 0) (nth args (inc i) nil))))
        pos (vec (remove #(or (str/starts-with? % "--") (= % (opt "--at"))) args))]
    (case cmd
      "export" (prn (export! (pos 0) (pos 1) :live (some #{"--live"} args)))
      "import" (prn (select-keys (import! (pos 0) (pos 1)) [:world/id :head :created-at :code]))
      "fork" (prn (fork! (pos 0) (pos 1) :at (some-> (opt "--at") parse-long)))
      "diff" (prn (diff (pos 0)))
      "promote" (prn (promote! (pos 0) (pos 1)))
      (do (println "usage: export <data-dir> <archive.tgz> [--live] | import <archive.tgz> <data-dir> | fork <parent-dir> <child-dir> [--at N] | diff <child-dir> | promote <child-dir> <parent-dir>")
          (System/exit 2)))
    (shutdown-agents)))
