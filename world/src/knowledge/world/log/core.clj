(ns knowledge.world.log.core
  "The world log: the only state (ground rule 3; spec 7, ADR 0003, 0006).
   open!, append! / append!!, read-from, read-stream, head, subscribe!, unsubscribe!, close!.
   Every appended event is validated against :ke/PendingEvent before the writer sees it and
   against :ke/Event after seq assignment; a value of type bytes above 4 KiB goes to the blob store."
  (:require [clojure.edn :as edn]
            [clojure.java.io :as io]
            [knowledge.schema.registry :as r]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.index :as idx]
            [knowledge.world.log.mem :as mem]
            [knowledge.world.log.reader :as reader]
            [knowledge.world.log.segment :as seg]
            [knowledge.world.log.writer :as writer]
            [taoensso.timbre :as log])
  (:import [java.io File]
           [java.util Date]))

(defn- humanize [k v] ((requiring-resolve 'malli.error/humanize) (r/explain k v)))

(defn- open-segment
  "Open one segment; a sealed one with a fresh sidecar is trusted, anything else is scanned."
  [f last?]
  (if-let [cached (and (not last?) (idx/read f))]
    (let [ch (seg/open-channel f)]
      {:file f :channel ch :reader (atom nil) :first-seq (:first-seq cached) :end (:file-size cached) :idx cached})
    (let [acc (volatile! nil)
          s (seg/open! f (fn [ev off next]
                           (vswap! acc (fn [i] (idx/add-frame (or i (idx/empty-index (seg/first-seq-of f))) ev off next)))))
          i (or @acc (idx/empty-index (:first-seq s)))
          i (assoc i :file-size (:end s))]
      (idx/write! f i)
      (assoc s :idx i))))

(defn- open-parent-segment
  "A parent world's segment, read-only and capped at the fork seq: a fresh sidecar is trusted,
   anything else is scanned in memory; nothing under the parent is ever written."
  [f fork-seq]
  (if-let [cached (idx/read f)]
    {:file f :channel (seg/open-channel-ro f) :reader (atom nil) :first-seq (:first-seq cached) :end (:file-size cached) :idx cached}
    (let [acc (volatile! nil)
          s (seg/open-ro! f fork-seq (fn [ev off next]
                                       (vswap! acc (fn [i] (idx/add-frame (or i (idx/empty-index (seg/first-seq-of f))) ev off next)))))]
      (assoc s :idx (assoc (or @acc (idx/empty-index (:first-seq s))) :file-size (:end s))))))

(defn- open-parent
  "{:segments :seq :dir} of the parent log, segments that can hold seqs up to the fork seq only."
  [parent-data-dir ^long fork-seq]
  (let [dir (io/file parent-data-dir "log")
        files (filter #(<= (seg/first-seq-of %) fork-seq) (seg/segment-files dir))]
    {:dir dir :seq fork-seq :segments (mapv #(open-parent-segment % fork-seq) files)}))

(defn- require-contiguous!
  "Segments are pruned from the front only, oldest first, so a hole between two of them is damage: the
   file went some other way. Refuse, as for a bad frame, and a person decides."
  [segments]
  (doseq [[a b] (partition 2 1 segments)
          :let [next-seq (inc (long (get-in a [:idx :last-seq])))]]
    (when (not= next-seq (long (:first-seq b)))
      (throw (ex-info "a segment is missing from the middle of the log; refusing to start"
                      {:after (str (:file a)) :before (str (:file b)) :expected next-seq})))))

(defn open!
  "Open (or create) the log under data-dir. Returns the log component. With opts
   {:parent {:data-dir .. :seq N}} the log is a fork: the parent's events 1..N are
   read in place before this world's own, which begin at N+1; writes never touch the parent."
  [data-dir {:keys [fsync parent] :or {fsync :batch}}]
  (let [dir (io/file data-dir "log")
        _ (.mkdirs dir)
        parent (when parent (open-parent (:data-dir parent) (long (:seq parent))))
        first-own (if parent (inc (long (:seq parent))) 1)
        files (seg/segment-files dir)
        segments (if (empty? files)
                   [(assoc (seg/create! dir first-own) :idx (idx/empty-index first-own))]
                   (vec (map-indexed (fn [i f] (open-segment f (= i (dec (count files))))) files)))
        _ (require-contiguous! segments)
        head (get-in (peek segments) [:idx :last-seq])
        state (atom (cond-> {:dir dir :segments segments :head head :subscribers {}}
                      parent (assoc :parent parent)))]
    (log/info "log open" (cond-> {:dir (str dir) :segments (count segments) :head head}
                           parent (assoc :fork-of (str (:dir parent)) :forked-at (:seq parent))))
    {:state state
     :blobs (blob/open data-dir (if parent [(.getParentFile ^java.io.File (:dir parent))] []))
     :writer (writer/start! state fsync)}))

(declare read-from read-stream stream-head)

(defn open-in-memory!
  "A log with no file: the parent's events 1..seq read in place, then events held in an atom,
   gone at close-in-memory!. Same functions as any log, so everything above it is unchanged
   (an off-the-record session). Blobs go to an in-memory store that falls back to the parent's."
  [parent seq]
  {:mem (mem/create seq) :parent parent :base seq :blobs (blob/in-memory (:blobs parent))})

(defn peek-head
  "The head seq of a world directory read without a writer, an index write or a truncation (world
   pass 9: tools look at live worlds). 0 when there is no log."
  ^long [data-dir]
  (let [files (seg/segment-files (io/file data-dir "log"))]
    (if (empty? files)
      0
      (let [s (seg/open-ro! (last files) nil (fn [_ _ _] nil))]
        (seg/close! s)
        (long (:last-seq s))))))

(defn fork-seq
  "The seq this world was forked at, or nil when it is not a fork."
  [{:keys [state base]}]
  (if state (get-in @state [:parent :seq]) base))

(defn read-own-from
  "This world's own events (after the fork seq) with seq >= seq, up to limit."
  [{:keys [state mem]} seq limit]
  (if mem (mem/read-from mem seq limit) (reader/read-own-from @state seq limit)))

(defn head ^long [{:keys [state mem]}] (if mem (mem/head mem) (:head @state)))

(defn first-seq
  "The oldest seq this log still holds: 1 until the retention sweep has dropped a segment."
  ^long [{:keys [state parent]}]
  (if state (reader/first-seq @state) (first-seq parent)))

(defn gap
  "The first seq still held when seq falls below it, else nil: what a reader that wanted seq has
   lost. read-from and read-stream go on from what is there, so a reader that must not skip asks."
  [log ^long seq]
  (let [f (first-seq log)]
    (when (< seq f) f)))

(defn own-gap
  "The first of a fork's own seqs it still holds, when its own log no longer starts at the fork seq."
  [{:keys [state]}]
  (let [st @state
        own (reader/first-seq (dissoc st :parent))
        at (get-in st [:parent :seq])]
    (when (and at (> own (inc (long at)))) own)))

(defn first-seq-of-dir
  "The oldest seq a world directory holds of its own, read from the segment names (a tool, on a world
   that may be live). 1 with no log. A fork's parent is not looked at."
  ^long [data-dir]
  (or (some-> (first (seg/segment-files (io/file data-dir "log"))) seg/first-seq-of) 1))

(defn- forks-dir ^File [data-dir] (io/file data-dir "forks"))

(defn mark-fork!
  "Leave a note under the parent that a fork reads its segments up to :seq (tools/fork!)."
  [data-dir {:keys [fork] :as note}]
  (.mkdirs (forks-dir data-dir))
  (spit (io/file (forks-dir data-dir) (str fork ".edn")) (pr-str note)))

(defn fork-ceiling
  "The lowest seq a living fork of this world reads in place, or nil. A note whose child has gone is
   removed: a deleted fork must not hold the log forever."
  [data-dir]
  (let [alive? (fn [{:keys [child fork]}]
                 (let [f (io/file (str child) "world.edn")]
                   (and (.isFile f) (= fork (:fork/id (try (edn/read-string (slurp f)) (catch Exception _ nil)))))))
        notes (for [^File f (.listFiles (forks-dir data-dir))
                    :let [n (try (edn/read-string (slurp f)) (catch Exception _ nil))]]
                [f n])
        [live gone] ((juxt filter remove) (fn [[_ n]] (alive? n)) notes)]
    (doseq [[^File f _] gone] (.delete f))
    (when (seq live) (apply min (map (fn [[_ n]] (long (:seq n))) live)))))

(defn- segment-bytes [{:keys [^File file]}]
  (+ (.length file) (.length (seg/idx-file file))))

(defn closed-segments
  "This world's own segments the sweep may look at, oldest first: all but the active one, as
   {:first-seq :last-seq :bytes}. A fork's parent's are not its own."
  [{:keys [state]}]
  (mapv (fn [s] {:first-seq (:first-seq s) :last-seq (get-in s [:idx :last-seq]) :bytes (segment-bytes s)})
        (butlast (:segments @state))))

(defn own-bytes
  "What this world's own segments and their indexes weigh."
  ^long [{:keys [state]}]
  (reduce + 0 (map segment-bytes (:segments @state))))

(defn prune-oldest!
  "Drop the oldest segment if it is closed and wholly at or below `through`. Returns {:bytes :first-seq},
   the bytes freed and the oldest seq left, or nil. Off the writer: it only moves the floor in the
   state, which the writer carries over, and the delete happens here. The active segment is never
   dropped, so the log always has a head. Deletes the frames first: a crash leaves an index with no
   segment, which nothing reads, and never a hole."
  [{:keys [state]} ^long through]
  (let [{:keys [segments]} @state
        s (first segments)]
    (when (and (> (count segments) 1) (<= (long (get-in s [:idx :last-seq])) through))
      (let [floor (long (:first-seq (second segments)))
            bytes (segment-bytes s)]
        (swap! state (fn [st] (-> st (assoc :floor floor) (update :segments seg/from-floor floor))))
        (seg/retire! s)
        (seg/delete-if-exists! (:file s))
        (seg/delete-if-exists! (seg/idx-file (:file s)))
        (seg/fsync-dir! (:dir @state))
        {:bytes bytes :first-seq floor}))))

(defn pending
  "Build a pending event (spec 4.4, 7.1). streams is the extra set when the event belongs on several."
  [kind stream actor body & {:keys [streams via delegation at]}]
  (cond-> {:event/kind kind :event/stream stream :event/actor actor :event/at (or at (Date.)) :event/body body}
    (seq (disj (set streams) stream)) (assoc :event/streams (disj (set streams) stream))
    via (assoc :event/via via)
    delegation (assoc :event/delegation delegation)))

(defn- externalize [blobs event]
  (let [[body hit?] (blob/externalize blobs (:event/body event))]
    (cond-> (assoc event :event/body body)
      (and hit? (= :field-set (:event/kind event))) (assoc-in [:event/body :mode] :blob))))

(defn append!
  "Validate and enqueue a pending event. Returns a promise of the assigned seq (or a Throwable)."
  [{:keys [writer blobs mem]} event]
  (let [event (externalize blobs event)]
    (when-not (r/valid? :ke/PendingEvent event)
      (throw (ex-info "pending event invalid" {:event event :explain (humanize :ke/PendingEvent event)})))
    (let [p (promise)]
      (if mem
        (deliver p (try (mem/append! mem event) (catch Throwable t t)))
        (writer/enqueue! writer [event p]))
      p)))

(defn append!!
  "append! and wait for the seq. Throws if the writer rejected the event."
  ^long [log event]
  (let [r @(append! log event)]
    (if (instance? Throwable r) (throw r) r)))

(defn read-from [{:keys [state mem parent base] :as _log} seq limit]
  (if mem
    (let [inherited (when (<= seq base)
                      (filterv #(<= (:event/seq %) base) (read-from parent seq (min limit (inc (- base seq))))))]
      (into (vec inherited) (mem/read-from mem seq (- limit (count inherited)))))
    (reader/read-from @state seq limit)))

(defn stream-head
  "The seq of the last committed event on a stream, or 0 (spec 4.5 as-of). Read from the segment
   indexes, newest segment first, through a fork's parent (capped at the fork seq)."
  ^long [{:keys [state mem parent base]} stream]
  (if mem
    (or (mem/stream-head mem stream) (min (long base) (stream-head parent stream)))
    (let [st @state
        cap (get-in st [:parent :seq])
        own (some (fn [s] (some-> (get-in s [:idx :streams stream]) peek first)) (rseq (:segments st)))
        parent (when cap
                 (some (fn [s] (some->> (get-in s [:idx :streams stream]) (filter (fn [[sq _]] (<= sq cap))) last first))
                       (rseq (get-in st [:parent :segments]))))]
    (or own parent 0))))
(defn read-stream [{:keys [state mem parent base]} stream after limit]
  (if mem
    (let [inherited (when (< after base)
                      (filterv #(<= (:event/seq %) base) (read-stream parent stream after limit)))]
      (into (vec inherited) (take (- limit (count inherited))) (mem/read-stream mem stream after)))
    (reader/read-stream @state stream after limit)))

(defn subscribe!
  "f is called on the writer thread with each committed batch (a vector of events). Returns the key."
  ([log f] (subscribe! log (random-uuid) f))
  ([{:keys [state mem]} k f]
   (if mem (mem/subscribe! mem k f) (swap! state assoc-in [:subscribers k] f))
   k))

(defn unsubscribe! [{:keys [state mem]} k]
  (if mem (mem/unsubscribe! mem k) (swap! state update :subscribers dissoc k))
  nil)

(defn blobs [log] (:blobs log))

(defn close-in-memory!
  "Forget everything an in-memory log held. Later appends throw."
  [{:keys [mem blobs]}]
  (mem/close! mem)
  (reset! (:mem blobs) {})
  nil)

(defn close!
  "Stop the writer, persist the active index, close channels."
  [{:keys [state writer]}]
  (writer/stop! writer)
  (let [st @state]
    (doseq [s (:segments st)]
      (idx/write! (:file s) (:idx s))
      (seg/close! s))
    ;; a parent's segments are closed, never indexed or written
    (doseq [s (get-in st [:parent :segments])] (seg/close! s)))
  (swap! state assoc :segments [] :subscribers {} :parent nil)
  nil)
