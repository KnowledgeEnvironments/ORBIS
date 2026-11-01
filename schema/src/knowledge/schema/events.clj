(ns knowledge.schema.events
  "Event record (spec 4.4) and the seventeen kinds with their bodies (spec 7.1).
   seq is global over the world log (ADR 0003); stream is a partition key."
  (:require [knowledge.schema.core :as c]
            [knowledge.schema.classes :as cls]))

(def Kind
  [:enum :created :bound :unbound :field-set :field-read :resolved :granted :revoked
   :sent :delivered :task-moved :evaluated :fired :attached :detached :retired :restored :scratch-began :scratch-ended
   :pane-opened :pane-input :pane-output :pane-edit :pane-closed])

(def kinds (vec (rest Kind)))

(def bodies
  {:created    [:map [:class cls/ClassName] [:fields {:optional true} [:map-of :keyword :any]]]
   :bound      [:map [:namespace c/Id] [:name c/Name] [:object c/Id]]
   :unbound    [:map [:namespace c/Id] [:name c/Name] [:object c/Id]]
   :field-set  [:map [:name :keyword] [:type c/FieldType] [:value c/Value] [:reader c/Reader]
                [:visibility {:optional true} c/Id] [:mode {:optional true} c/Mode]
                [:previous {:optional true} :any]
                ;; field.clear: the Field leaves the fold; value is empty, history keeps what it held
                [:cleared {:optional true} :boolean]]
   :field-read [:map [:name {:optional true} :keyword] [:names {:optional true} [:vector :keyword]]
                 [:by c/Id] [:via {:optional true} c/Id]]
   ;; object and grant are absent on the one summary event a snapshot read appends: count of Objects at seq
   :resolved   [:map [:name c/Name] [:object {:optional true} c/Id] [:grant {:optional true} c/Id] [:op c/Op]
                [:count {:optional true} :int] [:seq {:optional true} c/Seq]]
   :granted    [:map [:grant c/Id] [:grantee c/Id] [:ops [:set c/Op]] [:scope :any]
                [:delegable {:optional true} :boolean] [:until {:optional true} [:or c/Instant c/Id]]]
   :revoked    [:map [:grant c/Id] [:reason {:optional true} :string]]
   :sent       [:map [:message c/Id] [:route c/Id]]
   :delivered  [:map [:message c/Id] [:route c/Id] [:recipient c/Id]]
   :task-moved [:map [:task c/Id] [:from cls/TaskState] [:to cls/TaskState] [:evidence {:optional true} [:set c/Id]]]
   :evaluated  [:map [:evaluation c/Id] [:state cls/EvaluationState] [:model {:optional true} c/Id]
                [:cost {:optional true} [:map-of :keyword :double]] [:partial {:optional true} :string]
                [:delta {:optional true} :string] [:seq-in-stream {:optional true} :int]
                [:thinking-delta {:optional true} :string] [:thinking-chars {:optional true} :int]
                ;; cancelled (decisions 2026-10-30): who asked and how much content had streamed
                [:by {:optional true} c/Id] [:partial-chars {:optional true} :int]
                ;; steering cut the call and the wake goes on (decisions 2026-10-31)
                [:reason {:optional true} [:enum :steer]]
                [:serialization {:optional true} c/Id] [:outcome {:optional true} :map]]
   ;; a Trigger fired: the Message it sent (when it did), its owner, why; evaluation is the
   ;; spec's original shape, kept optional
   :fired      [:map [:trigger c/Id] [:evaluation {:optional true} c/Id] [:owner {:optional true} c/Id]
                [:message {:optional true} c/Id] [:to {:optional true} c/Id] [:reason {:optional true} :string]]
   ;; the token Object the socket attached with (absent for a legacy principal/token-hash)
   :attached   [:map [:session :string] [:client-kind [:or :string :keyword]] [:format {:optional true} [:enum :transit :json]]
                [:token {:optional true} c/Id]]
   :detached   [:map [:session :string] [:reason {:optional true} :string]]
   ;; off-the-record sessions: who, which Principal, how much; never text, Field names or paths
   ;; panes (docs/panes.md): output is in blobs, each pane-output chained to the one before
   :pane-opened [:map [:pane c/Id] [:kind :keyword] [:target {:optional true} c/Id] [:blob {:optional true} :string]]
   :pane-input [:map [:pane c/Id] [:who c/Id] [:bytes :int] [:from :string] [:text {:optional true} :string]]
   :pane-output [:map [:pane c/Id] [:n :int] [:blob :string] [:size :int] [:chain :string] [:offset {:optional true} :int]]
   :pane-edit [:map [:pane c/Id] [:who c/Id] [:version :int] [:verb :keyword] [:at :int] [:del :int] [:text :string]
               [:sha {:optional true} :string]]
   :pane-closed [:map [:pane c/Id] [:exit {:optional true} :int] [:reason :keyword]]
   :scratch-began [:map [:scratch c/Id] [:principal c/Id] [:forked-at c/Seq]]
   :scratch-ended [:map [:scratch c/Id] [:principal c/Id] [:messages :int] [:tokens-in :int] [:tokens-out :int]
                   [:duration-ms :int] [:reason :string]]
   ;; a retired Object keeps its log; names lists what was unbound with it, so a restore can rebind them
   :retired    [:map [:reason {:optional true} :string] [:names {:optional true} [:vector [:tuple c/Id c/Name]]]]
   :restored   [:map [:names {:optional true} [:vector [:tuple c/Id c/Name]]]]})

(defn event-of
  "Schema for an event of one kind."
  [kind]
  [:map {:title (str "Event/" (name kind))}
   [:event/id c/Id]
   [:event/stream c/Id]
   [:event/streams {:optional true} [:set c/Id]]
   [:event/seq c/Seq]
   [:event/kind [:= kind]]
   [:event/actor c/Id]
   [:event/via {:optional true} c/Id]
   [:event/delegation {:optional true} c/Id]
   ;; an event promoted from a fork keeps the fork's id and its seq there
   [:event/from-fork {:optional true} :string]
   [:event/fork-seq {:optional true} c/Seq]
   [:event/at c/Instant]
   [:event/body (bodies kind)]])

(def Event
  (into [:multi {:title "Event" :dispatch :event/kind}]
        (map (fn [k] [k (event-of k)]) kinds)))

(def PendingEvent
  "An event before the writer assigns seq (and id when absent)."
  (into [:multi {:title "PendingEvent" :dispatch :event/kind}]
        (map (fn [k] [k (-> (event-of k)
                            (update 1 assoc :title (str "Pending/" (name k)))
                            (->> (remove (fn [e] (and (vector? e) (#{:event/seq :event/id} (first e)))))
                                 vec))])
             kinds)))
