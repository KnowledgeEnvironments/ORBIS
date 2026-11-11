(ns knowledge.world.log.reader
  "Reads over the log state: by global seq (read-from) and by stream (read-stream) using the
   per-segment indexes. Readers never pass a segment's committed :end, so a batch in flight is
   invisible until the writer publishes it."
  (:require [knowledge.world.log.index :as idx]
            [knowledge.world.log.segment :as seg]))

(defn- scan-segment
  "Events of a segment with seq >= from, in order, up to limit."
  [{:keys [end] :as segment} ^long from ^long limit]
  (loop [offset (idx/start-offset (:idx segment) from) acc (transient [])]
    (if (>= (count acc) limit)
      (persistent! acc)
      (let [{:keys [status event next]} (seg/read-at segment offset end)]
        (if (= status :ok)
          (recur (long next) (if (>= (:event/seq event) from) (conj! acc event) acc))
          (persistent! acc))))))

(defn all-segments
  "The segments a reader walks, in order: the parent's (a fork) then the world's own.
   A parent segment's index may run past the fork seq (the parent went on); readers cap at it."
  [st]
  (into (vec (get-in st [:parent :segments])) (:segments st)))

(defn first-seq
  "The oldest seq a reader can still reach. 1 on a log nothing has pruned."
  ^long [st]
  (long (or (some-> (first (all-segments st)) :first-seq) 1)))

(defn- cap-of [st] (get-in st [:parent :seq]))

(defn- parent-segment? [st segment] (some #(identical? % segment) (get-in st [:parent :segments])))

(defn read-from
  "Up to `limit` events with event/seq >= seq, in log order; through a fork's parent first, capped
   at the fork seq."
  [st ^long seq ^long limit]
  (let [cap (cap-of st)]
    (loop [segments (drop-while #(< (get-in % [:idx :last-seq]) seq) (all-segments st))
           acc []]
      (if (or (empty? segments) (>= (count acc) limit))
        acc
        (let [seg (first segments)
              events (scan-segment seg seq (- limit (count acc)))
              events (if (and cap (parent-segment? st seg)) (filterv #(<= (:event/seq %) cap) events) events)]
          (recur (rest segments) (into acc events)))))))

(defn read-own-from
  "Up to `limit` of the world's OWN events (not the parent's) with seq >= seq (a fork's diff)."
  [st ^long seq ^long limit]
  (read-from (dissoc st :parent) seq limit))

(defn read-stream
  "Up to `limit` events on `stream` with seq > after, in log order; through a fork's parent first."
  [st stream ^long after ^long limit]
  (loop [segments (drop-while #(<= (get-in % [:idx :last-seq]) after) (all-segments st))
         acc []]
    (if (or (empty? segments) (>= (count acc) limit))
      acc
      (let [{:keys [end] :as segment} (first segments)
            cap (when (parent-segment? st segment) (cap-of st))
            events (->> (idx/stream-positions (:idx segment) stream after)
                        (take-while (fn [[sq _]] (or (nil? cap) (<= sq cap))))
                        (take (- limit (count acc)))
                        (keep (fn [[_ off]] (let [r (seg/read-at segment off end)]
                                              (when (= (:status r) :ok) (:event r))))))]
        (recur (rest segments) (into acc events))))))
