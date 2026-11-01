(ns knowledge.world.protocol.coalesce
  "Push coalescing limits (protocol.md Transport; ADR 0001): a push carries at most 256 events
   and 1 MiB. The 16 ms cadence lives in the subscription pump."
  (:require [knowledge.world.protocol.frames :as frames]))

(def max-events 256)
(def max-bytes (* 1024 1024))
(def interval-ms 16)

(defn split-push
  "[[frame encoded-string] ...]: the push split until every encoding fits in max-bytes."
  [encode push]
  (let [s (encode push)]
    (if (or (<= (count s) max-bytes) (<= (count (:events push)) 1))
      [[push s]]
      (let [[a b] (split-at (quot (count (:events push)) 2) (:events push))]
        (concat (split-push encode (assoc push :events (vec a) :lagging nil))
                (split-push encode (cond-> (assoc push :events (vec b))
                                     (:from push) (assoc :from (:event/seq (last a))))))))))

(defn pushes
  "Encoded pushes for events on one sub. from is the cursor the batch was read from; each later
   chunk's from is the last seq of the chunk before it."
  ([encode sub events as-of lagging?] (pushes encode sub events as-of lagging? nil))
  ([encode sub events as-of lagging? from]
   (let [chunks (partition-all max-events events)
         froms (reductions (fn [_ chunk] (:event/seq (last chunk))) from chunks)]
     (mapcat (fn [chunk from] (split-push encode (frames/push sub chunk as-of lagging? from)))
             chunks froms))))
