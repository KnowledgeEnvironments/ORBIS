(ns knowledge.schema.objects
  "Schemas are themselves Objects (spec 4.3, principle 7). This namespace turns the
   registry into the Objects kernel-zero creates: class :object, fields schema/name,
   schema/form, and instruction fields typed for agents and people."
  (:require [knowledge.schema.registry :as r]))

(def instructions
  "Shipped instruction Fields per schema key. reader :both. Extend freely; the world
   serializes these into every context that touches the class."
  {:ke/Object "Anything a name binds to. Resolve it by name; read its fields; every read is an event on your stream and its stream."
   :ke/Principal "A person, a process, or an appliance (driven-by :none). You are one. Your namespace is complete; what resolves is decided by Grants."
   :ke/Field "Typed for a reader. Set one with set-field; it carries you as author. A field on an object you do not own is your annotation, not its truth."
   :ke/Event "One fact. Never changes. seq is the world's total order."
   :ke/Serialization "Your context is one of these: a named rendering of a working set for one model. Ask for its id to replay or share it."
   :ke/Grant "What lets a name resolve for you. Delegate only what you hold; every delegation is on the log."
   :ke/Request "Send {id op args}. If an op answers not-implemented, it is not yet built; say so rather than working around it."})

(defn schema-objects
  "Seq of {:name :form :instruction} for kernel-zero to create."
  []
  (for [{:keys [id name form]} (r/all-schemas)]
    {:name (str "schema." name) :key id :form form :instruction (get instructions id)}))
