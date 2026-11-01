(ns knowledge.world.log.segment
  "Segment files (ADR 0006): $KE_DATA/log/seg-<first-seq, 20 digits>.klog with a 16-byte header
   \"KLOG\" u16 version=1 u16 flags u64 first-seq, then frames. Roll at 64 MiB.
   Recovery (open): a short read or a CRC mismatch at the tail truncates to the last good frame;
   a CRC mismatch not at the tail throws and refuses to start."
  (:require [clojure.java.io :as io]
            [knowledge.world.log.frame :as frame]
            [taoensso.timbre :as log])
  (:import [java.io File FileNotFoundException RandomAccessFile]
           [java.nio ByteBuffer]
           [java.nio.channels ClosedChannelException FileChannel]
           [java.nio.file Files StandardOpenOption]))

(def header-size 16)
(def magic (.getBytes "KLOG" "US-ASCII"))
(def version 1)
(def roll-bytes (* 64 1024 1024))

(defn file-name [^long first-seq] (format "seg-%020d.klog" first-seq))
(defn idx-file ^File [^File f] (io/file (.getParentFile f) (str (.getName f) ".idx")))

(defn segment-files
  "All .klog files in dir sorted by first-seq (name order)."
  [dir]
  (->> (.listFiles (io/file dir))
       (filter #(re-matches #"seg-\d{20}\.klog" (.getName ^File %)))
       (sort-by #(.getName ^File %))
       vec))

(defn first-seq-of [^File f] (parse-long (second (re-matches #"seg-0*(\d+)\.klog" (.getName f)))))

(defn open-channel ^FileChannel [^File f]
  (.getChannel (RandomAccessFile. f "rw")))

(defn open-channel-ro
  "A read-only channel: a parent world's segment is never written or truncated by a fork."
  ^FileChannel [^File f]
  (.getChannel (RandomAccessFile. f "r")))

(defn create!
  "Create a new segment whose first event will carry first-seq. Returns {:file :channel :first-seq :end}."
  [dir ^long first-seq]
  (let [f (io/file dir (file-name first-seq))
        _ (when (.exists f) (throw (ex-info "segment exists" {:file (str f)})))
        ch (open-channel f)
        buf (ByteBuffer/allocate header-size)]
    (.put buf ^bytes magic)
    (.putShort buf (short version))
    (.putShort buf (short 0))
    (.putLong buf first-seq)
    (.flip buf)
    (.write ch buf 0)
    (.force ch false)
    {:file f :channel ch :reader (atom nil) :first-seq first-seq :end header-size}))

(defn read-header [^FileChannel ch ^File f]
  (let [buf (ByteBuffer/allocate header-size)]
    (when (< (.read ch buf 0) header-size)
      (throw (ex-info "segment header short" {:file (str f)})))
    (.flip buf)
    (let [m (byte-array 4)] (.get buf m)
      (when-not (java.util.Arrays/equals m ^bytes magic)
        (throw (ex-info "segment magic mismatch" {:file (str f)}))))
    (let [v (.getShort buf) _flags (.getShort buf) fs (.getLong buf)]
      (when (not= v version) (throw (ex-info "segment version unsupported" {:file (str f) :version v})))
      {:first-seq fs})))

(defn scan
  "Walk every frame from the header to `end`, calling (on-frame event offset next-offset) per good frame.
   Returns {:end last-good-offset :count n :last-seq s :truncated? bool}. Throws on a bad
   frame that is not at the tail (bytes follow it)."
  [^FileChannel ch ^File f first-seq end on-frame]
  (loop [offset header-size expected first-seq n 0 last-seq (dec first-seq)]
    (let [{:keys [status event next]} (frame/read-at ch offset end)]
      (case status
        :ok (do (when (not= (:event/seq event) expected)
                  (throw (ex-info "segment seq out of order" {:file (str f) :offset offset :expected expected :got (:event/seq event)})))
                (on-frame event offset next)
                (recur (long next) (inc expected) (inc n) (long expected)))
        :short {:end offset :count n :last-seq last-seq :truncated? (not= offset end)}
        (:bad-crc :bad-payload)
        (if (= next end)
          {:end offset :count n :last-seq last-seq :truncated? true}
          (throw (ex-info "corrupt frame inside segment; refusing to start"
                          {:file (str f) :offset offset :status status})))))))

(defn open!
  "Open an existing segment with recovery. Returns {:file :channel :first-seq :end :count :last-seq}
   after calling (on-frame event offset next-offset) for every good frame. A truncated tail is cut off."
  [^File f on-frame]
  (let [ch (open-channel f)
        {:keys [first-seq]} (read-header ch f)
        size (.size ch)
        {:keys [end count last-seq truncated?]} (scan ch f first-seq size on-frame)]
    (when truncated?
      (log/warn "truncating segment tail" {:file (str f) :from size :to end})
      (.truncate ch end)
      (.force ch false))
    {:file f :channel ch :reader (atom nil) :first-seq first-seq :end end :count count :last-seq last-seq}))

(defn open-ro!
  "Open a segment read-only, scanning frames up to `max-seq` (nil = all) without truncating a torn
   tail or writing anything (a fork reads its parent's log in place, even while the
   parent runs). Returns {:file :channel :first-seq :end :count :last-seq}; a torn tail is simply
   where reading stops."
  [^File f max-seq on-frame]
  (let [ch (open-channel-ro f)
        {:keys [first-seq]} (read-header ch f)
        size (.size ch)
        stop (or max-seq Long/MAX_VALUE)]
    (loop [offset header-size n 0 last-seq (dec first-seq)]
      (let [{:keys [status event next]} (frame/read-at ch offset size)]
        (if (and (= status :ok) (<= (:event/seq event) stop))
          (do (on-frame event offset next)
              (recur (long next) (inc n) (long (:event/seq event))))
          {:file f :channel ch :reader (atom nil) :first-seq first-seq :end offset :count n :last-seq last-seq})))))

(defn append-bytes!
  "Write bytes at the segment end; returns the new end. Does not force."
  ^long [{:keys [^FileChannel channel ^long end]} ^bytes bs]
  (let [buf (ByteBuffer/wrap bs)]
    (loop [pos end]
      (if (.hasRemaining buf)
        (recur (+ pos (.write channel buf pos)))
        pos))))

(defn force! [{:keys [^FileChannel channel]}] (.force channel false))
(defn close!
  "Close the writer's channel and the readers', and refuse new reads."
  [{:keys [^FileChannel channel reader]}]
  (when (.isOpen channel) (.close channel))
  (when reader
    (let [r @reader]
      (reset! reader ::closed)
      (when (instance? FileChannel r) (.close ^FileChannel r)))))

(defn retire!
  "Close a segment the retention sweep has dropped. A reader that took it from an older state gets
   :pruned rather than an exception: it asked for something that has gone, not for something broken."
  [{:keys [^FileChannel channel reader]}]
  (locking reader
    (let [r @reader]
      (reset! reader ::pruned)
      (when (instance? FileChannel r) (.close ^FileChannel r))))
  (when (.isOpen channel) (.close channel)))

(defn from-floor
  "The segments at or above floor, the first seq still held; nil keeps them all."
  [segments floor]
  (if floor (vec (drop-while #(< (long (:first-seq %)) (long floor)) segments)) segments))

(defn- reader-channel
  "The segment's read channel, opened or reopened on demand; nil once the sweep has dropped it."
  ^FileChannel [{:keys [^File file reader]}]
  (locking reader
    (let [r @reader]
      (cond
        (= ::pruned r) nil
        (= ::closed r) (throw (ClosedChannelException.))
        (and r (.isOpen ^FileChannel r)) r
        :else (reset! reader (open-channel-ro file))))))

(defn read-at
  "The frame at offset, not past end, through a channel only readers use. Java closes a FileChannel
   when a thread is interrupted inside it, for every other user of that channel, and stopping a
   world interrupts its lanes and scheduler: one reader caught mid-read used to take the writer's
   channel with it. So the writer keeps its own, and a reader that loses this one reopens it and
   finishes, leaving the thread's interrupt set for whoever is waiting to see it. A segment that was
   pruned answers {:status :pruned}: a reader stops there as at the end of a segment, and the file
   of a fork's parent can go too, with no one here having been told."
  [segment ^long offset ^long end]
  (loop [tries 3 interrupted? false]
    (let [r (try (if-let [ch (reader-channel segment)]
                   (frame/read-at ch offset end)
                   {:status :pruned})
                 (catch FileNotFoundException _ {:status :pruned})
                 (catch ClosedChannelException e
                   (cond
                     (= ::pruned @(:reader segment)) {:status :pruned}
                     (or (zero? tries) (= ::closed @(:reader segment)))
                     (do (when interrupted? (.interrupt (Thread/currentThread)))
                         (throw e))
                     :else ::lost)))]
      (if (= ::lost r)
        (recur (dec tries) (or (Thread/interrupted) interrupted?))
        (do (when interrupted? (.interrupt (Thread/currentThread))) r)))))
(defn full? [{:keys [^long end]}] (>= end roll-bytes))

(defn fsync-dir!
  "Force the directory entry so a renamed/created file survives a crash."
  [dir]
  (try (with-open [ch (FileChannel/open (.toPath (io/file dir)) (into-array StandardOpenOption [StandardOpenOption/READ]))]
         (.force ch true))
       (catch Exception _ nil)))

(defn delete-if-exists! [^File f] (Files/deleteIfExists (.toPath f)))
