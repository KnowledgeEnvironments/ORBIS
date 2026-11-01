(ns knowledge.world.log.writer
  "The single writer thread (ADR 0003, 0006). append! enqueues [pending-event promise]; the writer
   drains up to 1000, assigns global seqs, writes one buffer, forces once per batch (per KE_FSYNC),
   delivers seqs to the promises, then notifies subscribers with the batch (group commit).
   seq is the log position: the head plus one, never reused, never skipped."
  (:require [knowledge.schema.registry :as r]
            [knowledge.world.log.frame :as frame]
            [knowledge.world.log.index :as idx]
            [knowledge.world.log.segment :as seg]
            [taoensso.timbre :as log])
  (:import [java.io ByteArrayOutputStream]
           [java.util ArrayList]
           [java.util.concurrent LinkedBlockingQueue TimeUnit]))

(def batch-size 1000)

(defn- humanize [k v] ((requiring-resolve 'malli.error/humanize) (r/explain k v)))

(defn- roll!
  "Seal the active segment (persist its index) and open the next one at first-seq."
  [st first-seq]
  (let [active (peek (:segments st))]
    (idx/write! (:file active) (:idx active))
    (let [fresh (assoc (seg/create! (:dir st) first-seq) :idx (idx/empty-index first-seq))]
      (seg/fsync-dir! (:dir st))
      (update st :segments conj fresh))))

(defn- encode-batch
  "Assign seqs and encode. Returns [entries failures] where an entry is [event promise bytes]."
  [head items]
  (loop [items items seq head entries [] failures []]
    (if-let [[ev prom] (first items)]
      (let [ev' (cond-> (assoc ev :event/seq (inc seq))
                  (nil? (:event/id ev)) (assoc :event/id (random-uuid)))]
        (if-not (r/valid? :ke/Event ev')
          (recur (rest items) seq entries
                 (conj failures [prom (ex-info "event invalid after seq assignment" {:explain (humanize :ke/Event ev')})]))
          (let [bs (try (frame/frame-bytes (frame/encode-payload ev')) (catch Exception e e))]
            (if (instance? Throwable bs)
              (recur (rest items) seq entries (conj failures [prom bs]))
              (recur (rest items) (inc seq) (conj entries [ev' prom bs]) failures)))))
      [entries failures])))

(defn- write-entries!
  "Write the entries' frames to the active segment per fsync policy; returns st' with the index
   and end advanced and the head moved."
  [st entries fsync]
  (let [st (if (seg/full? (peek (:segments st))) (roll! st (inc (:head st))) st)
        active (peek (:segments st))
        [active' _]
        (if (= fsync :always)
          ;; one write and one force per frame
          (reduce (fn [[a _] [ev _ bs]]
                    (let [off (:end a) end (seg/append-bytes! a bs)]
                      (seg/force! a)
                      [(-> a (assoc :end end) (update :idx idx/add-frame ev off end)) nil]))
                  [active nil] entries)
          ;; one write for the batch, one force (or none)
          (let [out (ByteArrayOutputStream.)
                [a offsets] (reduce (fn [[a offs] [_ _ ^bytes bs]]
                                      (.write out bs)
                                      [(update a :end + (alength bs)) (conj offs (:end a))])
                                    [active []] entries)
                _ (seg/append-bytes! active (.toByteArray out))
                _ (when (= fsync :batch) (seg/force! active))
                a (reduce (fn [a [[ev _ ^bytes bs] off]]
                            (update a :idx idx/add-frame ev off (+ off (alength bs))))
                          a (map vector entries offsets))]
            [a nil]))
        last-seq (:event/seq (first (peek entries)))]
    (-> st
        (assoc :segments (conj (pop (:segments st)) active'))
        (assoc :head last-seq))))

(defn- notify! [subscribers events]
  (doseq [[k f] subscribers]
    (try (f events)
         (catch Throwable t (log/error t "log subscriber failed" {:subscriber k})))))

(defn- run-batch! [state fsync ^ArrayList batch]
  (let [items (remove #(= % ::close) batch)
        closing? (some #(= % ::close) batch)]
    (when (seq items)
      (let [st @state
            [entries failures] (encode-batch (:head st) items)]
        (doseq [[prom ex] failures] (deliver prom ex))
        (when (seq entries)
          (let [st' (try (write-entries! st entries fsync)
                         (catch Throwable t t))]
            (if (instance? Throwable st')
              (do (log/error st' "log write failed")
                  (doseq [[_ prom _] entries] (deliver prom st')))
              ;; a subscribe! during the write must survive it: state read before the write and put back
              ;; whole lost the resident loop's subscription on 2 fresh boots of 5, a world that took
              ;; Messages and never woke
              (let [now (swap! state (fn [current] (assoc st' :subscribers (:subscribers current))))]
                (doseq [[ev prom _] entries] (deliver prom (:event/seq ev)))
                (notify! (:subscribers now) (mapv first entries))))))))
    closing?))

(defn start!
  "Start the writer over the log state atom. Returns {:queue :thread}."
  [state fsync]
  (let [queue (LinkedBlockingQueue.)
        thread (Thread.
                 (fn []
                   (loop []
                     (let [first (.take queue)
                           batch (ArrayList.)]
                       (.add batch first)
                       (.drainTo queue batch (dec batch-size))
                       (let [closing? (try (run-batch! state fsync batch)
                                           (catch Throwable t (log/error t "writer batch failed") false))]
                         (when-not closing? (recur))))))
                 "ke-log-writer")]
    (.setDaemon thread true)
    (.start thread)
    {:queue queue :thread thread}))

(defn enqueue! [{:keys [^LinkedBlockingQueue queue]} item] (.put queue item))

(defn stop!
  "Drain and stop; waits for the thread up to 10 s."
  [{:keys [^LinkedBlockingQueue queue ^Thread thread]}]
  (.put queue ::close)
  (.join thread (.toMillis TimeUnit/SECONDS 10)))
