(ns knowledge.world.log.blob
  "Content-addressed blob store beside the log (spec 4.3, 12.1, ADR 0006):
   $KE_DATA/blobs/sha256/ab/<64hex>. put! writes tmp, fsyncs, renames. Values of type bytes
   above `threshold` leave the log and the Field holds {:blob/sha256 :blob/size}.

   A file's mtime is when it was last used, for the retention sweep (docs/log-format.md Retention):
   get and put! both mark it, so the sweep evicts the least recently used. A blob the sweep took
   reads as nil, which is what every reader already handles, and the receipt on the log stays."
  (:refer-clojure :exclude [get])
  (:require [clojure.java.io :as io]
            [clojure.string :as str])
  (:import [java.io File FileOutputStream]
           [java.nio.file Files NoSuchFileException StandardCopyOption]
           [java.security MessageDigest]))

(def threshold 4096)
(def touch-every-ms "A read marks use at most this often, so reads do not become writes." (* 60 60 1000))

(defn sha256-hex ^String [^bytes bs]
  (let [d (.digest (MessageDigest/getInstance "SHA-256") bs)]
    (apply str (map #(format "%02x" (bit-and % 0xff)) d))))

(defn open
  "The store under data-dir; `fallbacks` are further data dirs read when a hash is not here
   (a fork resolves blobs child-then-parent; writes go to the child only)."
  ([data-dir] (open data-dir []))
  ([data-dir fallbacks]
   (let [root (io/file data-dir "blobs" "sha256")]
     (.mkdirs root)
     ;; lock: a put! that finds a blob present and an eviction of it must not interleave
     {:root root :fallbacks (mapv #(io/file % "blobs" "sha256") fallbacks) :lock (Object.)})))

(defn in-memory
  "A store that never touches a disk (an off-the-record session): its bytes live in an atom and
   die with it; reads fall back to `parent`, a store it never writes."
  [parent]
  {:mem (atom {}) :parent parent})

(defn- roots [{:keys [root fallbacks]}] (cons root fallbacks))

(defn- path ^File [{:keys [^File root]} ^String hex]
  (io/file root (subs hex 0 2) hex))

(declare exists?)
(defn- exists-on-disk? [store hex] (boolean (some #(.exists (path {:root %} hex)) (roots store))))
(defn exists? [{:keys [mem parent] :as store} hex]
  (if mem (or (contains? @mem hex) (exists? parent hex)) (exists-on-disk? store hex)))

(defn- used!
  "Mark a file used. Under the store's lock, so an eviction that read the old mtime leaves it alone.
   False when the file is gone."
  [{:keys [lock]} ^File f]
  (locking lock
    (and (.exists f) (.setLastModified f (System/currentTimeMillis)))))

(defn put!
  "Store bytes; returns the receipt {:blob/sha256 :blob/size}. Idempotent, and bytes the sweep took
   are simply stored again."
  [{:keys [mem] :as store} ^bytes bs]
  (let [hex (sha256-hex bs)
        f (when-not mem (path store hex))]
    (when mem (swap! mem assoc hex bs))
    (when (and f (not (used! store f)))
      (.mkdirs (.getParentFile f))
      (let [tmp (io/file (.getParentFile f) (str hex ".tmp-" (System/nanoTime)))]
        (with-open [out (FileOutputStream. tmp)]
          (.write out bs)
          (.force (.getChannel out) false))
        (Files/move (.toPath tmp) (.toPath f) (into-array java.nio.file.CopyOption [StandardCopyOption/ATOMIC_MOVE]))))
    {:blob/sha256 hex :blob/size (alength bs)}))

(defn- read-file
  "The file's bytes, or nil when it is gone, which the sweep may do between any two calls."
  ^bytes [^File f]
  (try (Files/readAllBytes (.toPath f)) (catch NoSuchFileException _ nil)))

(defn- touch!
  "A read marks use only when the mark is old. A fork's parent is never written."
  [store ^File f]
  (when (> (- (System/currentTimeMillis) (.lastModified f)) touch-every-ms)
    (used! store f)))

(defn get
  "The bytes for a sha256 hex, or nil: never stored, taken by the retention sweep, or no hash at all
   (a sender names one, and a pruned event may leave a reader none)."
  ^bytes [{:keys [mem parent ^File root] :as store} hex]
  (cond
    mem (or (clojure.core/get @mem hex) (get parent hex))
    (and (string? hex) (re-matches #"[0-9a-f]{64}" hex))
    (some (fn [r] (let [f (path {:root r} hex)]
                    (when-let [bs (read-file f)]
                      (when (identical? r root) (touch! store f))
                      bs)))
          (roots store))))

(defn inventory
  "What this store holds on its own disk, never a fork's parent: [{:hex :size :mtime}]. Files
   in flight are left out."
  [{:keys [^File root]}]
  (vec (for [^File d (.listFiles root) :when (.isDirectory d)
             ^File f (.listFiles d) :when (not (str/includes? (.getName f) ".tmp-"))]
         {:hex (.getName f) :size (.length f) :mtime (.lastModified f)})))

(defn evict!
  "Delete a blob the inventory saw, unless it has been used since. The bytes freed, or nil."
  [{:keys [lock] :as store} {:keys [hex size mtime]}]
  (let [^File f (path store hex)]
    (locking lock
      (when (and (= mtime (.lastModified f)) (Files/deleteIfExists (.toPath f)))
        size))))

(defn drop-stale-tmp!
  "Temp files a crash left behind, older than max-age-ms. How many went."
  [{:keys [^File root]} max-age-ms]
  (let [cut (- (System/currentTimeMillis) max-age-ms)]
    (count (for [^File d (.listFiles root) :when (.isDirectory d)
                 ^File f (.listFiles d)
                 :when (and (str/includes? (.getName f) ".tmp-") (< (.lastModified f) cut) (.delete f))]
             f))))

(defn receipt? [v] (and (map? v) (string? (:blob/sha256 v)) (int? (:blob/size v))))

(defn externalize
  "Walk a value; every byte array above the threshold is put! and replaced by its receipt.
   Returns [value' replaced?]."
  [store v]
  (let [hit (volatile! false)
        walk (fn walk [x]
               (cond (and (bytes? x) (> (alength ^bytes x) threshold)) (do (vreset! hit true) (put! store x))
                     (map? x) (into {} (map (fn [[k v]] [k (walk v)])) x)
                     (vector? x) (mapv walk x)
                     (set? x) (into #{} (map walk) x)
                     :else x))
        v' (walk v)]
    [v' @hit]))
