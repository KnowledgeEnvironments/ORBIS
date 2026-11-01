(ns knowledge.world.index.core
  "The Datascript index (spec 13.1, 17.1): rebuilt from the log at start (newest snapshot plus
   replay), kept current by a log subscription and by on-demand catch-up, snapshotted every 10k
   events and on graceful stop. Holds no fact that is not in the log (ground rule 3)."
  (:require [clojure.java.io :as io]
            [datascript.core :as d]
            [knowledge.world.fold.core :as fold]
            [knowledge.world.index.snapshot :as snap]
            [knowledge.world.log.core :as log]
            [taoensso.timbre :as timbre]))

(def snapshot-every 10000)
(def chunk-size 1000)

(defn create
  "An index over log for the world with id world-id, snapshots under data-dir/snapshots."
  [log data-dir world-id]
  {:log log
   :dir (io/file data-dir "snapshots")
   :anchor (snap/anchor-dir data-dir)
   :db (atom (d/db-with (fold/empty-db) [{:world/id world-id}]))
   :applied (atom 0)
   :since-snapshot (atom 0)
   :lock (Object.)
   :sub-key (atom nil)})

(defn at
  "An index over an in-memory log that starts from another index's db at seq, without replay and
   with no snapshot directory: nothing it holds reaches a disk (an off-the-record session)."
  [log world-id base-db seq]
  {:log log :dir nil :db (atom base-db) :applied (atom seq) :since-snapshot (atom 0)
   :lock (Object.) :sub-key (atom nil) :world-id world-id})

(defn- ingest-chunk! [{:keys [db applied since-snapshot dir]} events]
  (let [fresh (filter #(> (:event/seq %) @applied) events)]
    (when (seq fresh)
      (swap! db fold/apply-events fresh)
      (reset! applied (:event/seq (last fresh)))
      (swap! since-snapshot + (count fresh))
      (when (and dir (>= @since-snapshot snapshot-every))
        (reset! since-snapshot 0)
        (snap/write! dir @applied @db)
        (snap/prune! dir 3)))))

(defn catch-up!
  "Apply every committed event the index has not seen. Safe from any thread."
  [{:keys [log applied lock] :as idx}]
  (locking lock
    (loop []
      (let [from (inc @applied)]
        (when (<= from (log/head log))
          ;; read-from goes on from what is there: a fold over that would be a different world
          (when-let [f (log/gap log from)]
            (throw (ex-info "the log no longer holds the events this index needs"
                            {:type :log/gap :wanted from :first-seq f})))
          (let [events (log/read-from log from chunk-size)]
            (when (seq events)
              (ingest-chunk! idx events)
              (recur)))))))
  idx)

(defn db
  "The current db, caught up to the log head."
  [idx]
  (catch-up! idx)
  @(:db idx))

(defn applied-seq [idx] @(:applied idx))

(defn- refuse-boot!
  "The log's first segment is past every snapshot this release can load. Today's world is not
   recoverable from what is here, and a fold of what is left would be a world missing its past
   that looks fine. So stop, and say what was looked at: on a new release the supervisor goes back to
   the last good one, whose snapshots are still there."
  [{:keys [log dir anchor]} floor]
  (let [names (fn [d] (mapv #(.getName ^java.io.File %) (snap/snapshot-files d)))
        msg (str "this world's log starts at seq " floor " and no snapshot this release can load reaches "
                 (dec floor) ": refusing to start rather than fold part of a world. Looked in "
                 dir " " (names dir) " and " anchor " " (names anchor)
                 ". Restore a snapshot from a backup, or run the release that wrote these.")]
    (timbre/error msg)
    (throw (ex-info msg {:type :log/unrecoverable :first-seq floor :head (log/head log) :reads snap/schema-version}))))

(defn start!
  "Load the newest snapshot that is any use, replay the rest, subscribe to the log. On a pruned log a
   snapshot is required: the fold from seq 1 does not exist any more."
  [{:keys [log dir anchor db applied sub-key] :as idx}]
  (let [floor (log/first-seq log)
        found (snap/load-newest (remove nil? [dir anchor]) (log/head log) (dec floor))]
    (when (and (nil? found) (> floor 1)) (refuse-boot! idx floor))
    (when-let [{s :seq sdb :db} found]
      (timbre/info "index from snapshot" {:seq s})
      (reset! db (fold/current-schema sdb))
      (reset! applied s)))
  (catch-up! idx)
  (reset! sub-key (log/subscribe! log (fn [_batch] (catch-up! idx))))
  (timbre/info "index ready" {:applied @applied})
  idx)

(defn stop!
  "Unsubscribe and write a final snapshot."
  [{:keys [log dir db applied sub-key] :as idx}]
  (when-let [k @sub-key] (log/unsubscribe! log k) (reset! sub-key nil))
  (catch-up! idx)
  (when (pos? @applied)
    (try (snap/write! dir @applied @db) (snap/prune! dir 3)
         (catch Exception e (timbre/warn e "snapshot on stop failed"))))
  idx)
