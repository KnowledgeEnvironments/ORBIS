(ns knowledge.schema.protocol
  "Wire protocol frames and ops (spec 14, ADR 0002, 0004, 0008).
   Four frame kinds by key presence: request {id op args}, response {id ok result seq}
   or {id ok=false err seq}, push {sub events as_of}, ack {ack seq}."
  (:require [knowledge.schema.core :as c]
            [knowledge.schema.classes :as cls]
            [knowledge.schema.events :as ev]))

(def ops
  [:attach :detach :resolve :read :history :subscribe :unsubscribe :search :present
   :bind :unbind :set-field :send :move :evaluate :cancel :release :fail :grant :revoke
   :arm :disarm :serialize :act
   ;; two ops a socket may send before attach: hello (which world is this) and
   ;; pair.claim (trade a pairing code for a credential)
   :hello :pair.claim])

(def OpName (into [:enum] ops))

(def ErrCode
  "Wire error codes. The last five arrived with named actions (decisions 2026-10-30): unknown-action,
   invalid-args (the action's schema rejected the args), narrower-required (a Grant that would not be
   strictly narrower than the caller's), outside-mount (a path outside the caller's own mounts),
   invalid-ops (words outside the Grant vocabulary)."
  [:enum :unresolved :gated :budget :residency :conflict :lagging :invalid :not-implemented :unauthenticated
   :unknown-action :invalid-args :narrower-required :outside-mount :invalid-ops :not-allowed :too-large :not-an-image
   ;; approval gates (decisions 2026-10-30): a loop's gated op became a pending Task
   :approval-pending
   ;; approval rules: policy/* and rule actions are a person's own, from their own session
   :person-only
   ;; world.restart: no supervisor to bring the world back, a release older than the log
   :unsupervised :consent-declined :plan-not-granted :sign-in-failed :log-version
   ;; model.create refused a literal credential in its args
   :secret-in-args
   ;; a secret-bearing action a loop may not call; too many wrong pairing codes on one socket
   :session-only :rate-limited
   ;; a render with no model on this host
   :no-renderer :unconfined :no-speech-model :too-large :unreachable
   ;; an off-the-record session refused something that reaches past it
   :scratch-readonly])

(def Err [:map [:code ErrCode] [:message :string] [:data {:optional true} :map]])

(def sprint-0-ops
  "Implemented; everything else answers err not-implemented."
  #{:attach :detach :subscribe :unsubscribe :read :history :set-field :bind :unbind :grant :revoke :act})

(def args
  {:attach      [:map [:principal {:optional true} c/Id] [:token :string] [:session {:optional true} :string]
                 [:subs {:optional true} [:map-of :string c/Seq]] [:client-kind {:optional true} [:or :string :keyword]]] ; a free name (listener, adapter, test, phone, ...), 
   :detach      [:map]
   :resolve     [:map [:name c/Name] [:op c/Op] [:watch {:optional true} :boolean]]
   ;; read by id, or by name (resolution; a mounted path materializes its Object, spec 6.5); range is
   ;; [first last] lines (1-based, inclusive) of a text Object's file/text (decisions 2026-10-30)
   ;; bytes false: payloads of bytes Fields are left out, their size and hash stay in object/blobs
   :read        [:map [:object {:optional true} c/Id] [:name {:optional true} c/Name] [:field {:optional true} :keyword]
                 [:seq {:optional true} c/Seq] [:depth {:optional true} :int] [:range {:optional true} [:tuple :int :int]]
                 [:byte-range {:optional true} [:tuple :int :int]]
                 [:bytes {:optional true} :boolean]]
   ;; history of one stream, or of the whole log without :stream; with :snapshot true the
   ;; world answers the resolvable state of every Object at a seq plus the tail after it, objects paged by :cursor
   ;; limit above the server maximum (1000) is clamped, never refused
   :history     [:map [:stream {:optional true} c/Id] [:after {:optional true} c/Seq] [:limit {:optional true} [:int {:min 1}]]
                 [:snapshot {:optional true} :boolean] [:cursor {:optional true} :string] [:next {:optional true} [:or c/Seq :string]]
                 ;; a fork's own events (seq > the fork seq) with a summary of what changed
                 [:since-fork {:optional true} :boolean]
                 ;; conversations (decisions 2026-10-31): newest first, filtered by resolution
                 [:conversations {:optional true} :boolean] [:principal {:optional true} c/Id]
                 ;; a snapshot leaves retired Objects out unless asked (decisions 2026-10-31)
                 [:retired {:optional true} :boolean]
                 ;; the newest page: `last` events below `before` (default past the head)
                 [:before {:optional true} c/Seq] [:last {:optional true} [:int {:min 1}]]
                 ;; the Messages of a thread (the root Message's id), oldest first
                 [:thread {:optional true} c/Id]]
   :subscribe   [:map [:sub :string] [:stream {:optional true} c/Id] [:streams {:optional true} [:set c/Id]]
                 [:kinds {:optional true} [:set ev/Kind]] [:after {:optional true} c/Seq]
                 [:window {:optional true} [:int {:min 1 :max 4096}]] [:query {:optional true} :any]]
   :unsubscribe [:map [:sub :string]]
   ;; substring and word match over string Fields, Message texts and bound names the caller may read
   :search      [:map [:text :string] [:limit {:optional true} :int] [:kinds {:optional true} [:set [:enum :field :message :object]]]
                 [:since {:optional true} c/Seq]]
   ;; views: a view Serialization (kind, title, data) sent as a Message of kind :view on a Route
   :present     [:map [:to [:set c/Id]] [:route c/Id] [:kind [:enum :text :table :tree :graph :image :tex]] [:title :string]
                 [:data [:or :map c/Blob]] [:about {:optional true} [:vector c/Id]] [:in-reply-to {:optional true} c/Id]
                 [:title-refs {:optional true} [:vector cls/Ref]] [:data-refs {:optional true} [:vector cls/Ref]]
                 [:unresolved-refs {:optional true} [:vector :string]]]
   :bind        [:map [:namespace {:optional true} c/Id] [:name c/Name] [:object c/Id]]
   :unbind      [:map [:namespace {:optional true} c/Id] [:name c/Name]]
   ;; value is what to store; from copies another Field server-side instead (decisions 2026-10-31), so a
   ;; model keeps a whole document without retyping it; then type defaults to the source's
   :set-field   [:map [:object c/Id] [:field :keyword]
                 [:type {:optional true} c/FieldType] [:value {:optional true} c/Value]
                 [:from {:optional true} [:map [:object {:optional true} c/Id] [:name {:optional true} c/Name]
                                          [:field {:optional true} :keyword] [:range {:optional true} [:vector :int]]]]
                 [:reader {:optional true} c/Reader] [:visibility {:optional true} c/Id]
                 ;; spans of a text value, kept as <field>/refs beside it
                 [:refs {:optional true} [:vector cls/Ref]] [:unresolved-refs {:optional true} [:vector :string]]]
   ;; a session may post a view it authored: kind view naming the view Object; the approval
   ;; and trigger kinds are the world's own and never come from the wire
   :send        [:map [:to [:set c/Id]] [:route c/Id] [:text :string] [:tags {:optional true} [:vector c/Id]]
                 ;; references as spans of text, checked against it and against the sender's resolution
                 [:refs {:optional true} [:vector cls/Ref]] [:unresolved-refs {:optional true} [:vector :string]]
                 [:in-reply-to {:optional true} c/Id] [:reasoning {:optional true} [:enum :on :off]]
                 [:kind {:optional true} [:enum :view]] [:view {:optional true} c/Id]
                 ;; steering (decisions 2026-10-31): join the recipient's running wake instead of queueing
                 [:steer {:optional true} :boolean]
                 ;; images already stored, by sha256 or attachment; the world reads their type from the bytes
                 [:images {:optional true} [:vector [:or :string cls/ImageAttachment]]]]
   :move        [:map [:task c/Id] [:to cls/TaskState] [:evidence {:optional true} [:set c/Id]]]
   :evaluate    [:map [:working-set [:set c/Id]] [:model c/Id] [:route {:optional true} c/Id]
                 [:policy {:optional true} cls/Policy] [:depth {:optional true} [:map-of c/Id :int]]]
   :cancel      [:map [:evaluation c/Id]]
   :release     [:map [:evaluation c/Id]]
   :fail        [:map [:evaluation c/Id] [:reason {:optional true} :string]]
   :grant       [:map [:grantee c/Id] [:scope [:or [:set c/Id] cls/ScopeFilter [:= :everything]]]
                 [:ops [:set c/Op]] [:delegable {:optional true} :boolean] [:until {:optional true} c/Instant]]
   :revoke      [:map [:grant c/Id] [:reason {:optional true} :string]]
   ;; arm an existing Trigger by id, or (the spec's form) create and arm one from a predicate
   :arm         [:map [:trigger {:optional true} c/Id] [:as {:optional true} c/Id] [:predicate {:optional true} cls/Predicate]
                 [:evaluation {:optional true} :map] [:once {:optional true} :boolean]]
   :disarm      [:map [:trigger c/Id]]
   :serialize   [:map [:working-set [:set c/Id]] [:model c/Id] [:depth {:optional true} [:map-of c/Id :int]]]
   :act         [:map [:class {:optional true} cls/ClassName] [:object {:optional true} c/Id] [:action :keyword] [:args :map]]
   ;; before attach
   :hello       [:map]
   ;; the arg is pairing-code, not code: code is a JSON-form discriminator (an err's code) and would be read as a keyword
   :pair.claim  [:map [:pairing-code :string] [:label {:optional true} :string]]})

(def results
  {:attach      [:map [:session :string] [:principal c/Id] [:resumed :boolean] [:seq c/Seq]]
   :detach      [:map]
   :resolve     [:maybe [:map [:object c/Id] [:class cls/ClassName]]]
   :read        :map
   ;; next: the seq to page events from, or, in snapshot mode, the object cursor (a string) to pass back as next
   :history     [:map [:events [:vector ev/Event]] [:next {:optional true} [:or c/Seq :string]]
                 ;; a thread's Messages (history {thread}), the asker, how many the caller may read
                 [:thread {:optional true} c/Id] [:asked-by {:optional true} c/Id]
                 [:conversations {:optional true} [:vector :map]]
                 [:count {:optional true} :int] [:messages {:optional true} [:vector :map]]
                 [:snapshot {:optional true} [:map [:seq c/Seq] [:objects [:vector :map]] [:total :int]]]
                 [:fork {:optional true} [:map [:forked-at c/Seq] [:parent :string]]]
                 [:summary {:optional true} :map]
                 ;; the seq to pass as `before` for the page before a newest page
                 [:prev {:optional true} c/Seq]]
   :subscribe   [:map [:sub :string] [:as-of c/Seq]]
   :unsubscribe [:map]
   :search      [:map [:hits [:vector [:map [:object c/Id] [:field {:optional true} :keyword] [:name {:optional true} :string]
                                             [:seq c/Seq] [:snippet :string] [:score :int] [:kind [:enum :field :message :object]]]]]
                 [:total :int]]
   :present     [:map [:serialization c/Id] [:message c/Id] [:seq c/Seq] [:truncated :boolean]]
   :bind        [:map [:seq c/Seq]]
   :unbind      [:map [:seq c/Seq]]
   :set-field   [:map [:seq c/Seq]]
   :send        [:map [:message c/Id] [:seq c/Seq] [:steered {:optional true} :boolean] [:next-wake {:optional true} :boolean]]
   :move        [:map [:seq c/Seq]]
   :evaluate    [:map [:evaluation c/Id]]
   :cancel      [:map [:seq c/Seq]]
   :release     [:map [:seq c/Seq]]
   :fail        [:map [:seq c/Seq]]
   :grant       [:map [:grant c/Id] [:seq c/Seq]]
   :revoke      [:map [:seq c/Seq]]
   :arm         [:map [:trigger c/Id] [:seq c/Seq]]
   :disarm      [:map [:seq c/Seq]]
   :serialize   [:map [:serialization c/Id] [:size :map] [:hash :string]]
   :act         [:map [:result :any] [:object {:optional true} c/Id] [:seq {:optional true} c/Seq]]
   :hello       [:map [:world c/Id] [:name :string] [:url :string] [:format-versions :map] [:head c/Seq]
                 [:root {:optional true} c/Id]]
   :pair.claim  [:map [:token c/Id] [:secret :string] [:principal c/Id] [:world c/Id]]})

(def Request
  (into [:multi {:title "Request" :dispatch :op}]
        (map (fn [op] [op [:map [:id :string] [:op [:= op]] [:args (args op)]]]) ops)))

(def Response
  [:or {:title "Response"}
   [:map [:id :string] [:ok [:= true]] [:result :any] [:seq c/Seq]]
   [:map [:id :string] [:ok [:= false]] [:err Err] [:seq c/Seq]]])

(def Push [:map {:title "Push"} [:sub :string] [:events [:vector ev/Event]] [:as-of c/Seq]
           [:lagging {:optional true} :boolean]
           ;; the cursor this page was read from; seqs in (from, first event) were filtered, not lost
           [:from {:optional true} c/Seq]
           ;; an off-the-record session's events: for the session that opened it, never on the log
           [:scratch {:optional true} :string]])
(def Ack [:map {:title "Ack"} [:ack :string] [:seq c/Seq]])

(def PaneFrame
  "A pane's stream to one subscribed session (docs/panes.md). seq is the pane's own counter. base is
   the seq a delta applies on: a client whose last seq differs has a gap and subscribes again with
   after. full replaces what the client holds. data is escape sequences for a terminal, the buffer
   text for an editor. edit is one change to a buffer, state any other pane's new Fields."
  [:map {:title "PaneFrame"} [:pane c/Id] [:seq :int]
   [:base {:optional true} :int]
   [:full {:optional true} :boolean]
   [:cols {:optional true} :int]
   [:rows {:optional true} :int]
   [:data {:optional true} :string]
   [:edit {:optional true} [:map [:at :int] [:del :int] [:text :string] [:who c/Id] [:version :int]]]
   [:state {:optional true} :map]
   [:exited {:optional true} :boolean]
   [:exit {:optional true} :int]])

(defn frame-kind [m]
  (cond (contains? m :ack) :ack
        (contains? m :sub) :push
        (contains? m :pane) :pane
        (contains? m :op) :request
        (contains? m :ok) :response
        :else ::unknown))

(def Frame
  [:multi {:title "Frame" :dispatch frame-kind}
   [:request Request] [:response Response] [:push Push] [:ack Ack] [:pane PaneFrame]])
