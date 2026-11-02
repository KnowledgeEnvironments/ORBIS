(ns knowledge.world.retention
  "Keeping a world off a full disk (decisions 2026-11-01 'retention', docs/log-format.md Retention).
   One limit, a person's to set, covers the blob store and this world's log segments with their
   indexes: 16 GiB unless KE_RETENTION_GB says otherwise, or the Field retention/limit-gb, set by
   retention.set, which may be \"infinite\".

   A sweep runs on its own thread at start and on the scheduler's clock, never on the writer's. Over
   the limit it evicts the least recently used blobs a live thing does not need. Only when that is not
   enough does it drop log segments, oldest first, closed, and wholly at or below a snapshot it has
   loaded and kept in anchor/. That is the one place the log stops being the whole truth, so the
   rules are strict: see `prune-log!`.

   Text logs under logs/ rotate by size and old releases go, whatever the limit: neither is history
   of the world. retention/last-sweep on the world says what the last sweep that did anything took."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [datascript.core :as d]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.authority :as authority]
            [knowledge.world.notify :as notify]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.index.snapshot :as snap]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.pane.core :as pane]
            [knowledge.world.pane.editor :as editor]
            [knowledge.world.thread :as thread]
            [taoensso.timbre :as timbre])
  (:import [java.io File]
           [java.nio.channels FileChannel]
           [java.nio.file Files LinkOption StandardCopyOption StandardOpenOption]
           [java.time Instant]
           [java.util Date]))

(def limit-field :retention/limit-gb)
(def sweep-field :retention/last-sweep)
(def default-gb 16)
(def infinite "infinite")

(def defaults
  {:every-ms (* 15 60 1000)
   ;; evict down to this share of the limit, so the next sweep has nothing to do
   :low-water 0.9
   ;; a blob this new may be on its way to the log: put!, then the event that names it
   :grace-ms (* 60 60 1000)
   :stale-tmp-ms (* 24 60 60 1000)
   ;; a conversation with a Message this recent is current: its images stay
   :current-ms (* 14 24 60 60 1000)
   ;; the newest output of each open terminal that stays: a replay needs the end, and `yes` must
   ;; not be able to pin the disk
   :terminal-bytes (* 16 1024 1024)
   :log-max-bytes (* 64 1024 1024)
   :log-keep 2
   :keep-releases 3})

(defn- options [world] (merge defaults (get-in world [:config :retention])))

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- valid-gb? [v] (or (= infinite v) (and (number? v) (pos? v))))

(defn limit-gb
  "The limit as a person set it, else KE_RETENTION_GB, else the default: a number or \"infinite\"."
  [world]
  (let [v (field (act/db world) (:world-id world) limit-field)
        configured (get-in world [:config :retention-gb])]
    (cond (valid-gb? v) v
          (valid-gb? configured) configured
          :else default-gb)))

(defn- gb->bytes ^long [gb] (long (* gb 1024 1024 1024)))

;; ---------------------------------------------------------------- what a live thing needs

(defn- receipts-in
  "Every blob hash a Field value holds, at any depth."
  [v]
  (cond (blob/receipt? v) [(:blob/sha256 v)]
        (map? v) (mapcat receipts-in (vals v))
        (or (sequential? v) (set? v)) (mapcat receipts-in v)
        :else nil))

(def ^:private evictable-fields
  "What the sweep is for: a prompt's bytes and its handle table. Hash and size stay; nothing reads
   these back but a person looking at an old prompt."
  #{:serialization/bytes :serialization/ids})

(defn- field-pins
  "Blobs that are the current value of a Field: a thing the world holds now, not a record of one.
   Not a retired Object's, and not a closed pane's."
  [db]
  (into #{}
        (mapcat (fn [{:keys [e v]}]
                  (when-let [hashes (seq (receipts-in v))]
                    (let [k (:field/name (d/entity db e))
                          owner (d/entity db (:e (first (d/datoms db :avet :object/fields e))))
                          id (:object/id owner)]
                      (when-not (or (evictable-fields k)
                                    (:object/retired? owner)
                                    (and (= "pane" (namespace k)) (not= :open (field db id :pane/state))))
                        hashes)))))
        (d/datoms db :aevt :field/value)))

(defn- image-pins
  "Images on Messages of a current conversation: one with a Message newer than current-ms. An older
   conversation's images go by use like anything else, and image/expand then tells the Model the
   image is no longer stored."
  [db ^Date now current-ms]
  (let [cut (- (.getTime now) (long current-ms))
        at (fn [m] (some-> (get-in (objects/fields db m) [:message/text :field/set-at]) (.getTime)))
        current? (memoize (fn [t] (boolean (some #(> (or (at %) 0) cut) (thread/messages db t)))))]
    (into #{}
          (comp (filter #(or (> (or (at %) 0) cut) (current? (thread/thread-of db %))))
                (mapcat #(keep :sha256 (field db % :message/images))))
          (objects/objects-with-field db :message/images))))

(defn- open-panes
  "[id kind] of panes still open."
  [db]
  (for [id (objects/objects-with-field db :pane/kind)
        :when (= :open (field db id :pane/state))]
    [id (field db id :pane/kind)]))

(defn- terminal-pins
  "The newest output of an open terminal, up to terminal-bytes."
  [world id terminal-bytes]
  (let [outputs (filter #(= :pane-output (:event/kind %)) (log/read-stream (:log world) id 0 Long/MAX_VALUE))]
    (loop [es (reverse outputs) left (long terminal-bytes) acc []]
      (let [{:keys [blob size]} (:event/body (first es))]
        (if (and blob (pos? left))
          (recur (rest es) (- left (long size)) (conj acc blob))
          acc)))))

(defn live-blobs
  "The hashes the sweep must leave alone."
  [world opts ^Date now]
  (let [db (act/db world)]
    (into (into (field-pins db) (image-pins db now (:current-ms opts)))
          (mapcat (fn [[id kind]]
                    (case kind
                      :terminal (terminal-pins world id (:terminal-bytes opts))
                      :editor (editor/needed-blobs world id)
                      nil)))
          (open-panes db))))

;; ---------------------------------------------------------------- blobs

(defn- evict-blobs!
  "Least recently used first, until `over` bytes have gone or nothing evictable is left.
   Returns {:bytes :count :pinned}: pinned is what stayed only because something needs it."
  [store items pins over now-ms {:keys [grace-ms]} stopping]
  (let [cut (- (long now-ms) (long grace-ms))]
    (loop [items (sort-by :mtime items) freed 0 n 0 pinned 0]
      (let [{:keys [hex mtime size] :as item} (first items)]
        (cond
          (or (nil? item) @stopping) {:bytes freed :count n :pinned pinned}
          (>= freed (long over)) {:bytes freed :count n :pinned pinned}
          (pins hex) (recur (rest items) freed n (+ pinned (long size)))
          (> (long mtime) cut) {:bytes freed :count n :pinned pinned}
          :else (if-let [got (blob/evict! store item)]
                  (recur (rest items) (+ freed (long got)) (inc n) pinned)
                  (recur (rest items) freed n pinned)))))))

;; ---------------------------------------------------------------- log segments

(defn- slurp-trim [^File f] (when (.isFile f) (str/trim (slurp f))))

(defn- rollback-safe?
  "May this release's snapshots hold the world up? Not while it could still fail and be rolled
   back: the supervisor returns to release.good, which may not read what this one wrote, so until this
   release IS release.good only blobs go. A world with no supervisor has nothing to roll back to."
  [world]
  (let [running (get-in world [:config :release])]
    (or (nil? running)
        (= (str running) (slurp-trim (io/file (:data-dir world) "release.good"))))))

(defn- ensure-anchor!
  "The seq of the anchor after making it the newest snapshot that verifies, or the one already there,
   or nil. Loads a snapshot whole, so only when the anchor falls short of `want` and the snapshot
   would reach `useful`, the least that frees anything."
  [world want useful]
  (let [data (:data-dir world)
        have (snap/anchor-seq data)]
    (if (and have (>= (long have) (long want)))
      have
      (or (some (fn [^File f]
                  (when (and (> (snap/seq-of f) (long (or have 0)))
                             (>= (snap/seq-of f) (long useful))
                             (<= (snap/seq-of f) (log/head (:log world)))
                             (snap/verified f (:world-id world)))
                    (snap/anchor! data f)
                    (snap/seq-of f)))
                (reverse (snap/snapshot-files (io/file data "snapshots"))))
          have))))

(defn- keep-open-panes!
  "An open terminal's chain and an open editor's buffer live in events: before the segments holding
   them go, put them above the cut. Appended and durable before anything is deleted."
  [world through]
  (doseq [[id kind] (open-panes (act/db world))]
    (case kind
      :terminal (pane/record-terminal! world id through)
      :editor (when (editor/checkpoint-needed? world id through) (editor/checkpoint! world id))
      nil)))

(defn- prune-log!
  "Drop segments to free `over` bytes. A segment goes only if it is closed, the oldest, and wholly at or
   below a snapshot that has been loaded in full and copied to anchor/, on a release that cannot be rolled
   back from, and past nothing a fork reads in place. A fork keeps its own log whole: it is the thing
   world.diff and world-promote read. Returns {:bytes :segments :blocked?}."
  [world over stopping]
  (let [l (:log world)
        ceiling (log/fork-ceiling (:data-dir world))
        wanted (loop [segs (log/closed-segments l) left (long over) acc []]
                 (let [{:keys [bytes last-seq] :as s} (first segs)]
                   (if (and s (pos? left) (or (nil? ceiling) (<= (long last-seq) (long ceiling))))
                     (recur (rest segs) (- left (long bytes)) (conj acc s))
                     acc)))]
    (cond
      (log/fork-seq l) {:bytes 0 :segments 0 :blocked "a fork keeps its own log"}
      (not (rollback-safe? world)) {:bytes 0 :segments 0 :blocked "this release is not yet the good one"}
      (empty? wanted) {:bytes 0 :segments 0
                       :blocked (if (seq (log/closed-segments l)) "a fork reads these segments" "no closed segment")}
      :else
      (let [want (:last-seq (peek wanted))
            anchor (ensure-anchor! world want (:last-seq (first wanted)))
            cut (when anchor (min (long anchor) (long want)))]
        (if-not (and cut (>= cut (long (:last-seq (first wanted)))))
          {:bytes 0 :segments 0 :blocked (str "no verified snapshot reaches seq " (:last-seq (first wanted)))}
          (do (keep-open-panes! world cut)
              (loop [freed 0 n 0]
                (if-let [{:keys [bytes]} (when-not @stopping (log/prune-oldest! l cut))]
                  (recur (+ freed (long bytes)) (inc n))
                  {:bytes freed :segments n}))))))))

(defn- orphan-indexes!
  "An index whose segment a crash took between the two deletes."
  [data-dir]
  (doseq [^File f (.listFiles (io/file data-dir "log"))
          :when (and (str/ends-with? (.getName f) ".idx")
                     (not (.exists (io/file (.getParentFile f) (str/replace (.getName f) #"\.idx$" "")))))]
    (.delete f)))

;; ---------------------------------------------------------------- text logs

(defn rotate-logs!
  "Any logs/*.log over log-max-bytes is copied to .1, older copies shifted, and truncated. Copied rather
   than renamed: the supervisor holds these open for append, so its next line lands at the new end.
   A line written between the copy and the truncate is lost. How many rotated."
  [data-dir {:keys [log-max-bytes log-keep]}]
  (let [dir (io/file data-dir "logs")
        name-of (fn [^File f n] (io/file dir (str (.getName f) "." n)))]
    (count
      (for [^File f (.listFiles dir)
            :when (and (.isFile f) (str/ends-with? (.getName f) ".log") (> (.length f) (long log-max-bytes)))]
        (do (.delete ^File (name-of f log-keep))
            (doseq [n (range (dec (long log-keep)) 0 -1)]
              (let [^File from (name-of f n)]
                (when (.exists from) (.renameTo from (name-of f (inc n))))))
            (Files/copy (.toPath f) (.toPath ^File (name-of f 1))
                        ^"[Ljava.nio.file.CopyOption;" (into-array java.nio.file.CopyOption [StandardCopyOption/REPLACE_EXISTING]))
            (with-open [ch (FileChannel/open (.toPath f) (into-array StandardOpenOption [StandardOpenOption/WRITE]))]
              (.truncate ch 0))
            f)))))

;; ---------------------------------------------------------------- releases

(defn- delete-tree!
  "A release is read-only by file mode: open each directory first, then empty it. Links are removed,
   never followed: a tree is what a commit said it was."
  [^File dir]
  (doseq [^File f (.listFiles dir)]
    (if (Files/isDirectory (.toPath f) (into-array LinkOption [LinkOption/NOFOLLOW_LINKS]))
      (do (.setWritable f true true) (delete-tree! f))
      (.delete f)))
  (.setWritable dir true true)
  (.delete dir))

(defn- requested-release
  "What a restart request names, which the supervisor has not read yet."
  [data-dir]
  (let [f (io/file data-dir "restart.request")]
    (when (.isFile f)
      (some-> (re-find #"(?m)^release=(.*)$" (slurp f)) second str/trim))))

(defn trim-releases!
  "Under releases/, keep the one in release, the one running, release.good, one a restart has asked
   for, and the newest keep-releases; a rollback needs release.good and nothing older. Nothing happens
   without a release file: no supervisor made this directory. Only a full commit name with a manifest
   counts as a release, so a build in progress is left alone. The names it removed."
  [world {:keys [keep-releases]}]
  (let [data (:data-dir world)
        current (slurp-trim (io/file data "release"))
        dirs (filter (fn [^File d] (and (.isDirectory d)
                                        (re-matches #"[0-9a-f]{40}" (.getName d))
                                        (.isFile (io/file d "release.edn"))))
                     (.listFiles (io/file data "releases")))]
    (when (seq current)
      (let [wanted (set (remove str/blank? [current (slurp-trim (io/file data "release.good"))
                                            (str (get-in world [:config :release]))
                                            (requested-release data)]))
            newest (set (map #(.getName ^File %) (take keep-releases (sort-by #(.lastModified ^File %) > dirs))))
            drop? (fn [^File d] (not (or (wanted (.getName d)) (newest (.getName d)))))]
        (vec (for [^File d dirs :when (drop? d)]
               (do (.setWritable d true true)
                   (delete-tree! d)
                   (.getName d))))))))

;; ---------------------------------------------------------------- the sweep

(defn notice-for
  "What the people who own the world are told when a sweep's standing changes, or nil: over the limit
   with nothing left to take, once, and again only when what blocks it changes; back under, once. Until
   this, being stuck over the limit showed in retention/last-sweep and the world's log alone."
  [was r]
  (let [gb (fn [b] (format "%.1f GB" (/ (double b) 1e9)))]
    (cond
      (and (:over r) (or (nil? (:over was)) (not= (:blocked r) (:blocked was))))
      (str "ORBIS is " (gb (:over r)) " over its disk limit of " (:limit-gb r) " GB and nothing more can go: "
           (:blocked r) ". Raise the limit with retention.set, or retire what holds it.")
      (and (:over was) (not (:over r)))
      (str "ORBIS is back under its disk limit of " (:limit-gb r) " GB.")
      :else nil)))

(defn- record!
  "retention/last-sweep, the world as actor, when a sweep did something or what stops it changed; and
   a word to the people who own the world when its standing changes."
  [world {:keys [bytes releases logs-rotated blocked] :as r}]
  (let [wid (:world-id world)
        was (field (act/db world) wid sweep-field)]
    (when (or (pos? (long bytes)) (pos? (long releases)) (pos? (long logs-rotated)) (not= blocked (:blocked was)))
      (log/append!! (:log world)
                    (log/pending :field-set wid wid {:name sweep-field :type :map :reader :both
                                                     :value (into {} (remove (comp nil? val)) r)})))
    (when-let [text (notice-for was r)]
      (try
        (let [root (gates/root-of (act/db world))]
          (gates/tell-person! world root text)
          (when (:over r) (notify/send! world root {:level "urgent" :text text})))
        (catch Exception e (timbre/warn e "retention notice not delivered"))))))

(defn- housekeeping
  "A step that must not stop the disk limit being kept: a log that cannot rotate is no reason to fill
   the disk."
  [what f]
  (try (f) (catch Exception e (timbre/warn e "retention step failed" {:step what}) nil)))

(defn sweep!
  "One pass. Safe to run while the world runs. Returns what it did."
  ([world] (sweep! world (atom false)))
  ([world stopping]
   (let [opts (options world)
         data (:data-dir world)
         now (Date.)
         store (:blobs world)
         l (:log world)
         rotated (housekeeping :logs #(rotate-logs! data opts))
         releases (housekeeping :releases #(trim-releases! world opts))
         _ (housekeeping :tmp #(blob/drop-stale-tmp! store (:stale-tmp-ms opts)))
         _ (housekeeping :indexes #(orphan-indexes! data))
         gb (limit-gb world)
         items (blob/inventory store)
         blob-bytes (reduce + 0 (map :size items))
         used (+ blob-bytes (log/own-bytes l))
         limit (when (number? gb) (gb->bytes gb))
         target (when limit (long (* limit (double (:low-water opts)))))
         evicted (if (and limit (> used limit))
                   (evict-blobs! store items (live-blobs world opts now) (- used target) (.getTime now) opts stopping)
                   {:bytes 0 :count 0 :pinned 0})
         after-blobs (- used (long (:bytes evicted)))
         pruned (if (and limit (> after-blobs limit) (not @stopping))
                  (prune-log! world (- after-blobs limit) stopping)
                  {:bytes 0 :segments 0})
         after (- after-blobs (long (:bytes pruned)))
         result {:bytes (+ (long (:bytes evicted)) (long (:bytes pruned)))
                 :count (+ (long (:count evicted)) (long (:segments pruned)))
                 :blobs (:count evicted) :segments (:segments pruned)
                 :releases (count releases) :logs-rotated (or rotated 0)
                 :first-seq (log/first-seq l) :used after :limit-gb gb :at (str (Instant/now))
                 :over (when (and limit (> after limit)) (- after limit))
                 :pinned (when (pos? (long (:pinned evicted))) (:pinned evicted))
                 :blocked (when (and limit (> after limit)) (:blocked pruned "only what a live thing needs is left"))}]
     (when (or (pos? (long (:bytes result))) (seq releases))
       (timbre/info "retention sweep" result))
     (when (:over result) (timbre/warn "over the retention limit" (select-keys result [:over :used :limit-gb :blocked])))
     (when-not @stopping (record! world result))
     result)))

(defn component [] {:running (atom false) :again (atom false) :stopping (atom false) :thread (atom nil)})

(defn- sweeping! [world {:keys [running again stopping]}]
  (try
    (loop []
      (reset! again false)
      (try (sweep! world stopping)
           (catch Throwable t (timbre/warn t "retention sweep failed")))
      (when (and @again (not @stopping)) (recur)))
    (finally (reset! running false))))

(defn kick!
  "Start a sweep on its own thread, unless one is running, which then goes round once more. Never on the
   caller's: a sweep walks the blob store and can load a snapshot."
  [world]
  (when-let [{:keys [running again stopping thread] :as c} (when-not (:scratch world) (:retention world))]
    (reset! again true)
    (when (and (not @stopping) (compare-and-set! running false true))
      (reset! thread (doto (Thread. ^Runnable (fn [] (sweeping! world c)) "ke-retention")
                       (.setDaemon true)
                       (.setPriority Thread/MIN_PRIORITY)
                       (.start))))))

(defn stop!
  "Ask a running sweep to stop between steps and wait a little for it."
  [{:keys [stopping thread]}]
  (when stopping
    (reset! stopping true)
    (when-let [^Thread t @thread] (.join t 5000))))

;; ---------------------------------------------------------------- the action

(defn- would-go
  "What a limit `gb` below today's use would take at the next sweep, in bytes, or nil when it is not
   below: a person lowering the limit by a lot pruned at once, with no word of what went."
  [world gb]
  (let [l (:log world)
        blob-bytes (reduce + 0 (map :size (blob/inventory (:blobs world))))
        log-bytes (log/own-bytes l)
        used (+ blob-bytes log-bytes)
        limit (gb->bytes gb)]
    (when (> used limit)
      {:used used :limit limit :would-go (- used (long (* limit (double (:low-water (options world))))))
       :blobs blob-bytes :log log-bytes :first-seq (log/first-seq l)})))

(defmethod actions/handle! :retention.set
  [_ world p {:keys [gb confirm]} opts]
  (authority/require-person! (act/db world) p opts)
  (when-not (valid-gb? gb)
    (throw (ex-info "gb is a positive number or \"infinite\""
                    {:err {:code :invalid-args :message "gb is a positive number or \"infinite\""}})))
  (if-let [preview (and (number? gb) (not (true? confirm)) (would-go world gb))]
    {:ok {:result (assoc preview :gb gb :not-set true
                         :next (str "the limit is below what the world uses: the next sweep would take about "
                                    (format "%.1f MB" (/ (double (:would-go preview)) 1e6))
                                    ", the least recently used blobs first, then the oldest log segments, and it does not come back. "
                                    "retention.set again with confirm true to go ahead"))
          :object (:world-id world)}}
    (let [wid (:world-id world)
          seq-no (act/append! world (log/pending :field-set wid p {:name limit-field :type (if (number? gb) :number :string)
                                                                   :value gb :reader :both}))]
      (kick! world)
      {:ok {:result {:gb gb :bytes (when (number? gb) (gb->bytes gb))} :object wid :seq seq-no}})))
