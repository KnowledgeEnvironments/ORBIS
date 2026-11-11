(ns knowledge.world.fold.objects
  "Object state out of the folded db (spec 4.1): the state map, names, fields, and the
   namespace lookup resolution starts from. `state-at` replays a stream to a seq."
  (:require [datascript.core :as d]
            [knowledge.world.fold.core :as fold]))

(defn entity [db id] (when id (d/entity db [:object/id id])))
(defn exists? [db id] (some? (:object/class (entity db id))))
(defn retired? [db id] (boolean (:object/retired? (entity db id))))

(defn field-map
  "The Field record (spec 4.3) of one component entity."
  [f]
  (into {} (remove (comp nil? val))
        {:field/name (:field/name f) :field/type (:field/type f) :field/value (:field/value f)
         :field/reader (:field/reader f) :field/author (:field/author f) :field/set-at (:field/set-at f)
         :field/set-by (:field/set-by f) :field/visibility (:field/visibility f) :field/mode (:field/mode f)
         :field/reads (:field/reads f) :field/set-seq (:field/set-seq f)}))

(defn fields
  "map field-name -> Field record."
  [db id]
  (into {} (map (fn [f] [(:field/name f) (field-map f)])) (:object/fields (entity db id))))

(defn names
  "#{[namespace-id name] ...}."
  [db id]
  (into #{} (map fold/split-name-key) (:object/names (entity db id))))

(defn state
  "The present state of an Object as the ontology names it, or nil."
  [db id]
  (when-let [e (entity db id)]
    (when (:object/class e)
      (cond-> {:object/id id :object/class (:object/class e) :object/stream id
               :object/names (names db id) :object/fields (fields db id)}
        (:object/retired? e) (assoc :object/retired true)
        (:principal/driven-by e) (assoc :principal/driven-by (:principal/driven-by e))
        (:principal/namespace e) (assoc :principal/namespace (:principal/namespace e))
        (seq (:group/members e)) (assoc :group/members (set (:group/members e)))
        (:grant/grantee e) (merge {:grant/grantor (:grant/grantor e) :grant/grantee (:grant/grantee e)
                                   :grant/scope (:grant/scope e) :grant/ops (:grant/ops e)
                                   :grant/delegable (:grant/delegable e)}
                                  (when (:grant/until e) {:grant/until (:grant/until e)})
                                  (when (:grant/revoked? e) {:grant/revoked true}))))))

(defn lookup
  "The object bound at name in namespace-id, or nil."
  [db namespace-id n]
  (d/q '[:find ?id . :in $ ?k :where [?o :object/names ?k] [?o :object/id ?id]]
       db (fold/name-key namespace-id n)))

(defn bound-names
  "name -> object id for a namespace."
  [db namespace-id]
  (let [prefix (str namespace-id "/")]
    (into {} (for [[k id] (d/q '[:find ?k ?id :where [?o :object/names ?k] [?o :object/id ?id]] db)
                   :when (.startsWith ^String k prefix)]
               [(subs k (count prefix)) id]))))

(defn objects-with-field
  "ids of objects carrying a field named k."
  [db k]
  (d/q '[:find [?id ...] :in $ ?k :where [?f :field/name ?k] [?o :object/fields ?f] [?o :object/id ?id]] db k))

(defn objects-of-class
  "ids of the Objects of one class."
  [db class]
  (d/q '[:find [?id ...] :in $ ?c :where [?o :object/class ?c] [?o :object/id ?id]] db class))

(defn world-id [db] (d/q '[:find ?w . :where [_ :world/id ?w]] db))

(defn root-id
  "The root Principal: the world Principal's world/root Field, set at boot, so a rename of root
   changes nothing that finds it. The bound name is only the fallback for a world not upgraded yet."
  [db]
  (let [w (world-id db)]
    (or (get-in (fields db w) [:world/root :field/value])
        (lookup db w "root"))))

(defn state-at
  "Fold one stream up to seq (spec 7.2) from the log; nice-to-have, used by nothing yet."
  [read-stream stream seq]
  (let [events (take-while #(<= (:event/seq %) seq) (read-stream stream 0 Long/MAX_VALUE))]
    (state (fold/apply-events (fold/empty-db) events) stream)))
