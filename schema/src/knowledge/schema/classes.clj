(ns knowledge.schema.classes
  "The fourteen classes (spec 3, 4, 5). Five kernel classes, nine arrangements.
   Arrangements are Objects (or Principals) with a fixed field set; here they are
   expressed as maps merged onto the base class."
  (:require [knowledge.schema.core :as c]))

(defn extends
  "Data-level inheritance: the child's props, then the base's entries, then the child's.
   Stays plain data so refs resolve against the registry at m/schema time."
  [base child]
  (into [:map (second child)] (concat (drop 2 base) (drop 2 child))))

(def ClassName
  [:enum :object :principal :field :event :serialization
   :group :message :task :evaluation :trigger :model :route :grant :adapter])

(def kernel-classes #{:object :principal :field :event :serialization})
(def arrangements #{:group :message :task :evaluation :trigger :model :route :grant :adapter})

;; 4.3 Field
(def Field
  [:map {:title "Field"}
   [:field/name :keyword]
   [:field/type c/FieldType]
   [:field/reader c/Reader]
   [:field/value c/Value]
   [:field/author c/Id]
   [:field/set-by {:optional true} c/Id]
   [:field/set-at c/Instant]
   [:field/visibility {:optional true} c/Id]
   [:field/mode {:optional true} c/Mode]])

;; 4.1 Object
(def ObjectSchema
  [:map {:title "Object"}
   [:object/id c/Id]
   [:object/class ClassName]
   [:object/names [:set [:tuple c/Id c/Name]]]
   [:object/stream c/Id]
   [:object/fields [:map-of :keyword Field]]
   [:object/created {:optional true} c/Id]
   [:object/backing {:optional true} c/Id]])

;; 4.2 Principal
(def Principal
  (extends ObjectSchema
    [:map {:title "Principal"}
     [:principal/driven-by c/DrivenBy]
     [:principal/namespace c/Id]
     [:principal/grants [:set c/Id]]
     [:principal/workspaces {:optional true} [:set c/Id]]
     [:principal/models {:optional true} [:set c/Id]]
     [:principal/routes {:optional true} [:set c/Id]]
     [:principal/ephemeral {:optional true} :map]
     [:principal/budget {:optional true} c/Id]
     [:principal/public-key {:optional true} :string]
     ;; a person let this Principal's gated ops run without a card, until `until` (decisions 2026-10-31)
     [:policy/unsafe {:optional true} [:map [:on :boolean] [:until :string] [:except [:vector :string]]
                                       [:by :string] [:at :string]]]]))

;; 8.6 a reference: a span of a text Field, kept in <field>/refs beside it; offsets are Unicode code points
(def Ref
  [:map {:title "Ref"}
   [:start :int] [:end :int]
   [:kind [:enum :object :field :seq :path]]
   [:object {:optional true} c/Id]
   [:field {:optional true} :string]
   [:seq {:optional true} c/Seq]
   [:path {:optional true} :string]
   [:line {:optional true} :int]
   [:name {:optional true} :string]])

;; 4.5 Serialization
(def Serialization
  (extends ObjectSchema
    [:map {:title "Serialization"}
     [:serialization/working-set [:or [:set c/Id] [:map [:query :any]]]]
     [:serialization/model c/Id]
     [:serialization/depth [:map-of c/Id :int]]
     [:serialization/format :keyword]
     [:serialization/bytes [:or c/Blob :any]]
     [:serialization/size [:map [:bytes :int] [:tokens {:optional true} :int]]]
     [:serialization/hash :string]
     [:serialization/as-of [:map-of c/Id c/Seq]]
     ;; 8.6 the handle of every id this prompt renders, inline or a blob when large
     [:serialization/ids {:optional true} [:or c/Blob [:map-of :string :string]]]
     [:serialization/layout {:optional true} :map]
     [:serialization/live {:optional true} :boolean]
     ;; views (plan W11): a Serialization whose model is a listener and whose audience is people
     [:serialization/kind {:optional true} [:enum :view]]
     [:view/kind {:optional true} [:enum :text :table :tree :graph :image :tex :pane]]
     [:view/title {:optional true} :string]
     [:view/data {:optional true} [:or :map c/Blob]]
     [:view/about {:optional true} [:vector c/Id]]
     [:view/truncated {:optional true} :boolean]]))

;; 5.1 Group
(def Group
  (extends Principal
    [:map {:title "Group"}
     [:group/members [:set c/Id]]
     [:group/policy [:set c/Id]]
     [:group/kind [:enum :team :channel :project :federation :custom]]]))

;; 5.2 Message
(def ImageAttachment
  "An image a Message carries: the bytes are in the blob store under sha256."
  [:map {:title "ImageAttachment"}
   [:sha256 :string] [:size :int] [:mime :string] [:name :string]])

(def Message
  (extends ObjectSchema
    [:map {:title "Message"}
     [:message/from c/Id]
     [:message/to [:set c/Id]]
     [:message/route c/Id]
     [:message/text :string]
     [:message/tags [:vector c/Id]]
     [:message/refs {:optional true} [:vector Ref]]
     ;; images by sha256 in the blob store, typed from the bytes themselves
     [:message/images {:optional true} [:vector ImageAttachment]]
     [:message/unresolved-refs {:optional true} [:vector :string]]
     [:message/in-reply-to {:optional true} c/Id]
     [:message/reasoning {:optional true} [:enum :on :off]]
     ;; approval gates (decisions 2026-10-30): a card asking for a decision, or the decision itself, about a Task
     [:message/kind {:optional true} [:enum :approval :approval-result :trigger :view :status :request :directive :steer :gateway]]
     [:message/gateway {:optional true} :map]
     ;; a person's /word, handled by the kernel (decisions 2026-10-31); /resume names the thread it reopens
     [:message/directive {:optional true} :string]
     [:message/resumes {:optional true} c/Id]
     ;; a provisioning request from a Principal to its creator carries the ops it could not perform
     [:message/lacking {:optional true} [:vector :map]]
     ;; the Trigger whose firing sent this Message
     [:message/trigger {:optional true} c/Id]
     [:message/task {:optional true} c/Id]
     ;; an approval card carries its Task's expiry, so a client shows time left before it read the Task
     [:message/expires-at {:optional true} c/Instant]
     ;; views: the view Serialization a Message of kind :view carries
     [:message/view {:optional true} c/Id]
     ;; the conversation a Message belongs to (the root Message of the chain; a first
     ;; Message is its own thread) and who started it (the thread root's sender), so a loop that asked
     ;; others on behalf of a question can tell whom to answer when their replies come back
     [:message/thread {:optional true} c/Id]
     [:message/asked-by {:optional true} c/Id]
     ;; which client sent it: a session's Message names the token label (message/device) and the
     ;; session; a loop's Message carries neither (decisions 2026-10-31)
     [:message/device {:optional true} :string]
     [:message/session {:optional true} :string]
     [:message/seq c/Seq]]))

;; 5.3 Task
(def TaskState
  "5.3, plus the approval states (decisions 2026-10-30 'approval gates'): pending, approved, denied, expired, failed."
  [:enum :open :active :blocked :done :cancelled :pending :approved :denied :expired :failed])
(def Task
  (extends ObjectSchema
    [:map {:title "Task"}
     [:task/owner c/Id]
     [:task/state TaskState]
     [:task/blocked-on [:set c/Id]]
     [:task/due {:optional true} c/Instant]
     [:task/evidence [:set c/Id]]
     ;; an approval Task (decisions 2026-10-30 'approval gates'): the op held back, who asked, who decides
     [:task/kind {:optional true} :string]
     [:task/op {:optional true} :map]
     [:task/requested-by {:optional true} c/Id]
     [:task/evaluation {:optional true} c/Id]
     [:task/message {:optional true} c/Id]
     [:task/approvers {:optional true} [:vector c/Id]]
     [:task/expires-at {:optional true} c/Instant]
     [:task/summary {:optional true} :string]
     [:task/decided-by {:optional true} c/Id]
     [:task/decided-at {:optional true} c/Instant]
     [:task/note {:optional true} :string]
     [:task/reason {:optional true} :string]
     [:task/result {:optional true} :map]
     [:task/progress {:optional true} :string]
     [:task/waiting {:optional true} c/Id]
     [:task/overdue-told {:optional true} :boolean]
     ;; the standing rule that admitted it, or "unsafe", when no person decided it at the time
     [:task/rule {:optional true} :string]]))

;; 5.4 Evaluation
(def EvaluationState [:enum :queued :running :streaming :done :failed :cancelled :gated])
(def Policy [:enum :normal :one-shot])
(def Evaluation
  (extends ObjectSchema
    [:map {:title "Evaluation"}
     [:evaluation/initiator c/Id]
     [:evaluation/model c/Id]
     [:evaluation/delegation c/Id]
     [:evaluation/serialization c/Id]
     [:evaluation/policy Policy]
     [:evaluation/outcome {:optional true} :map]
     [:evaluation/produced [:set c/Id]]
     [:evaluation/cost [:map-of :keyword [:or :double :boolean]]]
     [:evaluation/state EvaluationState]
     [:evaluation/message {:optional true} c/Id]
     [:evaluation/wake {:optional true} c/Id]
     [:evaluation/reasoning {:optional true} [:enum :on :off]]
     [:evaluation/retry-of {:optional true} c/Id]]))

;; 5.5 Trigger
(def Predicate
  [:multi {:dispatch :kind}
   [:schedule [:map [:kind [:= :schedule]] [:cron {:optional true} :string] [:every-ms {:optional true} :int]]]
   [:completion [:map [:kind [:= :completion]] [:evaluation c/Id]]]
   [:state [:map [:kind [:= :state]] [:object c/Id] [:field :keyword] [:test :any]]]
   [:threshold [:map [:kind [:= :threshold]] [:meter :keyword] [:over c/Id] [:bound :double]]]])
(def TriggerKind
  "(plan §19.1 W8): interval fires every trigger/every-s; at fires once at trigger/at;
   on-change fires on a field-set of trigger/watch {object field?}; on-message fires on a delivery
   to trigger/watch {object}."
  [:enum :interval :at :on-change :on-message])
(def Trigger
  (extends ObjectSchema
    [:map {:title "Trigger"}
     [:trigger/owner c/Id]
     [:trigger/as {:optional true} c/Id]
     [:trigger/delegation {:optional true} c/Id]
     [:trigger/predicate {:optional true} Predicate]
     [:trigger/evaluation {:optional true} :map]
     [:trigger/once {:optional true} :boolean]
     [:trigger/state [:enum :armed :fired :disarmed]]
     ;; what fires, when, and what firing does (a Message from the owner, via the Trigger)
     [:trigger/kind TriggerKind]
     [:trigger/every-s {:optional true} number?]
     [:trigger/at {:optional true} c/Instant]
     [:trigger/watch {:optional true} [:map [:object c/Id] [:field {:optional true} :keyword]]]
     [:trigger/route {:optional true} c/Id]
     [:trigger/to {:optional true} c/Id]
     [:trigger/text {:optional true} :string]
     [:trigger/armed :boolean]
     [:trigger/armed-at {:optional true} c/Instant]
     [:trigger/last-tick {:optional true} c/Instant]
     [:trigger/last-fired {:optional true} c/Instant]
     [:trigger/fired-count {:optional true} :int]
     [:trigger/skipped {:optional true} :int]
     [:trigger/max-fires {:optional true} :int]
     [:trigger/unless-empty {:optional true} :boolean]
     [:trigger/unless-told {:optional true} :boolean]
     [:trigger/last-error {:optional true} :string]]))

;; 5.6 Model
(def ModelKind [:enum :model :compiler :runner :shell :emulator :storage :listener :validator :custom])
(def Model
  (extends Principal
    [:map {:title "Model"}
     [:model/kind ModelKind]
     [:model/format :keyword]
     [:model/residency c/Residency]
     [:model/capacity [:map-of :keyword :any]]
     [:model/pricing [:map-of :keyword :double]]
     [:model/adapter {:optional true} c/Id]
     [:model/reasoning {:optional true} [:enum :off :on :auto]]
     [:model/reasoning-kwargs {:optional true} [:map-of :keyword :any]]
     ;; the most reasoning characters one Evaluation may stream before it fails fast
     [:model/reasoning-max-chars {:optional true} :int]
     ;; the catalogue
     [:model/base-url {:optional true} :string]
     [:model/id {:optional true} :string]
     [:model/key-env {:optional true} :string]
     [:model/stt {:optional true} :string]
     [:model/tts {:optional true} :string]
     [:model/voice {:optional true} :string]
     [:model/label {:optional true} :string]
     [:model/price-in {:optional true} number?]
     [:model/price-out {:optional true} number?]
     [:model/context-tokens {:optional true} :int]
     ;; does it take images? A person says so; nothing on the wire can be trusted to
     [:model/images {:optional true} :boolean]
     [:model/status {:optional true} [:enum :ok :unreachable :unauthenticated :limit :unknown]]
     [:model/available {:optional true} [:vector :string]]
     [:model/api {:optional true} [:enum :chatgpt-plan]]
     [:model/catalogue {:optional true} [:vector [:map-of :any :any]]]
     [:model/probed-at {:optional true} c/Instant]]))

;; 5.7 Route
(def Route
  (extends ObjectSchema
    [:map {:title "Route"}
     [:route/transport c/Id]
     [:route/address :string]
     [:route/input-tags [:vector c/Id]]
     [:route/output-format {:optional true} c/Id]
     [:route/hard-rules [:vector c/Id]]
     [:route/members {:optional true} [:set c/Id]]]))

;; 5.9 Grant
(def ScopeFilter
  "A filtered Grant scope: every Object meeting every constraint named, evaluated at resolution."
  [:map [:prefix {:optional true} c/Name] [:class {:optional true} :keyword] [:field-prefix {:optional true} :string]])

(def Grant
  (extends ObjectSchema
    [:map {:title "Grant"}
     [:grant/grantor c/Id]
     [:grant/grantee c/Id]
     [:grant/scope [:or [:set c/Id] ScopeFilter [:= :everything]]]
     [:grant/ops [:set c/Op]]
     [:grant/delegable :boolean]
     [:grant/until {:optional true} [:or c/Instant c/Id]]]))

;; 5.10 Adapter
(def Adapter
  (extends Model
    [:map {:title "Adapter"}
     [:adapter/binds [:enum :api :cli :filesystem :container :task-system :database :transport :model :process :host :gateway]]
     [:adapter/locator :string]
     [:adapter/test {:optional true} :string]
     [:gateway/kind {:optional true} :string]
     [:gateway/key-env {:optional true} :string]
     [:gateway/poll-s {:optional true} :int]
     [:gateway/cursor {:optional true} :map]
     [:gateway/bindings {:optional true} [:vector :map]]
     [:gateway/dropped {:optional true} :int]
     [:gateway/enabled {:optional true} :boolean]
     [:gateway/status {:optional true} :keyword]
     [:adapter/objects [:set c/Id]]
     [:adapter/sync [:enum :pull :push :both]]
     [:adapter/last-seen [:map-of c/Id c/Instant]]]))

(def by-class
  {:object ObjectSchema :principal Principal :field Field :serialization Serialization
   :group Group :message Message :task Task :evaluation Evaluation :trigger Trigger
   :model Model :route Route :grant Grant :adapter Adapter})
