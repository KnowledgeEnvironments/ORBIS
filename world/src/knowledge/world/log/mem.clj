(ns knowledge.world.log.mem
  "The events of an in-memory log (an off-the-record session): seqs continue from the fork seq,
   nothing is written anywhere, and close! forgets them. Validated like the writer validates, so
   an event that could not reach the real log cannot pass here either."
  (:require [knowledge.schema.registry :as r]
            [knowledge.world.log.index :as idx]))

(defn create [base] (atom {:head base :events [] :subscribers {} :closed? false}))

(defn head ^long [mem] (:head @mem))

(defn append!
  "Assign the next seq, keep the event, tell subscribers on this thread. Returns the seq."
  [mem event]
  (let [ev (volatile! nil)]
    (swap! mem (fn [{:keys [closed? head] :as m}]
                 (when closed? (throw (ex-info "in-memory log closed" {})))
                 (let [e (assoc event :event/seq (inc head) :event/id (random-uuid))]
                   (when-not (r/valid? :ke/Event e)
                     (throw (ex-info "event invalid" {:kind (:event/kind event)})))
                   (vreset! ev e)
                   (-> m (assoc :head (inc head)) (update :events conj e)))))
    (doseq [[_ f] (:subscribers @mem)]
      (try (f [@ev]) (catch Throwable _ nil)))
    (:event/seq @ev)))

(defn read-from
  "Up to limit held events with seq >= from."
  [mem from limit]
  (into [] (comp (drop-while #(< (:event/seq %) from)) (take limit)) (:events @mem)))

(defn read-stream
  "Held events on stream with seq > after."
  [mem stream after]
  (filterv #(and (> (:event/seq %) after) (contains? (idx/streams-of %) stream)) (:events @mem)))

(defn stream-head [mem stream]
  (some->> (:events @mem) (filter #(contains? (idx/streams-of %) stream)) last :event/seq))

(defn subscribe! [mem k f] (swap! mem assoc-in [:subscribers k] f))
(defn unsubscribe! [mem k] (swap! mem update :subscribers dissoc k))

(defn close! [mem] (swap! mem assoc :events [] :subscribers {} :closed? true))
