(ns knowledge.world.system
  "The world as a map of components (spec 13.1), started and stopped by plain functions.
   start: data dir and world.edn, blob store and log, index (snapshot + replay), kernel-zero (and
   its steps on a fresh log), the resident loop (spec 9.8), sessions, the Trigger scheduler, the log
   subscription that wakes session pumps, the WebSocket server."
  (:require [clojure.edn :as edn]
            [clojure.java.io :as io]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.config :as config]
            [knowledge.world.index.core :as index]
            [knowledge.world.log.core :as log]
            [knowledge.world.manual :as manual]
            [knowledge.world.loop.core :as rloop]
            [knowledge.world.protocol.server :as server]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.protocol.subscription :as subscription]
            [knowledge.world.release :as release]
            [knowledge.world.retention :as retention]
            [knowledge.world.schedule.core :as schedule]
            [knowledge.world.pane.core :as pane]
            [knowledge.world.scratch.core :as scratch]
            [taoensso.timbre :as timbre])
  (:import [java.util Date]
           [java.util.concurrent Executors ScheduledExecutorService TimeUnit]))

(def config-from-env config/config-from-env)

(defn- world-edn!
  "Read or create $KE_DATA/world.edn {:world/id :format-version 1 :created-at}; a fork (world
   pass 9) also carries {:parent {:data-dir \"..\" :world id :seq N} :fork/id uuid}."
  [data-dir]
  (let [f (io/file data-dir "world.edn")]
    (if (.exists f)
      (edn/read-string (slurp f))
      (let [m {:world/id (random-uuid) :format-version 1 :created-at (Date.)}]
        (spit f (pr-str m))
        m))))

(defn- parent-of
  "{:data-dir (absolute) :seq N :world id} when world.edn names a parent, else nil."
  [data-dir {:keys [parent]}]
  (when parent
    (let [pd (io/file (:data-dir parent))
          pd (if (.isAbsolute pd) pd (io/file data-dir (:data-dir parent)))]
      (assoc parent :data-dir (.getCanonicalFile pd)))))

(defn start
  "Start a world from a config map (see knowledge.world.config). Returns the world."
  [config]
  (timbre/set-min-level! (:log-level config :info))
  (let [data-dir (config/absolute-data-dir (:data config))
        _ (.mkdirs data-dir)
        {:keys [world/id] :as world-edn} (world-edn! data-dir)
        parent (parent-of data-dir world-edn)
        log (log/open! data-dir (cond-> config parent (assoc :parent parent)))
        index (try (index/start! (index/create log data-dir id))
                   (catch Throwable t (log/close! log) (throw t)))
        scheduler (Executors/newScheduledThreadPool 2)
        sessions (session/registry)
        world (cond-> {:config config :data-dir data-dir :world-id id :world-edn world-edn
                       :log log :blobs (log/blobs log) :index index :sessions sessions :scheduler scheduler
                       ;; the wakes waiting and running, per Principal (knowledge.world.loop.queue)
                       :loop-state (atom {}) :loop-hooks (atom {})
                       ;; each Principal's notification window (knowledge.world.notify)
                       :notify-state (atom {})
                       ;; the port, delivered once the server is up, for handlers that closed over this map
                       :port* (promise)
                       ;; off-the-record sessions: in memory, gone at stop
                       :scratches (scratch/registry)
                       :panes (pane/registry)
                       ;; the socket to this world's terminal holder, when it has one
                       :holder (atom nil)
                       ;; the disk sweep's thread (knowledge.world.retention)
                       :retention (retention/component)}
                parent (assoc :fork {:parent parent :id (:fork/id world-edn)}))
        fresh (kz/kernel-zero! world)
        ;; a fork or an imported world arrives without credentials: new ones, their hashes on the log
        _ (kz/remint-tokens! world)
        _ (when parent (kz/mark-fork! world))
        _ (if fresh (kz/run-steps! world) (kz/upgrade! world))
        ;; the manual is this kernel's: a world on a new release gets its topics here
        _ (manual/ensure! world)
        ;; what this instance runs, its mounts on it, and what the supervisor left to report
        _ (release/boot! world)
        resident-loop (rloop/start! world)
        wake-key (log/subscribe! log (fn [_batch] (subscription/wake-all! world)))
        reaper (.scheduleAtFixedRate ^ScheduledExecutorService scheduler ^Runnable (fn [] (session/expire! sessions)) 30 30 TimeUnit/SECONDS)
        ;; the scheduler: armed Triggers from the log, and the approval expiry sweep
        ;; (decisions 2026-10-30 'approval gates') as its internal job
        trigger-scheduler (schedule/start! world)
        _ (pane/reconcile! world)
        pane-sweep (.scheduleAtFixedRate ^ScheduledExecutorService scheduler ^Runnable (fn [] (pane/sweep! world))
                                         5 5 java.util.concurrent.TimeUnit/SECONDS)
        scratch-sweep (.scheduleAtFixedRate ^ScheduledExecutorService scheduler ^Runnable (fn [] (scratch/sweep! world)) 30 30 TimeUnit/SECONDS)
        server (server/start! world config)
        _ (deliver (:port* world) (server/port server))
        world (assoc world :server server :wake-key wake-key :reaper reaper :scratch-sweep scratch-sweep :port (server/port server)
                     :loop resident-loop :trigger-scheduler trigger-scheduler)
        ;; who this world says it is (hello), now that the port is known
        identity (kz/world-identity! world)
        ;; data/world.pid names this process, so scripts stop this world and no other
        _ (spit (io/file data-dir "world.pid") (str (.pid (java.lang.ProcessHandle/current))))
        _ (retention/kick! world)]
    (timbre/info "world started" {:port (:port world) :data (str data-dir) :head (log/head log) :name (:name identity)})
    world))

(defn stop
  "Stop a world: server, scheduler, index snapshot, log."
  [{:keys [server ^ScheduledExecutorService scheduler log index wake-key sessions] :as world}]
  (when world
    (scratch/end-all! world :world-stop)
    (pane/stop-all! world)
    (server/stop! server)
    (schedule/stop! (:trigger-scheduler world) world)
    (rloop/stop! (:loop world) world)
    (when scheduler (.shutdownNow scheduler))
    (retention/stop! (:retention world))
    (when wake-key (log/unsubscribe! log wake-key))
    (when sessions (reset! sessions {}))
    (index/stop! index)
    (log/close! log)
    (when-let [d (:data-dir world)] (.delete (io/file d "world.pid")))
    (timbre/info "world stopped")
    nil))
