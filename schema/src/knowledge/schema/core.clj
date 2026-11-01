(ns knowledge.schema.core
  "Shared primitives for every schema in the knowledge environment.
   Field value types (spec 4.3): string number boolean instant keyword bytes ref list map."
  (:require [malli.core :as m]))

(def Id :uuid)
(def Instant inst?)
(def Seq [:int {:min 0}])
(def Name
  "A dot-separated, case-preserved name bound in a namespace (spec 6.4)."
  [:string {:min 1 :max 512}])

(def FieldTypeKey
  [:enum :string :number :boolean :instant :keyword :bytes :ref :list :map])

(def FieldType
  "A field type is a keyword, or a vector for parameterised types:
   [:ref <class>] [:list <type>] [:map <type>]."
  [:or FieldTypeKey
   [:tuple [:= :ref] :keyword]
   [:tuple [:= :list] [:ref ::field-type]]
   [:tuple [:= :map] [:ref ::field-type]]])

(def Reader [:enum :human :agent :both])
(def DrivenBy
  "spec: person | process | nil. :none stands for nil (an office appliance)."
  [:enum :person :process :none])
(def Mode [:enum :inline :blob :external])
(def Op [:enum :read :reference :transmit :write :delegate :evaluate :create])
(def Residency [:enum :local :tenant :external])

(def Blob
  "Receipt for bytes held in the content-addressed store (spec 12.1)."
  [:map [:blob/sha256 [:string {:min 64 :max 64}]] [:blob/size :int]])

(def External
  "Receipt for bytes held by an adapter (spec 12.1)."
  [:map [:external/model Id] [:external/key :string]
   [:external/sha256 {:optional true} :string] [:external/last-seen {:optional true} Instant]])

(def Value
  "A field value. Literal, reference (an Id), or a receipt."
  [:or :string :double :int :boolean Instant :keyword bytes? Id Blob External
   [:vector [:ref ::value]] [:map-of :keyword [:ref ::value]]])

(def base-registry
  {::id Id ::instant Instant ::seq Seq ::name Name ::field-type FieldType ::reader Reader
   ::driven-by DrivenBy ::mode Mode ::op Op ::residency Residency ::blob Blob ::external External
   ::value Value})

(defn registry
  "A malli registry: defaults plus ours plus extra."
  [& extras]
  (apply merge (m/default-schemas) base-registry extras))
