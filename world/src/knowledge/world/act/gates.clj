(ns knowledge.world.act.gates
  "Approval gates (plan §19.1 W4, decisions 2026-10-30). A loop's op that its Principal's
   policy/approval names is not executed: it becomes a Task of kind approval, pending, addressed to
   the people who may decide it, delivered to them as a Message on a Route they share with the
   requester, and the wake ends there with a status from the kernel; the decision wakes it with the result.
   task.approve executes the stored op as the requesting Principal, under that Principal's own
   Grants, with the Task as via, so the audit chain is one walk: Message, Evaluation, Task, the
   person's decision, the op, its result. task.deny ends it with a reason. Both deliver a Message
   of kind approval-result to the requester, which wakes its loop (knowledge.world.loop.core).
   A session's own ops are never gated: a person acting is a person acting. Expiry moves a Task
   nobody decided to expired after policy/approval-ttl-s and tells the requester.

   Everything is the existing primitives: a Task, Fields, Grants, Messages, task-moved events;
   nothing here recognizes a command (ground rule 1); every write goes through resolution or is
   the kernel's bookkeeping under the Grant that admitted the request (ground rule 4)."
  (:require [clojure.string :as str]
            [knowledge.world.authority :as authority]
            [clojure.walk :as walk]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates-defaults :as defaults]
            [knowledge.world.act.rules :as rules]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]
            [knowledge.world.thread :as thread]
            [taoensso.timbre :as timbre])
  (:import [java.util Date]))


(def default-policy defaults/default-policy)
(def default-ttl-s defaults/default-ttl-s)
(def defaults
  "The policy Fields every process Principal is born with (kernel-zero step approval-defaults; act create)."
  defaults/defaults)
(def types defaults/types)
(def missing defaults/missing)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn root-of [db] (objects/root-id db))

(defn- name-of
  "A bound name for people to read, else the short id."
  [db id]
  (let [wid (objects/world-id db)
        names (objects/names db id)]
    (or (some (fn [[ns n]] (when (= ns wid) n)) (sort-by second names))
        (some (fn [[_ n]] n) (sort-by second names))
        (subs (str id) 0 8))))

(defn policy
  "The gate words of a Principal: policy/approval, or the default when the Field is absent."
  [db p]
  (let [v (field db p :policy/approval)]
    (set (map str (if (sequential? v) v default-policy)))))

(defn ttl-s [db p]
  (let [v (field db p :policy/approval-ttl-s)]
    (if (number? v) (long v) default-ttl-s)))

;; ---------------------------------------------------------------- what is gated

(defn- action-gated?
  "action/gated true on the action Object forces the gate regardless of policy."
  [db n]
  (when n
    (when-let [id (objects/lookup db (objects/world-id db) (str "action/" n))]
      (true? (field db id :action/gated)))))

(defn- shell-run?
  "`act run` on a host Adapter."
  [db op args]
  (and (= :act (keyword (name op))) (= :run (some-> (:action args) name keyword))
       (= :host (field db (->uuid (:object args)) :adapter/binds))))

(defn- fs-write?
  "A set-field on an Object a filesystem mount backs."
  [db op args]
  (and (= :set-field (keyword (name op)))
       (when-let [backing (:object/backing (objects/entity db (->uuid (:object args))))]
         (= :filesystem (field db backing :adapter/binds)))))

(defn pane-form
  "A model writes `act {object <pane>, action pane.send}` as often as the named form (seen live).
   This is the named form, so the gate and the rest of the loop see one shape."
  [op {:keys [object action] :as args}]
  (if (and (= :act op) object (map? args) (some-> action name (str/starts-with? "pane.")))
    (let [slot (if (= "pane.open" (name action)) :target :pane)]
      (-> args (dissoc :object) (update :args #(assoc (or % {}) slot (str object)))))
    args))

(def ^:private outward-frobs
  "Pane verbs that change something outside the world."
  #{"save" "mkdir" "rename" "copy" "move" "delete"})

(defn- outward-frob?
  "`pane.frob` with a verb that writes to the disk."
  [a args]
  (and (= "pane.frob" a) (contains? outward-frobs (some-> (get-in args [:args :verb]) name))))

(defn- action-handler
  "The handler an action name reaches, as a string: an Object bound as action/<anything> with
   action/handler task.run is task.run, so a new name does not walk around the gate."
  [db n]
  (when n
    (when-let [id (objects/lookup db (objects/world-id db) (str "action/" n))]
      (some-> (field db id :action/handler) name))))

(defn- file-words
  "The file names an op reaches: an edit's name, a command's words that look like files."
  [op args]
  (let [inner (:args args)
        cmd (or (:cmd inner) (get inner "cmd"))
        n (or (:name inner) (get inner "name"))]
    (->> (concat (when (string? n) [n])
                 (when (string? cmd) (remove #(str/starts-with? % "-") (str/split cmd #"[\s\"']+"))))
         (keep #(last (str/split (str %) #"/")))
         (filter #(re-find #"[^.]\.[A-Za-z0-9]{1,8}$" %))
         distinct)))

(defn denied-before
  "The summary of a card a person denied p in the last hour that reached the same file as this op, or
   nil. A denial covers the end, not the command: seen live, `rm notes.txt` denied, then the file
   emptied with an edit, on a card that said nothing of the first."
  [db p op args]
  (when-let [files (seq (file-words op args))]
    (let [since (- (System/currentTimeMillis) 3600000)]
      (first (for [t (objects/objects-with-field db :task/reason)
                   :let [at (field db t :task/decided-at)
                         sum (str (field db t :task/summary))]
                   :when (and (= p (field db t :task/requested-by))
                              (= :denied (field db t :task/state))
                              (inst? at) (> (.getTime ^Date at) since)
                              (some #(str/includes? sum %) files))]
               sum)))))

(declare storable)

(defn denied-twin
  "A card a person denied p in the last hour for this very op, or nil. Asking again is not a new
   question: seen live, one denied `sed` came back 30 times, each `after you denied` the last."
  [db p op args]
  (let [wanted {:op (name op) :args (storable args)}
        since (- (System/currentTimeMillis) 3600000)]
    (some (fn [t] (let [at (field db t :task/decided-at)]
                    (when (and (= p (field db t :task/requested-by))
                               (= :denied (field db t :task/state))
                               (inst? at) (> (.getTime ^Date at) since)
                               (= wanted (field db t :task/op)))
                      t)))
          (objects/objects-with-field db :task/reason))))

(defn gated?
  "Does policy gate this op for p? Returns the word that gates it, or nil."
  [db p op args]
  (let [pol (policy db p)
        o (name op)
        a (authority/action-name op args)
        h (action-handler db a)]
    (cond
      (and (= "revoke" o) (authority/person-grant? db (:grant args))) "revoke:person"
      ;; what a person refused stays theirs to decide, however it is reached
      (denied-before db p op args) "after-denial"
      ;; a person's terminal, whatever the agent's own policy says: it is an unconfined shell
      (authority/shared-terminal-op? db op args) "shell"
      (contains? pol o) o
      (and a (contains? pol a)) a
      (and h (contains? pol h)) h
      (action-gated? db a) a
      (and (contains? pol "shell") (shell-run? db op args)) "shell"
      (and (contains? pol "fs:write") (fs-write? db op args)) "fs:write"
      ;; a model at a terminal is a model at a shell, and a pane writes a mount as set-field does
      (and (contains? pol "shell") (= "pane.send" a)) "shell"
      (and (or (contains? pol "shell") (contains? pol "fs:write")) (outward-frob? a args)) "fs:write"
      ;; the same reach as a shell's sed, so the same gate: an edit op must not be a way round it
      (and (or (contains? pol "shell") (contains? pol "fs:write")) (= "file.edit" h)) "fs:write"
      :else nil)))

;; ---------------------------------------------------------------- what lets a gated op through

(defn- op-words
  "Every name an except list can catch this op by."
  [db word op args]
  (let [a (authority/action-name op args)]
    (cond-> (set (keep identity [word (name op) a (action-handler db a)]))
      (:field args) (conj (subs (str (keyword (:field args))) 1)))))

(defn- never-unsafe?
  "policy/never-unsafe true on the action, or on the handler it reaches: a person's mark, which no
   except list drops."
  [db op args]
  (let [a (authority/action-name op args)]
    (boolean (some (fn [n] (when-let [id (and n (objects/lookup db (objects/world-id db) (str "action/" n)))]
                             (true? (field db id :policy/never-unsafe))))
                   [a (action-handler db a)]))))

(defn- own-work?
  "Unsafe mode covers a Principal's own work and what a person asked of it. Whoever woke it, and
   whoever started the thread, must be itself, a person or another armed Principal: anyone else keeps
   its gates, so a gated Principal cannot get an op past its gate by asking an unsafe one. No wake
   Message, nothing to show whose work it is, so a card."
  [db p message now]
  (and message
       (every? (fn [who] (or (nil? who) (= who p) (authority/person? db who) (unsafe/live? db who now)))
               [(field db message :message/from) (thread/asked-by db message)])))

(defn- unsafe-admits
  "The rule-shaped admission when p's own unsafe mode covers this op, or nil. Only p's Field counts:
   the mode does not pass to whoever p talks to."
  [db p word op args message]
  (let [now (Date.)
        s (unsafe/state db p)]
    (when (and (unsafe/live? db p now)
               (not (unsafe/excepted? s (op-words db word op args)))
               (not (never-unsafe? db op args))
               (not (authority/shared-terminal-op? db op args))
               (own-work? db p message now))
      {:id unsafe/rule-id :by (:by s) :unsafe true})))

(defn admission
  "What lets this op of p run without a card: a standing rule, then p's unsafe mode, else nil (the op
   is ungated, or waits for a person)."
  [db p op args message]
  (when-let [word (gated? db p op args)]
    (when-not (or (authority/must-card? db op args) (= "after-denial" word))
      (or (rules/admit db p word op args)
          (unsafe-admits db p word op args message)))))

;; ---------------------------------------------------------------- summary, approvers, route

(defn- clip [s n] (let [s (str s)] (if (> (count s) n) (str (subs s 0 n) "…") s)))

(defn summary
  "One line a person can decide on."
  [db op args]
  (let [a (authority/action-name op args)
        inner (:args args)]
    (cond
      (= a "task.run") (str "run `" (:cmd inner) "` through "
                               (or (some->> (->uuid (:adapter inner)) (name-of db)) (:adapter inner))
                               (when (:cwd inner) (str " in " (:cwd inner))))
      (= a "world.restart") (let [running (:commit (field db (objects/world-id db) :world/release))]
                              (str (if (authority/new-release? db op args)
                                     (str "replace the running kernel: " (clip running 12) " with release " (clip (:release inner) 12))
                                     (str "restart the world on " (clip running 12)))
                                   (when (:reason inner) (str ": " (:reason inner)))))
      (and (= "revoke" (name op)) (authority/person-grant? db (:grant args)))
      (let [{:grant/keys [grantee]} (objects/state db (->uuid (:grant args)))]
        (str "revoke a Grant of " (name-of db grantee) " on a person's standing"))
      (= a "principal.create") (let [g (:grant inner)]
                                 (str "principal.create " (:name inner)
                                      (when (:subtree inner) (str " (developer of " (:subtree inner) ", mounted as " (:name inner) ")"))
                                      (if (map? g)
                                        (str " with Grant scope " (pr-str (:scope g)) " ops " (pr-str (:ops g)))
                                        " (no grant given: it will be refused)")))
      (= a "pane.send") (str "type `" (clip (or (:text inner) (:keys inner)) 200) "` into terminal "
                             (name-of db (->uuid (:pane inner))))
      (= a "pane.frob") (str (:verb inner) " " (clip (j/write-value-as-string (:args inner)) 160)
                             " through pane " (name-of db (->uuid (:pane inner))))
      (= a "workspace.mount") (str "workspace.mount " (:path inner) " as " (:name inner) " for " (:principal inner))
      a (str a " " (clip (j/write-value-as-string inner) 160))
      (= :grant (keyword (name op))) (str "grant " (name-of db (->uuid (:grantee args))) " "
                                          (pr-str (:scope args)) " " (str/join "," (map name (:ops args))))
      (= :act (keyword (name op))) (str "act " (some-> (:action args) name) " on " (name-of db (->uuid (:object args)))
                                        " " (clip (j/write-value-as-string (:args args)) 160))
      :else (str (name op) " " (clip (j/write-value-as-string args) 160)))))

(defn first-person
  "The oldest person-driven Principal in the world, or nil."
  [db]
  (->> (objects/objects-of-class db :principal)
       (filter #(authority/person? db %))
       (sort-by #(or (:object/created-seq (objects/entity db %)) 0))
       first))

(defn approvers
  "Who decides: the wake Message's sender when a person; else the requester's creator, walking up
   creators until a person; else the oldest person in the world. Always person-driven Principals."
  [db p message]
  (let [sender (when message (field db message :message/from))]
    (if (and sender (authority/person? db sender))
      [sender]
      (loop [cur p seen #{}]
        (let [creator (:object/created-by (objects/entity db cur))]
          (cond
            (and creator (authority/person? db creator)) [creator]
            (and creator (not (seen creator)) (not= creator (objects/world-id db))) (recur creator (conj seen creator))
            :else (if-let [first-p (first-person db)] [first-p] [])))))))

(defn- shared-route
  "A Route whose members include both, the wake Route first."
  [db a b wake-route]
  (let [has? (fn [r] (let [ms (set (field db r :route/members))] (and (ms a) (ms b))))]
    (cond
      (and wake-route (has? wake-route)) wake-route
      :else (->> (objects/objects-of-class db :route) (filter has?) (sort-by str) first))))

(defn- route-between!
  "The Route a requester and an approver share, made by root when there is none."
  [world db p approver wake-route]
  (or (shared-route db p approver wake-route)
      (let [root (root-of db)
            handle! (requiring-resolve 'knowledge.world.act.actions/handle!)
            {:keys [ok err]} (handle! :route.create world root {:name (str (name-of db p) "." (name-of db approver)) :members [(str p) (str approver)]} {})]
        (if err
          (do (timbre/warn "approval route could not be made" err) nil)
          (get-in ok [:result :route])))))

;; ---------------------------------------------------------------- the gate

(defn- fail [code message] (act/err code message))

(defn- storable
  "An op's args as task/op holds them: a map Field takes no sets or uuids, and the op is decoded again
   from JSON when it runs, so vectors and strings lose nothing. Found live: a loop's `grant`, with its
   set of ops and scope ids, killed the wake instead of becoming a card."
  [v]
  (walk/postwalk (fn [x] (cond (set? x) (vec (sort-by str x)) (uuid? x) (str x) :else x)) v))

(defn pending-twin
  "A pending approval Task the requester already has for the same op and args: the model retried
   within a wake (seen live: two cards for one request); the first Task is the answer, no new card."
  [db p op args]
  (let [wanted {:op (name op) :args (storable args)}]
    (first (for [t (sort-by #(or (:object/created-seq (objects/entity db %)) 0) (objects/objects-of-class db :task))
                 :when (and (= "approval" (field db t :task/kind))
                            (= :pending (or (field db t :task/state) :open))
                            (= p (field db t :task/requested-by))
                            (= wanted (field db t :task/op)))]
             t))))

(defn gate!
  "Turn a gated op into a pending approval Task. Returns the err the model sees:
   {:code :approval-pending :message summary :task id :summary summary}, or another err when
   no Task could be made (no approver, no :create on the Task schema)."
  [world p {:keys [op args via message route]}]
  (let [db (act/db world)
        sum (summary db op args)
        who (approvers db p message)
        root (root-of db)]
    (cond
      (empty? who)
      (fail :gated (str "awaits approval but no person can approve it: " sum))

      :else
      (if-let [no (denied-twin db p op args)]
        {:code :denied :task no
         :message (str "a person denied exactly this (Task " no "). Do not ask again: reach the same thing another way, or say what it stops.")}
      (if-let [twin (pending-twin db p op args)]
        {:code :approval-pending :message (str "still awaits approval (Task " twin "), asked earlier in this wake; do not retry: " sum)
         :task twin :summary sum}
      (let [sum (str (when-let [d (denied-before db p op args)] (str "after you denied " d ": "))
                     sum
                     ;; a card says who it is really for: an agent asked, not a person
                     (let [from (when message (field db message :message/from))]
                       (when (and from (not= from p) (= :process (:principal/driven-by (objects/entity db from))))
                         (str " (asked by " (name-of db from) ")"))))
            {:keys [ok err]} (act/create! world p :task {:title sum})]
        (if err
          (fail :gated (str "awaits approval but you may not create a Task to carry it (" (:message err) "): " sum))
          (let [task (:object ok)
                grant (:object/created-under (objects/entity (act/db world) task))
                put! (fn [k type v] (act/append! world (log/pending :field-set task p {:name k :type type :value v :reader :both}
                                                                    :delegation grant :via via)))
                expires (Date. (+ (System/currentTimeMillis) (* 1000 (ttl-s db p))))]
            (put! :task/kind :string "approval")
            (put! :task/op :map {:op (name op) :args (storable args)})
            (put! :task/requested-by [:ref :principal] p)
            (when via (put! :task/evaluation [:ref :evaluation] via))
            (when message (put! :task/message [:ref :message] message))
            (put! :task/approvers [:list [:ref :principal]] (vec who))
            (put! :task/expires-at :instant expires)
            (put! :task/summary :string sum)
            (act/append! world (log/pending :task-moved task p {:task task :from :open :to :pending} :streams #{p} :delegation grant :via via))
            ;; the approvers may decide it: evaluate and write on the Task, evaluate on the two actions (root grants)
            (let [actions (keep #(objects/lookup db (objects/world-id db) (str "action/" %)) ["task.approve" "task.deny" "task.decide-many"])]
              (doseq [a who]
                (act/grant! world root {:grantee a :scope #{task} :ops #{:read :evaluate :write} :delegable false})
                (when (seq actions)
                  (act/grant! world root {:grantee a :scope (set actions) :ops #{:read :evaluate} :delegable false}))))
            ;; the card: a Message from the requester to each approver, on a Route they share
            (doseq [a who]
              (when-let [r (route-between! world (act/db world) p a route)]
                (let [{:keys [err]} (route/send! world p {:to #{a} :route r :text (str "approval needed: " sum)
                                                          :in-reply-to message :via via :hop 1
                                                          :kind :approval :task task :expires-at expires})]
                  (when err (timbre/warn "approval card not delivered" {:approver a :err err})))))
            {:code :approval-pending :message (str "awaits approval (Task " task "): " sum) :task task :summary sum}))))))))

;; ---------------------------------------------------------------- decisions

(defn- task-state [db task] (or (field db task :task/state) :open))

(defn- held-note
  "What stood behind this card in the same turn. It ran nothing, and the wake that wrote it is over, so
   only this says it is still to do: asked to set up two projects, a model wrote both, the first
   took a card, and the second was forgotten."
  [db task]
  (when-let [held (seq (field db task :task/held))]
    (str "\nNot run yet, from that same turn; do it now if it is still wanted: " (str/join "; " held))))

(defn- result-message!
  "The decision delivered to the requester on a Route they share, via the Task, kind approval-result."
  [world decider task requester wake-message wake-route text]
  (let [db (act/db world)]
    (when-let [r (route-between! world db requester decider wake-route)]
      (let [{:keys [err]} (route/send! world decider {:to #{requester} :route r :text text :in-reply-to wake-message
                                                      :via task :hop 1 :kind :approval-result :task task})]
        (when err (timbre/warn "approval result not delivered" {:task task :err err}))))))

(defn- expired? [db task now]
  (when-let [^Date at (field db task :task/expires-at)]
    (.before at now)))

(defn- expire-one!
  "Move a pending Task past its expiry to expired (root as actor) and tell the requester."
  [world task]
  (let [db (act/db world)
        root (root-of db)
        requester (field db task :task/requested-by)]
    (act/move! world root {:task task :to :expired})
    (when requester
      (result-message! world root task requester (field db task :task/message) (some-> (field db task :task/message) (as-> m (field db m :message/route)))
                       (str "approval expired, nothing was run: " (field db task :task/summary))))))

(defn expire!
  "Every pending approval Task past its expiry: expired, requester told. Returns the Task ids."
  [world]
  (let [db (act/db world)
        now (Date.)
        due (for [t (objects/objects-of-class db :task)
                  :when (and (= "approval" (field db t :task/kind)) (= :pending (task-state db t)) (expired? db t now))]
              t)]
    (doseq [t due]
      (try (expire-one! world t) (catch Exception e (timbre/warn e "approval expiry failed" {:task t}))))
    (vec due)))

(defn tell-person!
  "A status Message from p to the people who own it, on a Route they share."
  [world p text]
  (let [db (act/db world)]
    (doseq [person (approvers db p nil)]
      (when-let [r (route-between! world db p person nil)]
        (let [{:keys [err]} (route/send! world p {:to #{person} :route r :kind :status :text text})]
          (when err (timbre/warn "status not delivered" {:person person :err err})))))))

(defn expire-unsafe!
  "End every unsafe mode past its time and tell the person who armed it. Returns the Principals."
  ([world] (expire-unsafe! world (Date.)))
  ([world now]
   (let [ended (unsafe/expire! world now)]
     (doseq [{:keys [principal by]} ended]
       (try
         (let [db (act/db world)
               root (root-of db)]
           (when-let [r (and by (route-between! world db root by nil))]
             (let [{:keys [err]} (route/send! world root {:to #{by} :route r :kind :status
                                                          :text (if (authority/person-terminal? db principal)
                                                                  "a share of your terminal has ended"
                                                                  (str "unsafe mode on " (name-of db principal)
                                                                       " has ended: gated ops need approval again"))})]
               (when err (timbre/warn "unsafe expiry not delivered" {:person by :err err})))))
         (catch Exception e (timbre/warn e "unsafe expiry notice failed" {:principal principal}))))
     (mapv :principal ended))))

(defn- decidable!
  "The Task resolved for :evaluate by the decider and still pending, else an err."
  [world decider task]
  (let [db (act/db world)
        t (->uuid task)]
    (cond
      (not= :task (:object/class (objects/entity db t))) act/unresolved
      ;; the requester never decides its own gate, whatever its Grants say (root included): the gate
      ;; exists so that someone else looks; found live when root approved its own task.run
      (= (->uuid decider) (field db t :task/requested-by)) (fail :conflict "the requester cannot decide its own approval")
      ;; only the people the Task names decide it: a Grant on everything does not make root an
      ;; approver of another Principal's gate
      (not (contains? (set (field db t :task/approvers)) (->uuid decider))) act/unresolved
      (nil? (resolve/resolve! world db decider (act/id-name t) :evaluate)) act/unresolved
      (not= "approval" (field db t :task/kind)) (fail :conflict "not an approval Task")
      :else
      (let [_ (when (and (= :pending (task-state db t)) (expired? db t (Date.))) (expire-one! world t))
            state (task-state (act/db world) t)]
        (if (not= :pending state)
          (fail :conflict (str "Task is " (name state) ", not pending"))
          {:ok t})))))

(defn- wake-of [db task]
  (let [m (field db task :task/message)]
    {:message m :route (when m (field db m :message/route))}))

(defn- execute-approved!
  "The stored op as the requester, with the Task as via; approval is already decided, so the gate is
   skipped (ctx :approved). Returns the op's {:ok} or {:err}."
  [world requester task]
  (let [db (act/db world)
        {:keys [op args]} (field db task :task/op)
        handle (requiring-resolve 'knowledge.world.protocol.ops/handle)
        ;; the approved op runs with the wake it came from (message, route, thread), so a
        ;; principal.create still seats the asker and a named action still defaults its audience (found
        ;; live: person could not resolve the developers root created through the gate)
        {:keys [message route]} (wake-of db task)
        thread-of (requiring-resolve 'knowledge.world.thread/thread-of)
        ctx (cond-> {:world world :via task :hop 1 :loop? true :approved task
                     :conn (atom {:format :transit :session (atom {:id (str "approval-" task) :principal requester :client-kind :loop})})}
              message (assoc :message message :route route :thread (thread-of db message)))
        ;; args were stored in the Field (keyword keys, strings for ids); the handlers coerce as the wire would
        decode (requiring-resolve 'knowledge.schema.wire/decode)
        req (try (decode :ke/Request (j/write-value-as-string {:id (str "approval-" task) :op op :args (or args {})}))
                 (catch Exception e {:error (ex-message e)}))]
    (if (:error req)
      {:err {:code :invalid :message (str "stored op does not decode: " (:error req))}}
      (try (handle ctx (:op req) (:args req))
           (catch Throwable t {:err {:code :invalid :message (str (ex-message t))}})))))

(defn- walk-plain [v]
  (walk/postwalk (fn [x] (cond (keyword? x) (name x) (uuid? x) (str x) (set? x) (vec x) (instance? Date x) (str x) :else x)) v))

(defn- result-text [{:keys [ok err]}]
  (if err
    (str "err " (name (:code err)) ": " (:message err))
    (let [r (dissoc ok :seq)
          s (j/write-value-as-string (walk-plain r))]
      (str "ok " (clip s 1500)))))

(defn admit!
  "A gated op a standing rule or unsafe mode lets through: the same Task a person's approval leaves,
   decided by the rule's author or whoever armed the mode, with task/rule naming it, so the audit walk
   is unchanged; no card, and the run's result comes back in this wake. Returns the op's {:ok} or {:err}."
  [world p rule {:keys [op args via message]}]
  (let [db (act/db world)
        sum (summary db op args)
        {:keys [ok err]} (act/create! world p :task {:title sum})]
    (if err
      {:err (:err (fail :gated (str "a rule admits this but you may not create the Task that records it (" (:message err) "): " sum)))}
      (let [t (:object ok)
            grant (:object/created-under (objects/entity (act/db world) t))
            put! (fn [k type v] (act/append! world (log/pending :field-set t p {:name k :type type :value v :reader :both}
                                                                :delegation grant :via via)))
            move! (fn [from to] (act/append! world (log/pending :task-moved t p {:task t :from from :to to}
                                                                :streams #{p} :delegation grant :via via)))]
        (put! :task/kind :string "approval")
        (put! :task/op :map {:op (name op) :args (storable args)})
        (put! :task/requested-by [:ref :principal] p)
        (when via (put! :task/evaluation [:ref :evaluation] via))
        (when message (put! :task/message [:ref :message] message))
        (put! :task/summary :string sum)
        (put! :task/rule :string (:id rule))
        (put! :task/decided-by [:ref :principal] (->uuid (:by rule)))
        (put! :task/decided-at :instant (Date.))
        (move! :open :approved)
        (when-not (:unsafe rule) (rules/used! world p (:id rule)))
        (let [r (execute-approved! world p t)
              done? (nil? (:err r))
              ran (get-in r [:ok :result :task])
              exit (get-in r [:ok :result :exit])]
          (put! :task/result :map {:ok done? :text (clip (result-text r) 4000)})
          (when (uuid? ran)
            (put! :task/ran [:ref :task] ran)
            (when exit (put! :task/exit :number (long exit))))
          (move! :approved (if done? :done :failed))
          (if done?
            (update-in r [:ok :result] #(assoc (if (map? %) % {:result %}) :approval t :rule (:id rule)))
            r))))))

(defn approve!
  "task.approve by decider: the stored op runs as the requester under its Grants with the Task as via;
   the Task moves pending -> approved -> done|failed; the requester is told. {:ok {...}} or {:err}.
   With wait-ms a long op goes on alone past that time and the answer says it is running: a request
   holds one of the server's few workers, and a build held the person's card for its whole length."
  [world decider {:keys [task note wait-ms]}]
  (let [{:keys [ok err]} (decidable! world decider task)]
    (if err
      {:err err}
      (let [t ok
            db (act/db world)
            requester (field db t :task/requested-by)
            {:keys [message route]} (wake-of db t)
            now (Date.)]
        (act/move! world decider {:task t :to :approved})
        (act/set-field! world decider {:object t :field :task/decided-by :type [:ref :principal] :value decider :reader :both})
        (act/set-field! world decider {:object t :field :task/decided-at :type :instant :value now :reader :both})
        (when note (act/set-field! world decider {:object t :field :task/note :type :string :value (str note) :reader :both}))
        (let [finish (fn [r]
                       (let [text (result-text r)
                             done? (nil? (:err r))
                             ;; the result lands on the Task under the decider's write
                             _ (act/set-field! world decider {:object t :field :task/result :type :map
                                                              :value {:ok done? :text (clip text 4000)} :reader :both})
                             ;; when the approved op made a Task (a run), the approval Task points at it and
                             ;; mirrors its exit, so the requester's next wake can render the kernel's record of the run
                             ran (get-in r [:ok :result :task])
                             _ (when (uuid? ran)
                                 (act/set-field! world decider {:object t :field :task/ran :type [:ref :task] :value ran :reader :both})
                                 (when-let [e (get-in r [:ok :result :exit])]
                                   (act/set-field! world decider {:object t :field :task/exit :type :number :value (long e) :reader :both})))
                             {:keys [seq]} (:ok (act/move! world decider {:task t :to (if done? :done :failed)}))]
                         (result-message! world decider t requester message route
                                          (str "approved by " (name-of db decider) ": " (field db t :task/summary) "\n" text (held-note db t)))
                         {:ok {:result {:task t :state (if done? "done" "failed") :outcome text} :object t :seq seq}}))
              run (fn [] (finish (try (execute-approved! world requester t)
                                      (catch Exception e {:err {:code :failed :message (str (ex-message e))}}))))]
          (if wait-ms
            (let [running (future (run))
                  r (deref running wait-ms ::running)]
              (if (= ::running r)
                {:ok {:result {:task t :state "running" :outcome "approved and running; the result comes back when it ends"} :object t}}
                r))
            (run)))))))

(defn remember!
  "After an approval: a standing rule for what was just approved, made by the person who approved it.
   The match is the one given, else the narrowest the command supports. Returns the rule."
  [world decider task {:keys [match until uses]}]
  (let [db (act/db world)
        t (->uuid task)
        requester (field db t :task/requested-by)
        {:keys [op args]} (field db t :task/op)
        word (gated? db requester (keyword op) args)
        cmd (:cmd (rules/run-of db requester op args))]
    (rules/add! world decider requester {:action (or word (str op)) :match (or match (rules/infer-match cmd))
                                         :until until :uses uses})))

(def denied-next
  "Read by the requester's loop. After a denial models reported and stopped, or tried a variant of the
   same command; none reached for a different tool in 15 runs."
  "That op stays denied, and so does its end: do not repeat or reword it, or reach the same result another way. Other ways remain: read, grep, outline and projects need no card, and a directory a person named is add_project. Use one now if it can answer the question; otherwise say what you found and what stays unknown.")

(defn deny!
  "task.deny by decider with a reason: the Task moves to denied, the requester is told."
  [world decider {:keys [task reason]}]
  (let [{:keys [ok err]} (decidable! world decider task)]
    (if err
      {:err err}
      (let [t ok
            db (act/db world)
            requester (field db t :task/requested-by)
            {:keys [message route]} (wake-of db t)]
        (act/set-field! world decider {:object t :field :task/decided-by :type [:ref :principal] :value decider :reader :both})
        (act/set-field! world decider {:object t :field :task/decided-at :type :instant :value (Date.) :reader :both})
        (act/set-field! world decider {:object t :field :task/reason :type :string :value (str reason) :reader :both})
        (let [{:keys [seq]} (:ok (act/move! world decider {:task t :to :denied}))]
          (result-message! world decider t requester message route
                           (str "denied by " (name-of db decider) ": " (field db t :task/summary) "\nreason: " reason
                                "\n" denied-next (held-note db t)))
          {:ok {:result {:task t :state "denied" :reason (str reason)} :object t :seq seq}})))))

(defn- pending-for
  "The pending approval Tasks a decider is named on, oldest first, narrowed by a filter."
  [db decider {:keys [requested-by action thread]}]
  (let [thread-of (requiring-resolve 'knowledge.world.thread/thread-of)
        who (when requested-by (or (->uuid requested-by) (resolve/lookup db decider (str requested-by))))
        wanted (when thread (->uuid thread))]
    (->> (objects/objects-of-class db :task)
         (filter (fn [t]
                   (let [{:keys [op args]} (field db t :task/op)
                         m (field db t :task/message)]
                     (and (= "approval" (field db t :task/kind))
                          (= :pending (task-state db t))
                          (contains? (set (field db t :task/approvers)) decider)
                          (or (nil? who) (= who (field db t :task/requested-by)))
                          (or (nil? action)
                              (= action (some-> (:action args) name))
                              (= action (str op))
                              (= action (gated? db (field db t :task/requested-by) (keyword op) args)))
                          (or (nil? wanted) (and m (= wanted (thread-of db m))))))))
         (sort-by #(or (:object/created-seq (objects/entity db %)) 0)))))

(defn decide-many!
  "task.decide-many: each Task decided exactly as approve! or deny! would, one result per Task."
  [world decider {:keys [tasks filter decision reason]}]
  (let [db (act/db world)
        ids (if (seq tasks) (keep ->uuid tasks) (pending-for db decider filter))
        approving? (= "approve" (str decision))
        ;; a kernel swap or a person's Grant is decided on its own card, never in a batch
        alone? (fn [t] (let [{:keys [op args]} (field db t :task/op)] (authority/must-card? db (keyword (str op)) args)))
        one (fn [t]
              (let [{:keys [ok err]} (try (cond
                                            (and approving? (alone? t))
                                            (fail :conflict "this one is approved on its own card")
                                            approving? (approve! world decider {:task t})
                                            :else (deny! world decider {:task t :reason (or reason "denied with others")}))
                                          (catch Exception e {:err {:code :invalid :message (str (ex-message e))}}))]
                (if err
                  {:task t :ok false :err err}
                  {:task t :ok true :state (get-in ok [:result :state])})))
        results (mapv one ids)]
    {:count (count results) :decided (count (clojure.core/filter :ok results)) :results results}))
