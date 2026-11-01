(ns knowledge.world.protocol.subscription
  "Subscriptions as cursors over the log (protocol.md Subscriptions; spec 14.4):
   {sub stream|streams kinds window cursor pending lagging?}. The pump for a session reads
   from (inc cursor), limited by window - unacknowledged, keeps what the filter and resolution
   admit (the session principal must resolve a stream the event is on), pushes, advances the
   cursor past everything it read. It runs at most every 16 ms per session, woken by log append,
   ack and subscribe. Acks are transport state and are not logged (ADR 0004)."
  (:require [knowledge.world.index.core :as index]
            [knowledge.world.log.core :as log]
            [knowledge.world.log.index :as lidx]
            [knowledge.world.protocol.coalesce :as coalesce]
            [knowledge.world.protocol.codec :as codec]
            [knowledge.world.refs :as refs]
            [knowledge.world.resolve.core :as resolve]
            [org.httpkit.server :as hk]
            [taoensso.timbre :as timbre])
  (:import [java.util.concurrent ScheduledExecutorService TimeUnit]))

(def default-window 256)
(def max-rounds 8)

(defn create
  "A subscription from subscribe args and the current head."
  [{:keys [sub stream streams kinds after window]} head]
  (let [cursor (or after head)]
    {:sub sub :stream stream :streams (some-> streams set) :kinds (some-> kinds set)
     :window (or window default-window) :cursor cursor :acked cursor :pending [] :lagging? false}))

(defn ack
  "Advance acked: forget pushed seqs at or below seq."
  [sub seq]
  (-> sub
      (update :acked max seq)
      (update :pending (fn [p] (vec (remove #(<= % seq) p))))))

(defn filter-matches? [{:keys [stream streams kinds]} event]
  (let [on (lidx/streams-of event)]
    (and (or (nil? stream) (contains? on stream))
         (or (nil? streams) (some on streams))
         (or (nil? kinds) (contains? kinds (:event/kind event))))))

(defn resolves-event?
  "Honest filter: the principal resolves at least one stream the event is on (spec 6, 14.4)."
  [db principal event]
  (boolean (or (resolve/public-event? db event)
               (some #(resolve/resolves? db principal (str "/" %) :read) (lidx/streams-of event)))))

(defn- read-batch [log {:keys [stream cursor]} limit]
  (if stream
    (log/read-stream log stream cursor limit)
    (log/read-from log (inc cursor) limit)))

(defn- send-frames! [session frames]
  (let [{:keys [channel]} @session]
    (when channel
      (doseq [[_ s] frames] (hk/send! channel s)))))

(defn pump-sub
  "One round for one sub. Pushes what it reads and returns the delta
   {:cursor :pushed [seqs] :lagging? :more?} to merge into the live sub."
  [{:keys [log index]} session sub]
  (let [{:keys [window pending cursor]} sub
        budget (min coalesce/max-events (- window (count pending)))]
    (if (<= budget 0)
      {:cursor cursor :pushed [] :lagging? true :more? false}
      (let [events (read-batch log sub budget)]
        (if (empty? events)
          {:cursor cursor :pushed [] :lagging? (:lagging? sub) :more? false}
          (let [db (index/db index)
                principal (:principal @session)
                matched (filterv #(and (filter-matches? sub %) (resolves-event? db principal %)) events)
                matched (mapv #(refs/visible-event db principal %) matched)
                cursor' (:event/seq (peek events))
                more? (= (count events) budget)]
            (if (empty? matched)
              {:cursor cursor' :pushed [] :lagging? (:lagging? sub) :more? more?}
              (let [as-of (log/head log)
                    frames (coalesce/pushes (partial codec/encode (:format @session)) (:sub sub) matched as-of (:lagging? sub) cursor)]
                (send-frames! session frames)
                {:cursor cursor' :pushed (mapv :event/seq matched) :lagging? false :more? more?}))))))))

(defn- merge-delta
  "Apply a pump delta to the live sub; acks that raced the push win."
  [cur {:keys [cursor pushed lagging?]}]
  (when cur
    (-> cur
        (assoc :cursor (max (:cursor cur) cursor) :lagging? lagging?)
        (update :pending into (remove #(<= % (:acked cur)) pushed)))))

(defn pump!
  "Run every sub of a session for up to max-rounds. Returns true when more may be waiting."
  [world session]
  (loop [round 0]
    (if (or (>= round max-rounds) (not (:channel @session)))
      (>= round max-rounds)
      (let [more?
            (reduce (fn [acc sub-id]
                      (if-let [sub (get-in @session [:subs sub-id])]
                        (let [delta (try (pump-sub world session sub)
                                         (catch Throwable t
                                           (timbre/error t "pump failed" {:sub sub-id})
                                           {:cursor (:cursor sub) :pushed [] :lagging? false :more? false}))]
                          (swap! session update-in [:subs sub-id] merge-delta delta)
                          (or acc (:more? delta)))
                        acc))
                    false (keys (:subs @session)))]
        (if more? (recur (inc round)) false)))))

(defn wake!
  "Schedule a pump for the session, at most once per 16 ms."
  [{:keys [^ScheduledExecutorService scheduler] :as world} session]
  (let [pump (:pump @session)]
    (when (and scheduler (:channel @session))
      (let [[{:keys [scheduled? last-run]} _] (swap-vals! pump assoc :scheduled? true)]
        (when-not scheduled?
          (let [delay (max 0 (- coalesce/interval-ms (- (System/currentTimeMillis) last-run)))]
            (.schedule scheduler
                       ^Runnable (fn []
                                   (swap! pump assoc :scheduled? false :last-run (System/currentTimeMillis))
                                   (when (try (pump! world session)
                                              (catch Throwable t (timbre/error t "pump crashed") false))
                                     (wake! world session)))
                       (long delay) TimeUnit/MILLISECONDS)))))))

(defn wake-all! [{:keys [sessions] :as world}]
  (doseq [s (vals @sessions) :when (and (:channel @s) (seq (:subs @s)))]
    (wake! world s)))

(defn ack!
  "Handle an ack frame for the session's sub, then wake."
  [world session sub-id seq]
  (swap! session update-in [:subs sub-id] #(some-> % (ack seq)))
  (wake! world session))
