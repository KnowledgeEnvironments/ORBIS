(ns knowledge.world.log.blob
  "Content-addressed blob store beside the log (spec 4.3, 12.1, ADR 0006):
   $KE_DATA/blobs/sha256/ab/<64hex>. put! writes tmp, fsyncs, renames. Values of type bytes
   above `threshold` leave the log and the Field holds {:blob/sha256 :blob/size}."
  (:refer-clojure :exclude [get])
  (:require [clojure.java.io :as io])
  (:import [java.io File FileOutputStream]
           [java.nio.file Files StandardCopyOption]
           [java.security MessageDigest]))

(def threshold 4096)

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
     {:root root :fallbacks (mapv #(io/file % "blobs" "sha256") fallbacks)})))

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

(defn put!
  "Store bytes; returns the receipt {:blob/sha256 :blob/size}. Idempotent."
  [{:keys [mem] :as store} ^bytes bs]
  (let [hex (sha256-hex bs)
        f (when-not mem (path store hex))]
    (when mem (swap! mem assoc hex bs))
    (when (and f (not (.exists ^File f)))
      (.mkdirs (.getParentFile f))
      (let [tmp (io/file (.getParentFile f) (str hex ".tmp-" (System/nanoTime)))]
        (with-open [out (FileOutputStream. tmp)]
          (.write out bs)
          (.force (.getChannel out) false))
        (Files/move (.toPath tmp) (.toPath f) (into-array java.nio.file.CopyOption [StandardCopyOption/ATOMIC_MOVE]))))
    {:blob/sha256 hex :blob/size (alength bs)}))

(defn get
  "The bytes for a sha256 hex, or nil."
  ^bytes [{:keys [mem parent] :as store} hex]
  (if mem
    (or (clojure.core/get @mem hex) (get parent hex))
    (some (fn [r] (let [f (path {:root r} hex)] (when (.exists f) (Files/readAllBytes (.toPath f))))) (roots store))))

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
