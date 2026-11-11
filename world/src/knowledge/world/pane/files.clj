(ns knowledge.world.pane.files
  "A file manager in the world, on an fs Adapter: its place and selection are pane/* Fields, every
   path passes fs/safe-file, and a read-only mount refuses the verbs that change anything."
  (:require [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.pane.core :as core])
  (:import [java.io File]
           [java.nio.file Files StandardCopyOption]))

(def listing-cap 500)

(defn- err [code message] (act/err code message))

(defn- join-rel [cwd path]
  (cond (str/blank? path) cwd
        (str/starts-with? path "/") (subs path 1)
        (str/blank? cwd) path
        :else (str cwd "/" path)))

(defn- entry [^File f]
  (cond-> {:name (.getName f) :kind (if (.isDirectory f) "directory" "file")
           :modified (str (java.time.Instant/ofEpochMilli (.lastModified f)))}
    (.isFile f) (assoc :size (.length f))))

(defn listing
  "A directory's entries, directories first, clipped to listing-cap."
  [^File dir]
  (let [es (sort-by (fn [^File f] [(not (.isDirectory f)) (.getName f)]) (.listFiles dir))]
    {:entries (mapv entry (take listing-cap es)) :total (count es)}))

(defn- target-file
  "The File for a path relative to the pane's place, or nil outside the mount."
  ^File [locator cwd path]
  (fs/safe-file locator (join-rel cwd (str path))))

(defn- relative [locator ^File f]
  (let [root (.getPath (fs/safe-file locator "."))
        p (.getPath f)]
    (if (= p root) "" (subs p (inc (count root))))))

(defn read-listing
  "The listing at the pane's place."
  [world id]
  (let [db (act/db world)
        locator (core/field db (core/field db id :pane/target) :adapter/locator)
        cwd (or (core/field db id :pane/cwd) "")
        dir (target-file locator cwd "")]
    (assoc (listing dir) :cwd cwd :selection (or (core/field db id :pane/selection) []))))

(defn- done [world id result]
  {:ok {:result (assoc result :pane id) :seq (core/applied-seq world)}})

(defn- changed!
  "Tell subscribers the place or its contents moved on."
  [world id state]
  (core/push! world id {:state state}))

(defn- in-mount
  "(f file) for a path inside the mount, else err outside-mount."
  [locator cwd path f]
  (if-let [file (target-file locator cwd path)]
    (f file)
    (err :outside-mount (str path " is outside this mount"))))

(defn- moved-to
  "from to to with the copy or move, refusing what is already there."
  [locator cwd {:keys [from to]} op]
  (in-mount locator cwd from
            (fn [^File a]
              (in-mount locator cwd to
                        (fn [^File b]
                          (cond (not (.exists a)) (err :invalid-args (str from " does not exist"))
                                (.exists b) (err :conflict (str to " already exists"))
                                :else (do (op (.toPath a) (.toPath b)) {:from from :to to})))))))

(def ^:private no-options (into-array java.nio.file.CopyOption []))
(def ^:private copy-attrs (into-array java.nio.file.CopyOption [StandardCopyOption/COPY_ATTRIBUTES]))

(defn frob!
  "A files verb. open comes back as {:open file-path} for the caller to make the pane."
  [world _p id verb args]
  (let [db (act/db world)
        adapter (core/field db id :pane/target)
        locator (core/field db adapter :adapter/locator)
        cwd (or (core/field db id :pane/cwd) "")
        result (fn [r] (if (:err r) r (do (changed! world id {:cwd cwd :changed (name verb)}) (done world id r))))]
    (case verb
      :cd (in-mount locator cwd (:path args)
                    (fn [^File d]
                      (if-not (.isDirectory d)
                        (err :invalid-args (str (:path args) " is not a directory"))
                        (let [rel (relative locator d)]
                          (core/set-field! world id :pane/cwd :string rel)
                          (changed! world id {:cwd rel})
                          (done world id {:cwd rel})))))
      :list (in-mount locator cwd (:path args)
                      (fn [^File d]
                        (if (.isDirectory d)
                          (done world id (assoc (listing d) :cwd (relative locator d)))
                          (err :invalid-args (str (:path args) " is not a directory")))))
      :select (let [names (vec (map str (or (:names args) (when (:name args) [(:name args)]))))]
                (core/set-field! world id :pane/selection [:list :string] names)
                (changed! world id {:selection names})
                (done world id {:selection names}))
      :open (in-mount locator cwd (:name args)
                      (fn [^File f]
                        (if (.exists f)
                          {:open (relative locator f) :adapter adapter}
                          (err :invalid-args (str (:name args) " does not exist")))))
      :mkdir (in-mount locator cwd (:name args)
                       (fn [^File d]
                         (cond (.exists d) (err :conflict (str (:name args) " already exists"))
                               (.mkdirs d) (result {:made (:name args)})
                               :else (err :invalid (str "could not make " (:name args))))))
      :rename (result (moved-to locator cwd args (fn [a b] (Files/move a b no-options))))
      :move (result (moved-to locator cwd args (fn [a b] (Files/move a b no-options))))
      :copy (result (moved-to locator cwd args (fn [a b] (Files/copy ^java.nio.file.Path a ^java.nio.file.Path b ^"[Ljava.nio.file.CopyOption;" copy-attrs))))
      :delete (in-mount locator cwd (:name args)
                        (fn [^File f]
                          (cond (not (.exists f)) (err :invalid-args (str (:name args) " does not exist"))
                                (and (.isDirectory f) (seq (.list f)))
                                (err :conflict (str (:name args) " is not empty; delete what is in it first"))
                                (.delete f) (result {:deleted (:name args)})
                                :else (err :invalid (str "could not delete " (:name args))))))
      (err :invalid-args (str "a file pane has no verb " (name verb))))))

(def writes "Verbs that change the mount." #{:mkdir :rename :copy :move :delete})

(defn touched-paths
  "The relative paths a write verb names, for the Grant check."
  [cwd verb args]
  (mapv #(join-rel cwd (str %))
        (case verb
          (:rename :copy :move) [(:from args) (:to args)]
          [(:name args)])))
