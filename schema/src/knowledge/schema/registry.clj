(ns knowledge.schema.registry
  "One registry for everything; `all-schemas` is what the JSON export and the
   schema Objects (spec 7, principle 7) iterate."
  (:require [knowledge.schema.core :as c]
            [knowledge.schema.classes :as cls]
            [knowledge.schema.events :as ev]
            [knowledge.schema.protocol :as p]
            [malli.core :as m]))

(def named
  (merge
    {:ke/Class cls/ClassName :ke/Field cls/Field :ke/Object cls/ObjectSchema :ke/Principal cls/Principal
     :ke/Ref cls/Ref
     :ke/Serialization cls/Serialization :ke/Group cls/Group :ke/Message cls/Message
     :ke/Task cls/Task :ke/Evaluation cls/Evaluation :ke/Trigger cls/Trigger :ke/Model cls/Model
     :ke/Route cls/Route :ke/Grant cls/Grant :ke/Adapter cls/Adapter
     :ke/Event ev/Event :ke/PendingEvent ev/PendingEvent :ke/Kind ev/Kind
     :ke/Request p/Request :ke/Response p/Response :ke/Push p/Push :ke/PaneFrame p/PaneFrame :ke/Ack p/Ack :ke/Frame p/Frame
     :ke/Err p/Err :ke/OpName p/OpName}
    (into {} (map (fn [[k v]] [(keyword "ke.args" (name k)) v]) p/args))
    (into {} (map (fn [[k v]] [(keyword "ke.result" (name k)) v]) p/results))))

(def registry (c/registry named))
(def opts {:registry registry})

(defn schema [k] (m/schema (named k) opts))
(defn valid? [k v] (m/validate (schema k) v))
(defn explain [k v] (m/explain (schema k) v))

(defn all-schemas []
  (for [[k v] (sort-by (comp str key) named)]
    {:id k :name (name k) :form (m/form (m/schema v opts))}))
