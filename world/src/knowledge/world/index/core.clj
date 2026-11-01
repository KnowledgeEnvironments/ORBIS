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

(defn start!
  "Load the newest snapshot, replay the rest, subscribe to the log."
  [{:keys [log dir db applied sub-key] :as idx}]
  (when-let [{s :seq sdb :db} (snap/load-newest dir (log/head log))]
    (timbre/info "index from snapshot" {:seq s})
    (reset! db sdb)
    (reset! applied s))
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
