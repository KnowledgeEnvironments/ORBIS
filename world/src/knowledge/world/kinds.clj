(ns knowledge.world.kinds
  "What each kind of thing is, in plain English, for people and models alike (decisions 2026-10-31).
   The words live on the schema Objects as class/description, and for Adapters class/kinds, so the
   client and the serializer read one source. This namespace is what ships them."
  (:require [clojure.string :as str]
            [knowledge.world.fold.objects :as objects]))

(def descriptions
  {:ke/Principal "Anyone who can act here: a person, an agent, or an appliance."
   :ke/Route "A channel between Principals; Messages travel on it."
   :ke/Model "A model or program that can evaluate a prompt."
   :ke/Adapter "A connection to something outside the world."
   :ke/Task "A piece of work with a state: a run, or an approval waiting on a person."
   :ke/Message "One thing said on a Route."
   :ke/Evaluation "One call to a Model: what it was shown and what came back."
   :ke/Serialization "The exact bytes shown to a Model, or a view drawn for a person."
   :ke/Grant "Permission for a Principal to do named things to named objects."
   :ke/Trigger "A rule that sends a Message on a schedule or a change."})

(def adapter-kinds
  {"fs" "A mounted directory, read by name."
   "shell" "Runs commands on a machine, from a starting directory."
   "world" "A link to another ORBIS world."
   "gateway" "A message service, such as iMessage. Bound handles reach an agent; their words carry no authority."
   "listener" "The connection people's clients attach through."})

(def ^:private binds->kind {:filesystem "fs" :host "shell" :world "world" :transport "listener" :gateway "gateway"})

(defn fields-for
  "The Fields a schema Object carries for its key, or nil for a key with no description."
  [key]
  (when-let [d (descriptions key)]
    (cond-> {:class/description d}
      (= key :ke/Adapter) (assoc :class/kinds adapter-kinds))))

(def types {:class/description :string :class/kinds [:map :string]})

(defn kind-of
  "[label words] for an Object, read from its schema Object, or nil. An Adapter is labelled by what it
   binds: `fs adapter`, `shell adapter`."
  [db object]
  (let [state (objects/state db object)
        cls (:object/class state)
        schema (when cls (objects/lookup db (objects/world-id db) (str "schema." (str/capitalize (name cls)))))
        f (when schema (objects/fields db schema))
        d (get-in f [:class/description :field/value])]
    (when d
      (if (= :adapter cls)
        (let [k (binds->kind (some-> (get-in state [:object/fields :adapter/binds :field/value]) name keyword))
              w (get (get-in f [:class/kinds :field/value]) (keyword k) (get (get-in f [:class/kinds :field/value]) k))]
          [(str k " adapter") (or w d)])
        [(name cls) d]))))

(defn glossary
  "One line for the kinds present among objects, each kind once, or nil."
  [db objects]
  (let [kinds (distinct (keep #(kind-of db %) objects))]
    (when (seq kinds)
      (str "kinds: " (str/join " | " (map (fn [[l w]] (str l ": " w)) kinds))
           " (a name like x.fs is only a convention)"))))
