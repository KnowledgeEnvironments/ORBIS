(ns knowledge.world.authority
  "Who may decide what, above `resolves?` (docs/authority.md). Resolution answers what a Principal can
   reach; everything here is what reach must not decide, and it holds for root too. One home, a small
   vocabulary, read by every path that writes or runs:

     person?            this Principal is a person
     person-session?    this act comes from a person's own session: never a loop or an approved op
     person-approved?   a person decided this Task by hand, not a rule or unsafe mode
     by-a-person?       either of those: their own session, or their own card
     require-person!    throws person-only unless it is a person's own session
     person-grant?      a Grant a person holds or issued
     must-card?         an op no rule and no unsafe mode can admit
     operator?          root on the key the world wrote for its owner at boot
     pane-share, closed-pane?, shared-terminal-op?  a person's terminal: who an agent is to it
     passes-session-gate?  a socket session's op goes straight on
     write-refusal, clear-refusal  what no write of a Field gets past

   A leaf: it reads the fold and depends on nothing that acts, so the gate, the protocol, the actions
   and the panes can all ask here without a cycle."
  (:require [clojure.string :as str]
            [datascript.core :as d]
            [knowledge.world.fold.core :as fold]
            [knowledge.world.fold.objects :as objects])
  (:import [java.time Duration Instant]
           [java.util Date]))

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- refuse [code message] {:err {:code code :message message}})

;; ---------------------------------------------------------------- people

(defn person?
  "Is this Principal driven by a person? Takes an id or its string."
  [db id]
  (let [id (->uuid id)]
    (boolean (and id (= :person (:principal/driven-by (objects/entity db id)))))))

(defn person-session?
  "Does this act come from a person's own socket session? session? is false for a loop and an approved op."
  [db p session?]
  (boolean (and session? (person? db p))))

(defn person-approved?
  "Did a person decide this Task by hand? A rule or unsafe mode leaves task/rule."
  [db task]
  (boolean (and task (nil? (field db task :task/rule)) (person? db (field db task :task/decided-by)))))

(defn by-a-person?
  "A person's own session, or an op a person approved by hand."
  [db p session? task]
  (or (person-session? db p session?) (person-approved? db task)))

(defn require-person!
  "Throws person-only unless p is a person acting through their own session."
  [db p {:keys [session?]}]
  (when-not (person-session? db p session?)
    (throw (ex-info "only a person, from their own session"
                    (refuse :person-only "only a person, from their own session")))))

(defn person-grant?
  "A Grant a person holds or issued: losing one is a person's to decide (decisions 2026-10-31)."
  [db grant]
  (when-let [{:grant/keys [grantee grantor]} (some->> (->uuid grant) (objects/state db))]
    (or (person? db grantee) (person? db grantor))))

;; ---------------------------------------------------------------- what no rule admits

(defn action-name
  "The named action of an act op (`act {action args}` with no object or class), as a string, or nil."
  [op args]
  (when (and (= :act (keyword (name op))) (nil? (:object args)) (nil? (:class args)) (:action args))
    (name (:action args))))

(defn new-release?
  "world.restart onto a release other than the running one, read from world/release."
  [db op args]
  (when (= "world.restart" (action-name op args))
    (when-let [wanted (some-> (get-in args [:args :release]) str not-empty)]
      (let [running (str (:commit (field db (objects/world-id db) :world/release)))]
        (not (and (seq running) (str/starts-with? running wanted)))))))

(defn must-card?
  "An op no standing rule and no unsafe mode can admit, whatever the rule says: whoever swaps the
   kernel can remove every gate, and a person's standing is not an agent's to take."
  [db op args]
  (boolean (or (new-release? db op args)
               (= "adapter.unconfine" (action-name op args))
               (and (= "revoke" (name op)) (person-grant? db (:grant args))))))

;; ---------------------------------------------------------------- a person's terminal

(def max-minutes "The longest anything may run without a person: unsafe mode, an Adapter unboxed, a share." 480)
(def pane-person "On a terminal a person opened: it runs outside the box and is closed to agents." :pane/person)
(def pane-shares "On such a terminal, kernel-written: [{principal mode until by at}], one per agent." :pane/shared)

(defn in-time?
  "Did {at until} begin, not end yet, and fit max-minutes? A value that does not parse is out of time,
   so a forged or stale one fails towards closed."
  [{:keys [at until]} ^Date now]
  (try (let [at (Instant/parse (str at))
             until (Instant/parse (str until))
             n (.toInstant now)]
         (and (.isBefore n until)
              (not (.isAfter at n))
              (<= (.toMinutes (Duration/between at until)) max-minutes)))
       (catch Exception _ false)))

(defn person-terminal?
  "Is this a terminal a person opened? A direct lookup: resolution asks it on every read."
  [db id]
  (boolean (and id (d/entity db [:field/key (fold/field-key id pane-person)]))))

(defn pane-share
  "The share of pane with p that holds at now, {principal mode until by at}, or nil."
  [db pane p ^Date now]
  (let [v (field db pane pane-shares)]
    (when (sequential? v)
      (some #(when (and (map? %) (= (str p) (str (:principal %))) (person? db (:by %)) (in-time? % now)) %) v))))

(defn closed-pane?
  "Is id a person's terminal that p may not know of? Everything an agent could reach it by asks here."
  ([db id p] (closed-pane? db id p (Date.)))
  ([db id p now]
   (and (person-terminal? db id) (not (person? db p)) (nil? (pane-share db id p now)))))

(defn shared-terminal-op?
  "Typing into a person's terminal, or frobbing it: a card every time unless a person's rule admits it,
   whatever the policy says, and unsafe mode never does."
  [db op args]
  (let [a (action-name op args)]
    (boolean (and (#{"pane.send" "pane.frob"} a)
                  (person-terminal? db (->uuid (get-in args [:args :pane])))))))

;; ---------------------------------------------------------------- a socket session

(defn operator?
  "Is this root on the credential the world wrote for its owner at boot (data/root.token)? That
   key opens the world: it is how a world is set up and how the supervisor checks it, and no
   agent can read it from inside the box. A token root issued itself is not this."
  [db p token-object]
  (boolean (and (= p (objects/root-id db))
                (or (nil? token-object)
                    (= (objects/world-id db) (field db token-object :token/issued-by))))))

(defn passes-session-gate?
  "Does an op from this socket session go on without meeting the gate? A person's own session is a
   person acting, the operator's key opens the world, and off the record stays inside its fork.
   Everyone else meets what a loop meets: a token held by a process is no way round a gate."
  [db p {:keys [scratch? session? token]}]
  (boolean (or scratch? (person-session? db p session?) (operator? db p token))))

;; ---------------------------------------------------------------- Fields

(defn field-key
  "A Field name as the kernel means it, however it was spelled: a keyword, or a string with or
   without the leading colon the JSON form may carry. Every guard compares this."
  [f]
  (let [s (if (keyword? f) (subs (str f) 1) (str f))]
    (keyword (if (str/starts-with? s ":") (subs s 1) s))))

(def unsafe-field "Unsafe mode, on a Principal." :policy/unsafe)
(def unsafe-index "Every armed Principal, on the world." :world/unsafe)
(def confined-field "Whether an Adapter runs in the box." :adapter/confined)

(def kernel-fields
  "Fields that say what a Principal is, not what it knows. No set-field writes them, from any session
   (decisions 2026-10-31): person-only rests on driven-by and attach on token-hash, so write on an
   Object must not reach either, and every Principal holds write on itself. The kernel sets these at
   creation and token.issue rotates credentials. A forged world/release would make a new kernel look
   like a plain restart."
  #{:principal/driven-by :principal/namespace :principal/token-hash :principal/ephemeral :world/root
    :world/release :world/restart :world/notifications :world/manual
    ;; the box: changed by adapter.confine and adapter.unconfine, never by writing the Adapter
    :adapter/locator confined-field :adapter/network :adapter/read
    ;; a person's terminal: write on the pane must not open it, and root holds write on everything
    pane-person pane-shares})

(def person-chosen-fields
  "Which Model an agent runs on spends a person's money or plan, so an agent never chooses for itself."
  #{:principal/model-choice :principal/model-fallback :conversation/model})

(defn- gateway-field? [k] (= "gateway" (namespace k)))
(defn- policy-field? [k] (= "policy" (namespace k)))

(defn write-refusal
  "Why this Field cannot be written, as an err, or nil. person? is a person's own session. policy/*
   decides what needs a person's approval, so only a person changes it: a Principal cannot loosen its
   own gate, root included (decisions 2026-10-30). Kernel bookkeeping calls act directly."
  [field person?]
  (let [k (field-key field)]
    (cond
      (kernel-fields k)
      (refuse :conflict (str field " belongs to the kernel: it is set at creation, never by set-field"))
      ;; a handle, a password's variable and a cursor: what reaches an agent from outside is a person's act
      (gateway-field? k)
      (refuse :conflict (str field " changes only through gateway.create and gateway.bind"))
      (and (policy-field? k) (not person?))
      (refuse :person-only (str field " is written only by a person's own session"))
      (and (person-chosen-fields k) (not person?))
      (refuse :person-only (str field " is chosen from a person's own session"))
      ;; what a person has looked at: an agent marking it read would hide what it did
      (and (= :view/seen k) (not person?))
      (refuse :person-only (str field " is written only by a person's own session"))
      ;; the actions bound these: a time limit, a record of who, an index that stays true
      (#{unsafe-field unsafe-index} k)
      (refuse :conflict (str field " changes only through approval.unsafe and approval.unsafe.off")))))

(defn clear-refusal
  "Why this Field cannot be cleared, as an err, or nil. Clearing is a write, so everything that stops a
   write stops it; what the fold relies on stays too. A person may not clear unsafe mode either: that
   goes by approval.unsafe.off, with its record."
  [field person?]
  (let [k (field-key field)]
    (if (or (fold/structural k) (kernel-fields k) (gateway-field? k) (#{unsafe-field unsafe-index} k))
      (refuse :conflict "the kernel needs that Field")
      (write-refusal field person?))))
