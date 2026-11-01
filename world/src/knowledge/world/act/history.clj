(ns knowledge.world.act.history
  "Global history and the snapshot read (sprint item W7). `history` without a stream
   pages the whole log, each event filtered by resolution of its streams for :read; with
   `snapshot true` the answer opens with the resolvable state of every Object at one seq (the
   index pinned under its lock, so the tail that follows starts exactly after it), paged by a
   cursor over object ids, so a client folds from the snapshot and subscribes `after` its seq
   instead of replaying from 0. One `resolved` event summarises the read: name \"*\", the count,
   the seq. Nothing here is state: the snapshot is the fold at a seq, recomputable from the log."
  (:require [datascript.core :as d]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.index.core :as index]
            [knowledge.world.log.core :as log]
            [knowledge.world.refs :as refs]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]
            [knowledge.world.thread :as thread]))

(def max-limit 1000) ; events or objects per history page; a larger limit is clamped, never refused
(def default-objects-per-page 400)
(def default-tail 256)
(def bytes-per-page (* 2 1024 1024)) ; well under the 4 MiB frame

(defn- pinned
  "[db seq] with the index caught up and both read under its lock."
  [idx]
  (locking (:lock idx)
    (index/catch-up! idx)
    [@(:db idx) (index/applied-seq idx)]))

;; A paged snapshot is one fold: every page comes from the db pinned for the first page, found by
;; the seq the cursor carries. The pins are a cache of immutable db values, kept a minute, nothing
;; the log does not hold (ground rule 3).
(defonce ^:private pins (atom {}))
(def pin-ttl-ms 60000)

(defn- remember-pin! [world-id at db]
  (let [now (System/currentTimeMillis)]
    (swap! pins (fn [m] (into {[world-id at] {:db db :at now :ids (atom {})}}
                              (remove (fn [[_ {t :at}]] (> (- now t) pin-ttl-ms)) m))))))

(defn- parse-cursor
  "\"<seq>:<id>\" -> [seq id]; a bare id (no seq) -> [nil id]."
  [cursor]
  (when (string? cursor)
    (if-let [[_ s id] (re-matches #"(\d+):(.+)" cursor)] [(parse-long s) id] [nil cursor])))

(defn visible-event?
  "May the principal read the event? It resolves the event's stream or one of its extra streams."
  [db p {:keys [event/stream event/streams]}]
  (boolean (some #(resolve/resolves? db p (act/id-name %) :read) (cons stream streams))))

(defn- all-ids
  "Object ids in cursor order; retired Objects only when asked (decisions 2026-10-31). The manual's
   topics stay out: hundreds of records no client folds, and doc.* reads them when asked."
  [db retired?]
  (let [topics (set (objects/objects-with-field db :topic/name))]
    (sort (map str (cond->> (remove topics (d/q '[:find [?id ...] :where [?o :object/id ?id] [?o :object/class _]] db))
                     (not retired?) (remove #(objects/retired? db %)))))))

(defn- state-for
  "The read shape of one Object for the principal: its state with the Fields it may see."
  [db p id]
  (let [state (objects/state db id)]
    (assoc state :object/fields (refs/visible-fields db p (resolve/visible-fields db p (:object/fields state) :read)))))

(defn- resolvable-ids
  "The sorted ids of what p may read at this pin, worked out once: a boot asks for them page by page,
   and each page deriving them again is quadratic in the Objects of a world."
  [ids db p retired?]
  (let [compute (fn [] (filterv #(resolve/resolves? db p (act/id-name (parse-uuid %)) :read) (all-ids db retired?)))]
    (if ids
      (or (get @ids [p retired?]) (let [v (compute)] (swap! ids assoc [p retired?] v) v))
      (compute))))

(defn- after-cursor
  "The part of sorted ids after cursor."
  [ids cursor]
  (if-not cursor
    ids
    (let [i (java.util.Collections/binarySearch ^java.util.List ids cursor)]
      (subvec ids (if (neg? i) (- (inc i)) (inc i))))))

(defn snapshot
  "{:seq :objects :total :next?}: the resolvable Objects at the pinned seq after cursor, up to
   per-page of them (and bytes-per-page); :next is the cursor for the page after, when there is one.
   ids is the pin's cache of resolvable ids, when there is a pin."
  [db at p cursor per-page retired? ids]
  (let [all (resolvable-ids ids db p retired?)
        page (loop [acc [] bytes 0 [id & more :as xs] (seq (after-cursor all cursor))]
               (if (or (nil? xs) (>= (count acc) per-page) (> bytes bytes-per-page))
                 {:objects acc :rest xs}
                 (let [st (state-for db p (parse-uuid id))]
                   (recur (conj acc st) (+ bytes (count (pr-str st))) more))))]
    (cond-> {:seq at :objects (:objects page) :total (count all)}
      (seq (:rest page)) (assoc :next (str at ":" (:object/id (peek (:objects page))))))))

(defn- summarize
  "What a fork changed, from its own events: counts by kind, objects created (with class), Fields
   set by object (count), Messages and Evaluations (`world.diff`)."
  [events]
  (let [by-kind (frequencies (map :event/kind events))
        created (vec (for [e events :when (= :created (:event/kind e))]
                       {:object (:event/stream e) :class (get-in e [:event/body :class])}))
        fields (->> events (filter #(= :field-set (:event/kind %)))
                    (group-by :event/stream)
                    (into {} (map (fn [[o es]] [o (count es)]))))]
    {:events (count events)
     :by-kind by-kind
     :objects-created created
     :fields-set fields
     :messages (count (filter #(and (= :created (:event/kind %)) (= :message (get-in % [:event/body :class]))) events))
     :evaluations (count (filter #(and (= :created (:event/kind %)) (= :evaluation (get-in % [:event/body :class]))) events))
     :first-seq (some-> events first :event/seq)
     :last-seq (some-> events peek :event/seq)}))

(defn since-fork!
  "{:after :limit} over this world's OWN events (seq > the fork seq), each through resolution, with
   a summary of all of them. Not a fork -> {:err conflict}."
  [world p {:keys [after limit]}]
  (if-let [at (log/fork-seq (:log world))]
    (let [limit (min max-limit (or limit 256))
          [db _] (pinned (:index world))
          own (loop [acc [] from (inc at)]
                (let [batch (log/read-own-from (:log world) from 1000)]
                  (if (empty? batch) acc (recur (into acc batch) (inc (:event/seq (peek batch)))))))
          visible (filterv #(visible-event? db p %) own)
          page (vec (take limit (drop-while #(<= (:event/seq %) (or after at)) visible)))
          last-seq (some-> page peek :event/seq)]
      {:ok (cond-> {:events page :fork {:forked-at at :parent (str (get-in world [:fork :parent :data-dir]))}
                    :summary (summarize visible)}
             (and last-seq (seq (drop-while #(<= (:event/seq %) last-seq) visible))) (assoc :next last-seq))})
    {:err {:code :conflict :message "this world is not a fork"}}))

(defn newest!
  "The newest page: `last` visible events (default `limit`, clamped) with seq below
   `before` (default past the head), ascending; `prev` is the seq to pass as `before` for the page
   before it, present when older visible events remain. Reads the log backwards in chunks."
  [world p {:keys [before last limit]}]
  (let [head (log/head (:log world))
        end (long (or before (inc head)))
        want (long (min max-limit (or last limit 256)))
        [db _] (pinned (:index world))
        visible (loop [acc [] hi (dec end)]
                  (if (or (< hi 1) (> (count acc) want))
                    acc
                    (let [lo (max 1 (- hi 999))
                          batch (log/read-from (:log world) lo (inc (- hi lo)))
                          keep (filterv #(visible-event? db p %) batch)]
                      (recur (into keep acc) (dec lo)))))
        page (vec (take-last want visible))
        more? (> (count visible) (count page))]
    {:ok (cond-> {:events page}
           (and more? (seq page)) (assoc :prev (:event/seq (first page))))}))

(defn thread!
  "`history {thread <id>}`: the Messages of a thread, oldest first, each in the read
   shape the principal may see (resolution per Message), with the asker and the count. For clients
   and for a loop following up a search hit. An id that is no Message -> err unresolved."
  [world p {:keys [thread]}]
  (let [[db _] (pinned (:index world))
        t (cond (uuid? thread) thread (string? thread) (parse-uuid thread))
        root (when t (objects/state db t))]
    (if-not (and root (= :message (:object/class root)))
      {:err {:code :unresolved :message "no such thread (its id is the root Message's)"}}
      (let [ids (let [ms (thread/messages db t)] (if (some #{t} ms) ms (into [t] ms)))
            visible (filterv #(resolve/resolves? db p (act/id-name %) :read) ids)]
        {:ok {:events [] :thread t :asked-by (thread/asked-by db t) :count (count visible)
              :messages (mapv #(state-for db p %) visible)}}))))

(defn- iso [^java.util.Date d] (when d (str (.toInstant d))))

(defn conversations!
  "Conversations as things (decisions 2026-10-31): {:principal? :limit? :before?} -> newest first
   {:thread :route :participants :title :first-at :last-at :messages :tokens-in :tokens-out :compressed?},
   only those whose root the caller resolves for :read. `next` is the last-seq to pass as before."
  [world p {:keys [principal limit before]}]
  (let [db (act/db world)
        limit (min 200 (or limit 50))
        field (fn [o k] (get-in (objects/fields db o) [k :field/value]))
        by-thread (group-by #(thread/thread-of db %) (objects/objects-of-class db :message))
        usage (reduce (fn [acc e]
                        (let [t (some->> (field e :evaluation/message) (thread/thread-of db))
                              c (field e :evaluation/context)]
                          (cond-> acc
                            (and t c) (-> (update-in [t :in] (fnil + 0) (or (:prompt-tokens c) 0))
                                          (update-in [t :out] (fnil + 0) (or (:completion-tokens c) 0))))))
                      {} (objects/objects-of-class db :evaluation))
        row (fn [[t ms]]
              (let [ms (sort-by #(:object/created-seq (objects/entity db %)) ms)
                    at (fn [m] (get-in (objects/fields db m) [:message/text :field/set-at]))
                    parts (distinct (mapcat (fn [m] (cons (field m :message/from) (field m :message/to))) ms))]
                {:thread t :route (field t :message/route) :participants (vec parts)
                 :title (route/title db t) :first-at (iso (at (first ms))) :last-at (iso (at (last ms)))
                 :last-seq (:object/created-seq (objects/entity db (last ms)))
                 :messages (count ms) :tokens-in (get-in usage [t :in] 0) :tokens-out (get-in usage [t :out] 0)
                 :compressed (some? (field t :conversation/summary))}))
        rows (->> by-thread
                  (filter (fn [[t _]] (resolve/resolves? db p (act/id-name t) :read)))
                  (map row)
                  (filter #(or (nil? principal) (some #{principal} (:participants %))))
                  (filter #(or (nil? before) (< (:last-seq %) before)))
                  (sort-by :last-seq >))
        page (vec (take limit rows))]
    {:ok (cond-> {:events [] :conversations page}
           (< (count page) (count rows)) (assoc :next (:last-seq (peek page))))}))

(defn history!
  "The history op without a stream: {:after :limit :snapshot :cursor|:next} -> {:ok {...}}.
   Plain: events after `after`, `limit` per page (clamped), `next` the seq to continue from.
   Snapshot: `limit` is objects per page (clamped), `next` (or `cursor`) the object cursor to pass
   back, `events` the tail after the snapshot seq (default-tail of it; a client subscribes after
   snapshot.seq for the rest), as the listener's boot consumes it."
  [world p {:keys [after limit cursor next before last] snapshot? :snapshot since-fork? :since-fork :as args}]
  (cond
    (:conversations args) (conversations! world p args)
    (:thread args) (thread! world p args)
    since-fork? (since-fork! world p args)
    (and (not snapshot?) (or before last)) (newest! world p args)
    :else
  (let [[pin-seq cursor] (parse-cursor (or cursor (when (string? next) next)))
        limit (min max-limit (or limit (if snapshot? default-objects-per-page 256)))
        world-id (:world-id world)
        [db at] (or (when pin-seq (some-> (get @pins [world-id pin-seq]) :db (vector pin-seq)))
                    (pinned (:index world)))
        _ (when (and snapshot? (nil? cursor)) (remember-pin! world-id at db))
        snap (when snapshot? (snapshot db at p cursor limit (:retired args) (get-in @pins [[world-id at] :ids])))
        from (if snapshot? at (or after 0))
        ;; a snapshot's tail comes once, with its first page
        n-events (if snapshot? (if cursor 0 default-tail) limit)
        events (loop [acc [] from from]
                 (let [batch (when (pos? n-events) (log/read-from (:log world) (inc from) n-events))
                       keep (filter #(visible-event? db p %) batch)
                       acc (into acc keep)]
                   (cond (or (empty? batch) (>= (count acc) n-events)) (vec (take n-events acc))
                         ;; a page of unreadable events is skipped, the page after it read
                         :else (recur acc (:event/seq (peek (vec batch)))))))
        last-seq (some-> events peek :event/seq)]
    (when (and snapshot? (nil? cursor))
      (act/append! world (log/pending :resolved p p {:name "*" :op :read :count (:total snap) :seq at})))
    {:ok (cond-> {:events (mapv #(refs/visible-event db p %) events)}
           (and (not snapshot?) last-seq (= (count events) n-events)) (assoc :next last-seq)
           snap (assoc :snapshot (dissoc snap :next))
           (:next snap) (assoc :next (:next snap)))})))
