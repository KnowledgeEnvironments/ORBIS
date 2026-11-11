(ns knowledge.world.adapters.fs
  "Filesystem Adapter (spec 5.10 binds :filesystem, 6.5 mounts, 12.3). Pure file work for a mount:
   an Adapter bound at a name (`harness`) with a directory locator; `<mount>/<path>` denotes the
   file at that path. `describe` turns a path into the Fields of its Object: file/path, file/kind,
   file/size, file/sha256, file/mime, file/mtime, and the content as file/text (text up to the
   blob threshold), file/bytes (anything larger or binary; the log moves it to a blob), or
   file/entries for a directory. Nothing here appends to the log; knowledge.world.resolve.mount
   materializes the Object as the resolving Principal under its Grant."
  (:require [knowledge.world.adapters.confine :as confine]
            [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.log.blob :as blob])
  (:import [java.io File]
           [java.nio.file Files]
           [java.util Date]))

(def text-threshold blob/threshold)
(def max-range-bytes 16384)
(def max-entries 512)

(defn harness-root
  "The repository root: KE_HARNESS_ROOT (config :harness-root), else the nearest ancestor of the
   working directory holding docs/protocol.md, else nil."
  [{:keys [harness-root]}]
  (if harness-root
    (when (.isDirectory (io/file harness-root)) (.getCanonicalPath (io/file harness-root)))
    (loop [^File d (.getCanonicalFile (io/file "."))]
      (cond (nil? d) nil
            (.exists (io/file d "docs" "protocol.md")) (.getCanonicalPath d)
            :else (recur (.getParentFile d))))))

(defn work-root
  "Where source is edited: the instance's working copy (KE_WORK), else the repository itself."
  [{:keys [work-root] :as config}]
  (or (when work-root (when (.isDirectory (io/file work-root)) (.getCanonicalPath (io/file work-root))))
      (harness-root config)))

(defn- locator-dir ^File [^String locator]
  (io/file (str/replace locator #"^file://" "")))

(defn safe-file
  "The file for a relative path inside the locator, or nil when the path escapes it."
  ^File [locator rel]
  (let [root (.getCanonicalFile (locator-dir locator))
        f (.getCanonicalFile (io/file root rel))
        rp (.getPath root)]
    (when (and (or (= (.getPath f) rp) (.startsWith (.getPath f) (str rp File/separator)))
               ;; what a shell cannot read, a mount does not hand over either
               (not (confine/secret-path? (.getPath f))))
      f)))

(def ^:private mime-by-ext
  {"md" "text/markdown" "txt" "text/plain" "clj" "text/x-clojure" "cljc" "text/x-clojure" "edn" "application/edn"
   "cpp" "text/x-c++src" "hpp" "text/x-c++hdr" "h" "text/x-chdr" "c" "text/x-csrc" "mm" "text/x-objc++src"
   "json" "application/json" "yaml" "text/yaml" "yml" "text/yaml" "sh" "text/x-shellscript" "zsh" "text/x-shellscript"
   "cmake" "text/x-cmake" "py" "text/x-python" "html" "text/html" "svg" "image/svg+xml" "css" "text/css"
   "js" "text/javascript" "ts" "text/typescript" "swift" "text/x-swift" "plist" "text/xml" "xml" "text/xml"
   "toml" "text/toml" "lisp" "text/x-lisp" "png" "image/png" "jpg" "image/jpeg" "jpeg" "image/jpeg"
   "ttf" "font/ttf" "otf" "font/otf" "bmp" "image/bmp" "klog" "application/octet-stream"})

(defn- ext [^File f]
  (let [n (.getName f) i (.lastIndexOf n ".")]
    (when (pos? i) (str/lower-case (subs n (inc i))))))

(defn- looks-text? [^bytes bs]
  (let [n (min (alength bs) 8192)]
    (loop [i 0] (cond (= i n) true (zero? (aget bs i)) false :else (recur (inc i))))))

(defn mime-of [^File f ^bytes bs]
  (or (mime-by-ext (ext f))
      (let [n (.getName f)]
        (cond (#{"Makefile" "CMakeLists.txt" "LICENSE" "README"} n) "text/plain"
              (str/starts-with? n ".") "text/plain"
              (looks-text? bs) "text/plain"
              :else "application/octet-stream"))))

(defn text-mime? [mime]
  (or (str/starts-with? (str mime) "text/") (contains? #{"application/json" "application/edn" "image/svg+xml"} mime)))

(defn- line-count [^String s] (if (str/blank? s) 0 (count (str/split-lines s))))

(defn stat
  "{:size :mtime :kind} for the path, or nil when absent or outside the locator."
  [locator rel]
  (when-let [f (safe-file locator rel)]
    (when (.exists f)
      {:size (.length f) :mtime (Date. (.lastModified f)) :kind (if (.isDirectory f) :directory :file)})))

(defn describe
  "The Fields of a mounted path's Object (see ns doc), or nil when it does not exist."
  [locator rel]
  (when-let [f (safe-file locator rel)]
    (when (.exists f)
      (let [base {:file/path rel :file/mtime (Date. (.lastModified f))}]
        (if (.isDirectory f)
          (let [names (->> (.listFiles f) (map #(str (.getName ^File %) (when (.isDirectory ^File %) "/"))) sort vec)]
            (assoc base :file/kind :directory :file/size (count names)
                   :file/entries (vec (take max-entries names))))
          (let [bs (Files/readAllBytes (.toPath f))
                mime (mime-of f bs)
                text? (text-mime? mime)
                s (when text? (String. bs "UTF-8"))]
            (cond-> (assoc base :file/kind :file :file/size (alength bs) :file/sha256 (blob/sha256-hex bs) :file/mime mime)
              text? (assoc :file/lines (line-count s))
              (and text? (<= (alength bs) text-threshold)) (assoc :file/text s)
              (or (not text?) (> (alength bs) text-threshold)) (assoc :file/bytes bs))))))))

(defn field-type
  "The Field type of a describe entry."
  [k v]
  (case k
    :file/path :string :file/kind :keyword :file/size :number :file/sha256 :string :file/mime :string
    :file/mtime :instant :file/lines :number :file/text :string :file/bytes :bytes
    :file/entries [:list :string]
    (cond (string? v) :string (number? v) :number (keyword? v) :keyword (bytes? v) :bytes :else :string)))

(defn lines-range
  "Lines first..last (1-based, inclusive) of text, capped at max-range-bytes.
   {:text :from :to :of :truncated}."
  [^String text [first last]]
  (let [lines (str/split-lines (str text))
        n (count lines)
        from (max 1 (long (or first 1)))
        to (min n (max from (long (or last n))))
        picked (if (> from n) [] (subvec lines (dec from) to))
        joined (str/join "\n" picked)
        over? (> (count (.getBytes joined "UTF-8")) max-range-bytes)
        joined (if over? (str (subs joined 0 max-range-bytes) "\n[... range truncated at " max-range-bytes " bytes; ask for fewer lines]") joined)]
    {:text joined :from from :to to :of n :truncated over?}))
