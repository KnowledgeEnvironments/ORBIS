(ns knowledge.world.protocol.frames
  "Frame constructors (protocol.md: request, response, push, ack)."
  (:require [knowledge.schema.protocol :as p]))

(defn ok [id result seq] {:id id :ok true :result result :seq seq})

(defn err
  ([id code message seq] (err id code message seq nil))
  ([id code message seq data]
   {:id id :ok false :err (cond-> {:code code :message message} data (assoc :data data)) :seq seq}))

(defn push
  "A push frame; from is the cursor the page was read from, so a client tells a
   resolution-filtered hole (seqs between from and the first event) from a lost delivery."
  ([sub events as-of lagging?] (push sub events as-of lagging? nil))
  ([sub events as-of lagging? from]
   (cond-> {:sub sub :events (vec events) :as-of as-of}
     lagging? (assoc :lagging true)
     (some? from) (assoc :from from))))

(def kind p/frame-kind)

(def not-implemented-message
  "Error message for every registered op that is not built yet."
  (fn [op] (str "op " (name op) " is not built yet")))
