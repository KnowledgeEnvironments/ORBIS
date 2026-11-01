(ns knowledge.world.fold.core
  "The fold (spec 7.2): present state is a reduction of the log. `fold` is a multimethod on
   :event/kind returning Datascript tx-data for one event; `apply-event` applies it. The index is
   derived and disposable (ground rule 3): deleting it and replaying the log rebuilds it.

   Entity model: one entity per Object (:object/id unique), Fields as component entities
   (:field/key = \"<object-id>/<field-name>\"), names as \"<namespace-id>/<name>\" strings,
   Grants as attributes on the Grant's own Object entity, Group members and the Principal's
   driven-by and namespace projected from the class fields of the same name."
  (:require [datascript.core :as d]))

(def schema
  {:object/id        {:db/unique :db.unique/identity}
   :object/names     {:db/cardinality :db.cardinality/many}
   :object/fields    {:db/valueType :db.type/ref :db/cardinality :db.cardinality/many :db/isComponent true}
   :field/key        {:db/unique :db.unique/identity}
   :field/name       {:db/index true}
   :group/members    {:db/cardinality :db.cardinality/many}
   :grant/grantee    {:db/index true}
   :grant/grantor    {:db/index true}
   :world/id         {:db/unique :db.unique/identity}})

(defn empty-db [] (d/empty-db schema))

(defn current-schema
  "A db from a snapshot, under this release's schema. A snapshot carries the schema it was written
   with, and on a pruned log a new release cannot refold to get its own."
  [db]
  (if (= schema (:schema db)) db (d/init-db (d/datoms db :eavt) schema)))

(def structural
  "Class fields the index also keeps as entity attributes, for resolution and membership queries."
  #{:principal/driven-by :principal/namespace :group/members :object/backing})

(defn field-key [object-id field-name] (str object-id "/" (subs (str field-name) 1)))
(defn name-key [namespace-id n] (str namespace-id "/" n))

(defn split-name-key
  "\"<ns>/<name>\" -> [ns-uuid name]."
  [s]
  (let [i (.indexOf ^String s "/")]
    [(parse-uuid (subs s 0 i)) (subs s (inc i))]))

(defn infer-type [v]
  (cond (string? v) :string (boolean? v) :boolean (number? v) :number (inst? v) :instant
        (keyword? v) :keyword (bytes? v) :bytes (uuid? v) [:ref :object] (vector? v) [:list :string]
        (map? v) [:map :string] :else :string))

(defn- without-nils [m] (into {} (remove (comp nil? val)) m))

(defn- field-entity
  [object-id {:keys [event/actor event/at event/seq event/via]} {:keys [name type value reader visibility mode]} reads]
  (without-nils
    {:field/key (field-key object-id name) :field/name name :field/type type :field/value value
     :field/reader reader :field/author actor :field/set-at at :field/set-seq seq :field/set-by via
     :field/visibility visibility :field/mode mode :field/reads (or reads 0)}))

(defn- structural-tx [object-id fields]
  (let [m (select-keys fields structural)]
    (when (seq m)
      (without-nils (assoc (update m :group/members #(some-> % vec)) :object/id object-id)))))

(defmulti fold
  "tx-data for one event over the db it is applied to."
  (fn [_db event] (:event/kind event)))

(defmethod fold :default [_ _] [])

(defmethod fold :created [_ {:keys [event/stream event/actor event/seq event/body] :as ev}]
  (let [fields (:fields body)]
    (cond-> [(cond-> {:object/id stream :object/class (:class body) :object/stream stream
                      :object/created-seq seq :object/created-by actor
                      :object/fields (vec (for [[k v] fields :when (some? v)]
                                            (field-entity stream ev {:name k :type (infer-type v) :value v :reader :both} 0)))}
               ;; the Grant the creator acted under: the creator reads its own record by it (decisions 2026-10-30)
               (:event/delegation ev) (assoc :object/created-under (:event/delegation ev)))]
      (structural-tx stream fields) (conj (structural-tx stream fields)))))

(defmethod fold :bound [db {:keys [event/body]}]
  (let [{:keys [namespace name object]} body
        k (name-key namespace name)
        holders (d/q '[:find [?o ...] :in $ ?k :where [?o :object/names ?k]] db k)]
    (into (vec (for [h holders] [:db/retract h :object/names k]))
          [{:object/id object :object/names [k]}])))

(defmethod fold :unbound [db {:keys [event/body]}]
  (let [k (name-key (:namespace body) (:name body))]
    (vec (for [h (d/q '[:find [?o ...] :in $ ?k :where [?o :object/names ?k]] db k)]
           [:db/retract h :object/names k]))))

(defmethod fold :field-set [db {:keys [event/stream event/body] :as ev}]
  (let [old (d/entity db [:field/key (field-key stream (:name body))])
        tid -1]
    (if (:cleared body)
      ;; field.clear (decisions 2026-10-31): the Field leaves the fold; the log keeps its history
      (if old [[:db/retractEntity (:db/id old)]] [])
    (cond-> []
      old (conj [:db/retractEntity (:db/id old)])
      true (conj (assoc (field-entity stream ev body (:field/reads old)) :db/id tid)
                 {:object/id stream :object/fields [tid]})
      (structural (:name body)) (conj (structural-tx stream {(:name body) (:value body)}))))))

(defmethod fold :field-read [db {:keys [event/stream event/body]}]
  (vec (for [n (or (:names body) [(:name body)])
             :let [f (d/entity db [:field/key (field-key stream n)])]
             :when f]
         [:db/add (:db/id f) :field/reads (inc (or (:field/reads f) 0))])))

(defmethod fold :granted [_ {:keys [event/actor event/seq event/body]}]
  (let [{:keys [grant grantee ops scope delegable until]} body]
    [(without-nils {:object/id grant :object/class :grant :object/stream grant
                    :grant/grantor actor :grant/grantee grantee :grant/ops (set ops) :grant/scope scope
                    :grant/delegable (boolean delegable) :grant/until until :grant/seq seq})]))

(defmethod fold :revoked [_ {:keys [event/seq event/body]}]
  [{:object/id (:grant body) :grant/revoked? true :grant/revoked-seq seq}])

(defmethod fold :evaluated [db {:keys [event/stream event/body] :as ev}]
  ;; spec 9.3: the Evaluation's state, serialization, outcome and cost are the fold of its evaluated events
  (let [{:keys [state serialization outcome cost]} body
        sets (cond-> [[:evaluation/state :keyword state]]
               serialization (conj [:evaluation/serialization [:ref :serialization] serialization])
               outcome (conj [:evaluation/outcome [:map :string] outcome])
               cost (conj [:evaluation/cost [:map :number] cost]))]
    (into []
          (mapcat (fn [[n t v]]
                    (let [old (d/entity db [:field/key (field-key stream n)])
                          tid (- (inc (Math/abs (long (hash n)))))]
                      (cond-> []
                        old (conj [:db/retractEntity (:db/id old)])
                        true (conj (assoc (field-entity stream ev {:name n :type t :value v :reader :both} (:field/reads old)) :db/id tid)
                                   {:object/id stream :object/fields [tid]})))))
          sets)))

(defmethod fold :task-moved [db {:keys [event/stream event/body] :as ev}]
  ;; spec 5.3: the Task's state is the fold of its task-moved events
  (let [old (d/entity db [:field/key (field-key stream :task/state)])
        tid -1]
    (cond-> []
      old (conj [:db/retractEntity (:db/id old)])
      true (conj (assoc (field-entity stream ev {:name :task/state :type :keyword :value (:to body) :reader :both} (:field/reads old)) :db/id tid)
                 {:object/id stream :object/fields [tid]}))))

(defmethod fold :retired [_ {:keys [event/stream event/seq]}]
  [{:object/id stream :object/retired? true :object/retired-seq seq}])

(defmethod fold :restored [db {:keys [event/stream]}]
  (if-let [e (d/entity db [:object/id stream])]
    [[:db.fn/retractAttribute (:db/id e) :object/retired?]
     [:db.fn/retractAttribute (:db/id e) :object/retired-seq]]
    []))

(defn apply-event
  "db after one event."
  [db event]
  (let [tx (fold db event)]
    (if (seq tx) (d/db-with db tx) db)))

(defn apply-events [db events] (reduce apply-event db events))
