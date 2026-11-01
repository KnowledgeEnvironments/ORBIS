(ns knowledge.world.adapters.harness
  "The harness index (decisions 2026-10-30 'root knows the harness'): a compact map of the
   repository root mounts as `harness`, written as the Field harness/index on the Principal that
   asks for it, with harness/head (the git commit and dirty flag). Built from `git ls-files` and
   file sizes, grouped by top-level directory, with the first heading of each docs/*.md; noisy
   trees (docs/history, listener/generated, listener/assets) are counted, not listed. Capped at
   `index-cap` bytes by collapsing the largest groups to one line each, the core of the harness
   (docs, world, schema, the root files) last."
  (:require [clojure.edn :as edn]
            [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.adapters.confine :as confine]))

(def index-cap
  "Bytes the index Field may hold. 12 KiB since the repository passed 360 tracked files, 16 KiB at 640:
   at 8 KiB the world's own source group collapsed and root could no longer find loop/core.clj by
   name. The Field's size is not its prompt cost: the serializer clips a standing Field at
   serialize/self-field-cap (2000 chars) and the model reads the rest on demand."
  16384)
(def summarized-prefixes ["docs/history/" "listener/generated/" "listener/assets/" "docs/figures/"])
(def core-groups #{"." "docs" "world" "schema"})

(def ^:dynamic *data-dir*
  "The data dir of the world asking. Bound, git runs in the box."
  nil)

(defn git
  "Run git in root. {:exit :out}. A repository's own config can run programs: core.fsmonitor on any
   status, a filter's clean on a changed file. A shell can write that config and the world reads the
   repository, so for a world git runs in the box the shell is in (decisions 2026-10-31). On a host
   with no sandbox a shell cannot start either, so there is no such config to fear."
  [root & args]
  (let [box (when (and *data-dir* (confine/primitive))
              (confine/spec {:root root :data-dir *data-dir* :adapter "harness-git" :network "none"}))
        argv (into ["git" "-c" "core.fsmonitor=false"] args)
        pb (doto (ProcessBuilder. ^java.util.List (if box (confine/command box argv) argv))
             (.directory (io/file root))
             (.redirectErrorStream true))
        _ (when box
            (doto (.environment pb) (.clear) (.putAll (confine/environment box))))
        p (.start pb)
        out (slurp (.getInputStream p))]
    {:exit (.waitFor p) :out out}))

(defn head
  "\"<short sha> clean|dirty\", \"<short sha> release\" for a release tree, or \"no git\"."
  [root]
  (let [{:keys [exit out]} (git root "rev-parse" "--short" "HEAD")
        manifest (io/file root "release.edn")]
    (cond
      (.isFile manifest) (str (let [c (str (:commit (edn/read-string (slurp manifest))))] (subs c 0 (min 7 (count c)))) " release")
      (zero? exit) (str (str/trim out) (if (str/blank? (:out (git root "status" "--porcelain"))) " clean" " dirty"))
      :else "no git")))

(defn tracked-files
  "[[path bytes] ...] from git ls-files, or from a walk when git is unavailable."
  [root]
  (let [{:keys [exit out]} (git root "ls-files" "-z")
        paths (if (zero? exit)
                (remove str/blank? (str/split out #"\u0000"))
                (->> (file-seq (io/file root)) (filter #(.isFile ^java.io.File %))
                     (map #(subs (.getPath ^java.io.File %) (inc (count root))))
                     (remove #(str/starts-with? % ".git/"))))]
    (vec (for [p paths :let [f (io/file root p)] :when (.isFile f)] [p (.length f)]))))

(defn first-heading [^java.io.File f]
  (with-open [r (io/reader f)]
    (some #(when (str/starts-with? % "#") (str/trim (str/replace % #"^#+\s*" ""))) (take 40 (line-seq r)))))

(defn- group-key [path]
  (or (some #(when (str/starts-with? path %) (subs % 0 (dec (count %)))) summarized-prefixes)
      (let [i (str/index-of path "/")] (if i (subs path 0 i) "."))))

(defn- summarized? [k] (some #(= k (subs % 0 (dec (count %)))) summarized-prefixes))

(defn- fmt-bytes [n] (cond (>= n 1048576) (format "%.1f MB" (/ n 1048576.0)) (>= n 1024) (format "%d KB" (quot n 1024)) :else (str n)))

(defn- group-lines [root k files collapsed?]
  (let [total (reduce + (map second files))]
    (if (or collapsed? (summarized? k))
      [(str "## " k ": " (count files) " files, " (fmt-bytes total) " (not listed)")]
      (into [(str "## " k " (" (count files) " files, " (fmt-bytes total) ")")]
            (for [[p n] (sort files)]
              (str p " " n
                   (when (and (str/starts-with? p "docs/") (str/ends-with? p ".md"))
                     (when-let [h (first-heading (io/file root p))] (str "  # " h)))))))))

(defn index-text
  "The index as text, under index-cap bytes."
  [root]
  (let [files (tracked-files root)
        groups (group-by (comp group-key first) files)
        order (sort-by (fn [k] [(if (= k ".") 0 1) k]) (keys groups))
        render (fn [collapsed]
                 (str/join "\n" (into [(str "# harness index: " (count files) " tracked files at " (head root) "; paths are names under the mount `harness` (read {name \"harness/<path>\"})")]
                                      (mapcat #(group-lines root % (groups %) (collapsed %)) order))))
        by-size (sort-by (fn [k] [(if (core-groups k) 1 0) (- (count (groups k)))]) (remove summarized? order))]
    (loop [collapsed #{} candidates by-size]
      (let [text (render collapsed)]
        (if (or (<= (count (.getBytes ^String text "UTF-8")) index-cap) (empty? candidates))
          (if (> (count (.getBytes ^String text "UTF-8")) index-cap) (str (subs text 0 (- index-cap 40)) "\n[... index truncated]") text)
          (recur (conj collapsed (first candidates)) (rest candidates)))))))
