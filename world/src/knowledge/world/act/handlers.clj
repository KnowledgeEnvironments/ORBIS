(ns knowledge.world.act.handlers
  "The handlers behind the shipped actions (knowledge.world.act.actions). Each composes the
   existing primitives as the caller, so every write is on the log with the caller as actor and
   the action's Evaluation as via, and every refusal is the same err a client would get:

   - principal.create: a Principal, its charter, a Route from the caller, the caller's Model
     bound with an evaluate Grant, loop/accepts-from, empty memory/notes, the Grant named (strictly
     narrower than the caller's, else err narrower-required), evaluate on task.run and any other
     actions asked for. A process Principal's token goes to data/<name>.token (0600) and is returned
     only to a session, never to a loop (the result lands on the log as evaluation/results).
   - workspace.mount: fs and shell Adapters at a path, bound as <name> and <name>.shell, in the
     Principal's principal/adapters, with Grants on the prefix and the Adapters; the path must lie
     in one of the caller's own mounts unless the caller's scope is everything (err outside-mount).
   - route.create, route.join: explicit Routes on the listener transport.
   - harness.index, task.run: the index and run actions on Adapters, by name."
  (:require [knowledge.world.thread :as thread]
            [knowledge.world.authority :as authority]
            [knowledge.world.adapters.harness :as harness]
            [knowledge.world.delegate :as delegate]
            [knowledge.world.act.history :as history] [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.perform :as perform]
            [knowledge.world.act.rules :as rules]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.adapters.world :as wadapter]
            [knowledge.world.auth :as auth]
            [knowledge.world.code :as code]
            [knowledge.world.cost :as cost]
            [knowledge.world.chatgpt :as chatgpt]
            [knowledge.world.models :as models]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.gateway.core :as gateway]
            [knowledge.world.log.core :as log]
            [knowledge.world.loop.queue :as queue]
            [knowledge.world.changes :as changes]
            [knowledge.world.conversation :as conversation]
            [knowledge.world.manual :as manual]
            [knowledge.world.notify :as notify]
            [knowledge.world.projects :as projects]
            [knowledge.world.release :as release]
            [knowledge.world.search.core :as search]
            [knowledge.world.speech :as speech]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]
            [knowledge.world.view.core :as view]
            [taoensso.timbre :as timbre])
  ;; model.create, model.probe, cost.report, search.find live at the end of this file;
  ;; token.issue, token.revoke, token.list, pair.begin, world.link, world.send after them
  (:import [java.io File]
           [java.util Date]))

(defn- ok! [{:keys [ok err]}] (if err (throw (ex-info (str (:code err) " " (:message err)) {:err err})) ok))

(defn- shipped
  "A shipped instruction resource's text, with <you> replaced by the Principal's name."
  [file n]
  (some-> (io/resource (str "knowledge/world/boot/instructions/" file)) slurp (str/replace "<you>" (str n))))
(defn- fail! [code message] (throw (ex-info message {:err {:code code :message message}})))

(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- name-of
  "The name a Principal is known by: in the world namespace first, else its own, else the short id."
  [db id]
  (let [wid (objects/world-id db)
        names (objects/names db id)]
    (or (some (fn [[ns n]] (when (= ns wid) n)) (sort-by second names))
        (some (fn [[ns n]] (when (= ns id) n)) (sort-by second names))
        (subs (str id) 0 8))))

(defn- bind-world!
  "Bind name in the world namespace when the caller may; a caller who may not keeps the name in
   its own namespace (create! already bound it there)."
  [world p n object]
  (let [{:keys [err]} (act/bind! world p {:namespace (:world-id world) :name n :object object})]
    (nil? err)))

(defn- append-refs!
  "A list-of-refs Field on object gains ids (distinct, in order)."
  [world p object k class ids via]
  (let [db (act/db world)
        now (vec (filter uuid? (field db object k)))
        value (vec (distinct (into now ids)))]
    (ok! (act/set-field! world p {:object object :field k :type [:list [:ref class]] :value value :reader :both :via via}))))

(defn- scope-of [v]
  (cond (= v "everything") :everything
        (map? v) (cond-> {}
                   (:prefix v) (assoc :prefix (str (:prefix v)))
                   (:class v) (assoc :class (keyword (name (:class v))))
                   (:field-prefix v) (assoc :field-prefix (str (:field-prefix v))))
        (sequential? v) (set (keep ->uuid v))
        :else v))

(defn- ops-of [v] (set (map keyword v)))

(defn- first-model
  "The Principal's first Model, or nil."
  [db p]
  (some (fn [e] (when (= :model (field db e :model/kind)) e)) (sort-by str (field db p :principal/models))))

(defn- transport-of
  "The transport Adapter of the caller's first Route, else transport.listener."
  [db p]
  (or (some (fn [r] (field db r :route/transport)) (field db p :principal/routes))
      (objects/lookup db (objects/world-id db) "transport.listener")))

(defn- root-scoped?
  "Does p hold an active Grant with scope :everything?"
  [db p]
  (let [holders (conj (resolve/groups-of db p) p)]
    (boolean (some #(= :everything (:grant/scope %)) (resolve/active-grants db holders (Date.))))))

(defn- strictly-narrower!
  "The caller's delegable Grant covering scope and ops, when the request is strictly narrower than
   it: a smaller scope, or fewer ops, never the same. Throws narrower-required or unresolved."
  [db p scope ops]
  (let [g (act/delegation-for db p scope ops)]
    (when-not g (fail! :unresolved "you hold no delegable Grant covering that scope and those ops"))
    (let [mine (objects/state db g)
          mine-ops (disj (set (:grant/ops mine)) :delegate)]
      (when (and (= (:grant/scope mine) scope) (= mine-ops (set ops)))
        (fail! :narrower-required "the Grant must be strictly narrower than yours: a smaller scope or fewer ops"))
      g)))

;; ---------------------------------------------------------------- principal.create

(defn- developer-template?
  "Is this a developer being made: `template developer`, or a charter that is the shipped template's text?"
  [{:keys [template charter]}]
  (or (= "developer" (some-> template name))
      (boolean (and (string? charter) (str/includes? charter "a developer of this harness")))))

(defn- developer-charter
  "The shipped developer template filled for name, subtree and check."
  [n subtree check]
  (some-> (shipped "charter-template-developer.md" n)
          (str/replace #"(?s)^.*?(?=You are )" "")
          (str/replace "<name>" (str n))
          (str/replace "<subtree>" (str subtree))
          (str/replace "<check command>" (str (or check "the project's own checks")))))

(defn- create-args!
  "a developer needs its subtree (this action mounts it, so a blank one would mount
   the repository root); anyone else needs a charter. Throws invalid-args. Run by the preflight
   (loops) and by the handler (sessions)."
  [world {:keys [charter subtree] n :name :as args}]
  (when (and n (objects/lookup (act/db world) (:world-id world) (str n)))
    (fail! :conflict (str "the name " n " is taken")))
  (if (developer-template? args)
    (let [root (fs/work-root (:config world))]
      (when (str/blank? (str subtree)) (fail! :invalid-args "a developer needs `subtree` (listener or world): the action mounts it"))
      (when (re-find #"\.\.|^/" (str subtree)) (fail! :invalid-args "subtree is a name under the repository root, not a path"))
      (when-not (and root (.isDirectory (io/file root (str subtree))))
        (fail! :invalid-args (str "subtree " subtree " is not a directory under the repository root"))))
    (when (str/blank? (str charter)) (fail! :invalid-args "charter is required (or template \"developer\" with subtree)"))))

(defmethod actions/preflight! :principal.create
  [_ world p {:keys [grant] :as args} ]
  ;; refused before the gate, not after a person approved it
  (try (let [db (act/db world) ops (ops-of (:ops grant)) scope (scope-of (:scope grant))]
         (when (contains? ops :delegate) (fail! :narrower-required "a Principal you create may not delegate (no :delegate in ops)"))
         (strictly-narrower! db p scope ops)
         (create-args! world args)
         nil)
       (catch clojure.lang.ExceptionInfo e
         (if-let [err (:err (ex-data e))] {:err err} (act/err :invalid (ex-message e))))))


(declare mount!)

(defn- instruct-new!
  "A created Principal's charter, the protocol and workspace how-tos, and who may wake it. Root alone
   had the how-tos once, and the first created Principals failed 48 ops on vocabulary."
  [world p new name charter from via]
  (ok! (act/set-field! world p {:object new :field :instructions/charter :type :string :value charter :reader :both :via via}))
  (doseq [[k file] [[:instructions/protocol "protocol.md"] [:instructions/workspace "workspace.md"]]]
    (when-let [text (shipped file name)]
      (ok! (act/set-field! world p {:object new :field k :type :string :value text :reader :both :via via}))))
  (ok! (act/set-field! world p {:object new :field :loop/accepts-from :type [:list [:ref :principal]] :value [from] :reader :both :via via})))

(defn- model-new!
  "The Model and the Grant to evaluate on it, ending at until when given. Returns the Grant."
  [world p new model via until]
  (when model
    (ok! (act/set-field! world p {:object new :field :principal/models :type [:list [:ref :model]] :value [model] :reader :both :via via}))
    (:grant (ok! (act/grant! world p {:grantee new :scope #{model} :ops #{:read :evaluate} :delegable false :until until})))))

(defn- seat-people!
  "The asker of this wake (decisions 2026-10-30) and the first person sit on a new Principal's Route,
   so a person can always talk to any Principal without asking root. The first person's Grants are
   only what the caller can delegate: a narrow caller seats the person, it cannot widen them."
  [world p new route from asker owner via]
  (when (and asker (not= asker from))
    (append-refs! world p asker :principal/routes :route [route] via)
    (ok! (act/grant! world p {:grantee asker :scope #{route} :ops #{:read :transmit} :delegable false}))
    (ok! (act/grant! world p {:grantee asker :scope #{new} :ops #{:read} :delegable false})))
  (when (and owner (not= owner from) (not= owner asker))
    (append-refs! world p owner :principal/routes :route [route] via)
    (when (act/delegation-for (act/db world) p #{route} #{:read :transmit})
      (ok! (act/grant! world p {:grantee owner :scope #{route} :ops #{:read :transmit} :delegable false})))
    (when (act/delegation-for (act/db world) p #{new} #{:read})
      (ok! (act/grant! world p {:grantee owner :scope #{new} :ops #{:read} :delegable false})))))

(defn- route-new!
  "The Route from the caller, on both Principals. Returns {:route :route-name}."
  [world p new name transport from asker owner via]
  (let [route-name (str (name-of (act/db world) from) "." name)
        members (vec (distinct (remove nil? [from new asker owner])))
        route (:object (ok! (act/create! world p :route {:transport transport :address name :members members :name route-name})))]
    (bind-world! world p route-name route)
    (append-refs! world p from :principal/routes :route [route] via)
    (append-refs! world p new :principal/routes :route [route] via)
    (seat-people! world p new route from asker owner via)
    {:route route :route-name route-name}))

(defn- schema-grant!
  "What the caller may delegate on a class's schema: a run is a Task and a view a Serialization, and
   the new Principal creates its own."
  [world p new class until]
  (let [schema (objects/lookup (act/db world) (:world-id world) (act/schema-name class))]
    (when (and schema (act/delegation-for (act/db world) p #{schema} #{:create :read}))
      (ok! (act/grant! world p {:grantee new :scope #{schema} :ops #{:create :read} :delegable false :until until})))))

(defn- grant-new!
  "A new Principal's authority, each piece only what the caller may give. Its Route is named
   <caller>.<name>, outside its own prefix, so it needs transmit on that by id; it reads and writes
   its own record whatever its prefix is called; the Grant named, strictly narrower, is never
   delegable. Returns {:route-grant :grant :seq :actions}."
  [world p new route scope ops {:keys [until] :as args}]
  (let [route-grant (:grant (ok! (act/grant! world p {:grantee new :scope #{route} :ops #{:read :transmit} :delegable false :until until})))
        _ (ok! (act/grant! world p {:grantee new :scope #{new} :ops #{:read :write} :delegable false :until until}))
        {g :grant seq :seq} (ok! (act/grant! world p {:grantee new :scope scope :ops ops :delegable false :until until}))
        _ (schema-grant! world p new :task until)
        _ (schema-grant! world p new :serialization until)
        action-objects (actions/action-objects (act/db world))
        wanted (distinct (into ["task.run" "view.present" "view.table-of" "view.graph-of" "search.find" "cost.report"
                                 "pane.open" "pane.list" "pane.close" "pane.resize" "pane.read" "pane.send" "pane.frob" "file.open"]
                                (:actions args)))
        granted (vec (for [n wanted
                           :let [id (get action-objects n)]
                           :when (and id (act/delegation-for (act/db world) p #{id} #{:evaluate}))]
                       (do (ok! (act/grant! world p {:grantee new :scope #{id} :ops #{:read :evaluate} :delegable false :until until})) n)))]
    {:route-grant route-grant :grant g :seq seq :actions granted}))

(defn- supervise-new!
  "The caller watches the new Principal's loop/stalled: a non-empty value fires a Message, and the
   caller's loop wakes. A caller who may not create Triggers simply has none."
  [world p new name]
  (:object (:ok (act/create! world p :trigger {:kind :on-change :watch {:object new :field :loop/stalled}
                                               :unless-empty true :unless-told true :armed true :to p
                                               :text (str "loop/stalled on " name " (a Principal you created)")
                                               :name (str name ".stalled")}))))

(defn- readable-mounts
  "The filesystem Adapters p can read, the most a subagent starts with."
  [db p]
  (vec (for [a (objects/objects-of-class db :adapter)
             :when (and (= :filesystem (field db a :adapter/binds)) (not (objects/retired? db a))
                        (resolve/resolves? db p (act/id-name a) :read))]
         a)))

(defmethod actions/handle! :principal.create
  [_ world p {:keys [name charter driven-by model routes-from grant subtree check ttl-s] :as args} {:keys [via session? message] :as opts}]
  (let [db (act/db world)
        _ (create-args! world args)
        task (->uuid (:for args))
        _ (when (and (:for args) (not (and task (= :task (:object/class (objects/entity db task))))))
            (fail! :invalid-args "`for` names a Task"))
        ephemeral? (or task ttl-s)
        ;; what a short-lived Principal makes cannot outlast it
        _ (when (and (delegate/state db p) (not ephemeral?))
            (fail! :narrower-required "a temporary Principal makes only temporary ones: give `for` or `ttl-s`"))
        until (when ephemeral? (delegate/until-for db p ttl-s (java.util.Date.)))
        ;; a little longer than the Principal, so its last word to its parent can still go out
        grant-until (when until (java.util.Date. (+ (.getTime ^java.util.Date until) 60000)))
        args (cond-> args grant-until (assoc :until grant-until))
        developer? (developer-template? args)
        charter (if (and developer? (or (str/blank? (str charter)) (str/includes? (str charter) "<subtree>")))
                  (developer-charter name subtree check)
                  charter)
        asker (when message (let [a (thread/asked-by db message)] (when (and a (not= a p)) a)))
        owner (gates/first-person db)
        driven-by (keyword (or driven-by "process"))
        scope (scope-of (:scope grant))
        ops (ops-of (:ops grant))
        _ (when (contains? ops :delegate) (fail! :narrower-required "a Principal you create may not delegate (no :delegate in ops)"))
        _ (strictly-narrower! db p scope ops)
        from (or (->uuid routes-from) p)
        model (or (->uuid model) (when (= :process driven-by) (first-model db p)))
        transport (transport-of db p)
        _ (when (and (= :process driven-by) (nil? model)) (fail! :invalid-args "no Model: give `model` or bind one to yourself first"))
        {:keys [object result]} (ok! (act/create! world p :principal {:driven-by driven-by :name name} {:return-token? session?}))
        new object
        ;; bookkeeping, not the caller's Grant: a recipient must be able to name who speaks to it
        _ (act/bind-in-world! world new name)
        _ (instruct-new! world p new name charter from via)
        model-grant (model-new! world p new model via grant-until)
        {:keys [route route-name]} (route-new! world p new name transport from asker owner via)
        {:keys [route-grant seq] g :grant granted-actions :actions} (grant-new! world p new route scope ops args)
        token-file (:token-file result)
        supervisor (supervise-new! world p new name)
        _ (when ephemeral?
            (when task (ok! (act/grant! world p {:grantee new :scope #{task} :ops #{:read :write} :delegable false :until grant-until})))
            (delegate/mark! world new {:for task :until until :parent p :supervisor supervisor :route route}))
        ;; a developer's subtree is mounted here, for the new Principal: left to the model, root
        ;; reused its own mount in every run and the developer had no shell
        mount (when developer?
                (:result (ok! (mount! world p {:principal (str new) :path (str "harness/" subtree) :name name} opts))))]
    {:ok {:result (cond-> {:principal new :name name :route route :route-name route-name :route-grant route-grant :grant g :actions granted-actions}
                    mount (assoc :mount mount :subtree subtree)
                    (and asker (not= asker from)) (assoc :asker asker)
                    supervisor (assoc :supervisor supervisor)
                    until (assoc :until (str (.toInstant ^java.util.Date until)))
                    task (assoc :for task)
                    model (assoc :model model :model-grant model-grant)
                    token-file (assoc :token-file token-file)
                    (and session? (:token result)) (assoc :token (:token result)))
          :object new :seq seq}}))

;; ---------------------------------------------------------------- delegate

(defmethod actions/handle! :delegate
  [_ world p {:keys [task charter deadline-s ttl-s grant]} {:keys [via message hop] :as opts}]
  (let [db (act/db world)
        now (java.util.Date.)
        deadline (min delegate/max-ttl-s (max 1 (long (or deadline-s delegate/default-deadline-s))))
        title (let [t (str/trim (str task))] (if (> (count t) 80) (str (subs t 0 80) "...") t))
        _ (when (str/blank? title) (fail! :invalid-args "`task` says what to do"))
        nm (str (name-of db p) ".sub-" (subs (str (random-uuid)) 0 4))
        scope (or (:scope grant) (let [m (readable-mounts db p)] (if (seq m) (mapv str m) {:prefix nm})))
        ops (or (:ops grant) ["read" "reference"])
        asker (when message (thread/asked-by db message))
        made (ok! (act/create! world p :task {:title title}))
        t (:object made)
        put! (fn [k type v] (ok! (act/set-field! world p {:object t :field k :type type :value v :reader :both :via via})))
        _ (put! :task/kind :string "delegated")
        _ (put! :task/due :instant (java.util.Date. (+ (.getTime now) (* 1000 deadline))))
        _ (when message (put! :task/message [:ref :message] message))
        _ (when (and asker (not= asker p)) (put! :task/requested-by [:ref :principal] asker))
        ;; the child outlives its deadline by as much again, so the parent can still hear from it
        child (:result (ok! (actions/handle! :principal.create world p
                                             {:name nm :for (str t) :ttl-s (or ttl-s (* 2 deadline))
                                              :charter (or charter (delegate/charter nm (name-of db p) task))
                                              :grant {:scope scope :ops ops}}
                                             opts)))
        sent (ok! (route/send! world p (cond-> {:to #{(:principal child)} :route (:route child) :text (str task)
                                                :via via :hop (if via (inc (long (or hop 0))) nil)}
                                         message (assoc :thread (thread/thread-of (act/db world) message)))))]
    {:ok {:result {:delegated t :principal (:principal child) :name nm :deadline-s deadline :until (:until child)
                   :message (:message sent)
                   :next "Say it is delegated and stop. The result wakes you."}
          :object t :seq (:seq sent)}}))

(defmethod actions/handle! :task.progress
  [_ world p {:keys [text task]} _]
  (delegate/progress! world p (or (->uuid task) (delegate/task-of (act/db world) p)) text))

(defmethod actions/handle! :task.ask
  [_ world p {:keys [text]} {:keys [via message hop]}]
  (delegate/ask! world p text {:via via :message message :hop hop}))

;; ---------------------------------------------------------------- workspace.mount

(defn- canonical-dir ^File [path]
  (let [f (.getCanonicalFile (io/file path))]
    (when (.isDirectory f) f)))

(defn- inside?
  "Is dir equal to or under root?"
  [^File root ^File dir]
  (let [rp (.getPath (.getCanonicalFile root)) dp (.getPath dir)]
    (or (= rp dp) (str/starts-with? dp (str rp File/separator)))))

(defn- own-mounts
  "The directories of the caller's fs Adapters."
  [db p]
  (for [a (field db p :principal/adapters)
        :when (= :filesystem (field db a :adapter/binds))
        :let [l (field db a :adapter/locator)]
        :when (string? l)]
    (io/file (str/replace l #"^file://" ""))))

(defmethod actions/handle! :workspace.mount
  [_ world p args opts]
  (mount! world p args opts))

(defn- mount!
  "workspace.mount's work: fs (and shell) Adapters at path for the Principal, bound <name>.fs and
   <name>.shell, Grants on the prefix and the Adapters. Shared with principal.create for developers."
  [world p {:keys [principal path name shell read-only]} {:keys [via]}]
  (let [db (act/db world)
        wid (:world-id world)
        target (or (->uuid principal) (resolve/lookup db p (str principal)))
        ;; a relative path, or a `harness/<subtree>` name, means a directory under the repository root
        harness-root (fs/work-root (:config world))
        harness-rel (cond (str/starts-with? (str path) "harness/") (subs (str path) 8)
                          (and (string? path) (not (.isAbsolute (io/file (str path))))) (str path))
        dir (or (canonical-dir path)
                (when (and harness-root harness-rel) (canonical-dir (io/file harness-root harness-rel))))
        ;; a subtree of the harness is a developer's mount; its shell runs at the repository
        ;; root (make, ctest and git work there) with the subtree recorded as adapter/scope (report
        ;; 2026-10-30, finding 4: both developers failed `make -C ...` from the subtree)
        harness-subtree? (boolean (and harness-root dir (inside? (io/file harness-root) dir) (not= (.getPath dir) harness-root)))
        shell? (not (false? shell))]
    (when-not (and target (= :principal (:object/class (objects/state db target))))
      (fail! :unresolved (str "no Principal " principal " (an id or a name you can look up); principal.create it first")))
    (when-not dir (fail! :invalid-args (str "path is not a directory: " path)))
    ;; a name already bound in the world is not free (seen live: a mount named `world` collided with the
    ;; world Principal and surfaced as not-implemented)
    (when-let [taken (objects/lookup db wid (str name))]
      (when (not= taken target) (fail! :conflict (str "name " name " is taken (it names " (:object/class (objects/state db taken)) " " taken "); mount under the Principal's own name or another"))))
    (when (objects/lookup db wid (str name ".fs"))
      (fail! :conflict (str name ".fs is already a mount; unmount or choose another name")))
    (when-not (or (root-scoped? db p) (some #(inside? % dir) (own-mounts db p)))
      (fail! :outside-mount (str path " lies outside your own mounts; only a Principal whose Grant is scope everything may mount it")))
    (let [locator (.getPath dir)
          ;; the mount is bound <name>.fs and the shell <name>.shell; the bare name stays the Principal's,
          ;; and <name>/<file> resolves through the Principal's workspace (resolve.mount)
          fs-adapter (:object (ok! (act/create! world p :adapter {:binds :filesystem :locator locator :sync :pull :kind :storage :name (str name ".fs")})))
          _ (bind-world! world p (str name ".fs") fs-adapter)
          _ (when read-only (ok! (act/set-field! world p {:object fs-adapter :field :adapter/read-only :type :boolean :value true :reader :both :via via})))
          shell-adapter (when shell?
                          (let [shell-locator (if harness-subtree? harness-root locator)
                                id (:object (ok! (act/create! world p :adapter {:binds :host :locator shell-locator :sync :both :kind :shell :name (str name ".shell")})))]
                            (bind-world! world p (str name ".shell") id)
                            (when-let [cmd (and (not harness-subtree?) (code/test-command locator))]
                              (ok! (act/set-field! world p {:object id :field :adapter/test :type :string :value cmd :reader :both :via via})))
                            (when harness-subtree?
                              (ok! (act/set-field! world p {:object id :field :adapter/cwd :type :string :value harness-root :reader :both :via via}))
                              (ok! (act/set-field! world p {:object id :field :adapter/scope :type :string :value locator :reader :both :via via})))
                            id))
          adapters (vec (remove nil? [fs-adapter shell-adapter]))
          _ (append-refs! world p target :principal/adapters :adapter adapters via)
          prefix-ops (cond-> #{:read :reference} (not read-only) (conj :write))
          g1 (:grant (ok! (act/grant! world p {:grantee target :scope {:prefix name} :ops prefix-ops :delegable false})))
          r2 (ok! (act/grant! world p {:grantee target :scope #{fs-adapter} :ops #{:read} :delegable false}))
          r3 (when shell-adapter (ok! (act/grant! world p {:grantee target :scope #{shell-adapter} :ops #{:read :evaluate} :delegable false})))
          g2 (:grant r2)
          g3 (:grant r3)
          seq (:seq (or r3 r2))]
      {:ok {:result (cond-> {:principal target :mount name :fs fs-adapter :fs-name (str name ".fs") :path locator :grants (vec (remove nil? [g1 g2 g3]))}
                      shell-adapter (assoc :shell shell-adapter :shell-name (str name ".shell"))
                      (and shell-adapter harness-subtree?) (assoc :shell-cwd harness-root :shell-scope locator
                                                                  :note "the shell runs at the repository root; the mount is the subtree"))
            :object fs-adapter :seq seq}})))

;; ---------------------------------------------------------------- projects

(defmethod actions/handle! :projects
  [_ world p _ _]
  {:ok {:result (binding [harness/*data-dir* (:data-dir world)] (projects/status (act/db world) p))}})

(def ^:private project-grant
  "A project's Principal holds its own names, and nothing else."
  ["read" "reference" "transmit" "write" "evaluate" "create"])

(defn- project-charter [name creator-name what]
  (str "You own the project " name (when what (str ": " what)) ". Work inside your mount `" name
       "`, answer from its files, and ask " creator-name " on your Route for anything beyond it."))

(defmethod actions/preflight! :project.add
  [_ world p {:keys [name path]}]
  (let [db (act/db world)]
    (cond
      (not (and (string? path) (canonical-dir path)))
      (act/err :invalid-args (str "path is not a directory: " path))
      (not (root-scoped? db p))
      (act/err :outside-mount "only a Principal whose Grant is scope everything adds a project")
      (or (objects/lookup db (:world-id world) (str name)) (objects/lookup db (:world-id world) (str name ".fs")))
      (act/err :conflict (str "name " name " is taken; `projects` shows what you have")))))

(defmethod actions/handle! :project.add
  [_ world p {:keys [name path charter] :as args} opts]
  (when-let [{:keys [err]} (actions/preflight! :project.add world p args)] (throw (ex-info (:message err) {:err err})))
  (let [dir (.getPath ^File (canonical-dir path))
        readme (projects/head-of-readme dir)
        what (projects/about dir)
        ;; `grid project` is not a charter (seen live): the README says more
        short? (< (count (str/trim (str charter))) 40)
        made (:result (ok! (actions/handle! :principal.create world p
                                            {:name name
                                             :charter (if short? (project-charter name (name-of (act/db world) p) what) charter)
                                             :grant {:scope {:prefix name} :ops project-grant}}
                                            opts)))
        mounted (:result (ok! (mount! world p {:principal (str (:principal made)) :path dir :name name} opts)))
        tests (some-> (:shell mounted) (#(field (act/db world) % :adapter/test)))]
    {:ok {:result (cond-> {:principal (:principal made) :name name :route (:route made) :route-name (:route-name made)
                           :mount (:mount mounted) :path dir :shell-name (:shell-name mounted)
                           :next (if short?
                                   "its charter was written from the README's first line: set instructions/charter if you can say more"
                                   "done")}
                    readme (assoc :readme readme)
                    tests (assoc :tests tests))
          :object (:principal made)}}))

;; ---------------------------------------------------------------- routes

(defmethod actions/handle! :route.create
  [_ world p {:keys [name members]} {:keys [via]}]
  (let [db (act/db world)
        ids (vec (keep ->uuid members))]
    (when (empty? ids) (fail! :invalid-args "members must name at least one Principal"))
    (when-not (every? #(objects/exists? db %) ids) (fail! :unresolved "a member does not exist"))
    (let [transport (transport-of db p)
          {:keys [object seq]} (ok! (act/create! world p :route {:transport transport :address name :members ids :name name}))
          _ (bind-world! world p name object)
          joined (vec (for [m ids] [m (nil? (:err (act/set-field! world p {:object m :field :principal/routes :type [:list [:ref :route]]
                                                                             :value (vec (distinct (conj (vec (filter uuid? (field (act/db world) m :principal/routes))) object)))
                                                                             :reader :both :via via})))]))
          ;; each member may transmit on it, when the caller can delegate that (a member who already can is unharmed)
          _ (doseq [m ids :when (not= m p)]
              (when (act/delegation-for (act/db world) p #{object} #{:read :transmit})
                (act/grant! world p {:grantee m :scope #{object} :ops #{:read :transmit} :delegable false})))]
      {:ok {:result {:route object :name name :joined (vec (for [[m ok] joined :when ok] m)) :skipped (vec (for [[m ok] joined :when (not ok)] m))}
            :object object :seq seq}})))

(defmethod actions/handle! :route.join
  [_ world p {:keys [route principal]} {:keys [via]}]
  (let [db (act/db world) r (->uuid route) m (->uuid principal)]
    (when-not (= :route (:object/class (objects/state db r))) (fail! :unresolved "route is not a Route"))
    (when-not (objects/exists? db m) (fail! :unresolved "principal does not exist"))
    (let [seq (append-refs! world p m :principal/routes :route [r] via)]
      (when (and (not= m p) (act/delegation-for (act/db world) p #{r} #{:read :transmit}))
        (act/grant! world p {:grantee m :scope #{r} :ops #{:read :transmit} :delegable false}))
      {:ok {:result {:route r :principal m} :object r :seq (:seq seq)}})))

;; ---------------------------------------------------------------- adapters by name

(defn- adapter-by
  "An Adapter from an id or a name the caller can look up."
  [db p v]
  (or (->uuid v) (resolve/lookup db p (str v))))

(defmethod actions/handle! :world.diff
  [_ world p args _]
  (let [r (history/since-fork! world p args)]
    (if (:err r) r {:ok {:result (:ok r)}})))

(defmethod actions/handle! :harness.index
  [_ world p {:keys [adapter]} {:keys [via]}]
  (let [db (act/db world)
        a (or (some->> adapter (adapter-by db p)) (objects/lookup db (objects/world-id db) "harness"))]
    (when-not a (fail! :unresolved "no harness mount"))
    (perform/perform! world p {:object a :action :index :args {} :via via})))

(defmethod actions/handle! :task.run
  [_ world p {:keys [adapter cmd cwd timeout-s]} {:keys [via]}]
  (let [db (act/db world)
        a (adapter-by db p adapter)]
    (when-not a (fail! :unresolved (str "no Adapter " adapter)))
    (perform/perform! world p {:object a :action :run :args (cond-> {:cmd cmd} cwd (assoc :cwd cwd) timeout-s (assoc :timeout-s timeout-s)) :via via})))

;; ---------------------------------------------------------------- views

(defn- audience
  "Where a view goes when the action does not say: the Route of the Message being answered and its
   sender, else the caller's first Route and the other members on it."
  [db p {:keys [to route]} {:keys [message] wake-route :route}]
  (let [route (or (->uuid route) wake-route (first (field db p :principal/routes)))
        to (set (keep ->uuid to))
        to (cond (seq to) to
                 (and message (uuid? (field db message :message/from))) #{(field db message :message/from)}
                 route (disj (set (field db route :route/members)) p)
                 :else #{})]
    {:route route :to to}))

(defn- present-view!
  [world p kind title data about opts in-reply-to {:keys [to route]}]
  (when-not route (fail! :invalid-args "no Route: give `route` (you are on none and answer no Message)"))
  (when (empty? to) (fail! :invalid-args "no recipient: give `to`"))
  (let [{:keys [ok err]} (view/present! world p {:to to :route route :kind kind :title title :data data :about about
                                                 :title-refs (:title-refs opts) :data-refs (:data-refs opts)
                                                 :in-reply-to in-reply-to :via (:via opts) :hop (when (:via opts) 1)})]
    (if err {:err err} {:ok {:result ok :object (:serialization ok) :seq (:seq ok)}})))

(defmethod actions/handle! :view.present
  [_ world p {:keys [kind title data about in-reply-to] :as args} {:keys [message] :as opts}]
  (present-view! world p (keyword kind) title data about (merge opts (select-keys args [:title-refs :data-refs])) (or (->uuid in-reply-to) message) (audience (act/db world) p args opts)))

(defmethod actions/handle! :view.table-of
  [_ world p {:keys [objects fields title class] :as args} {:keys [message] :as opts}]
  (let [db (act/db world)
        _ (when (and (empty? objects) (nil? class)) (fail! :invalid-args "give objects (ids) or class"))
        {:keys [data about skipped]} (view/table-of db p {:objects objects :fields fields :class class})]
    (when (empty? about) (fail! :unresolved (if class (str "no " class " resolves for you") "none of those Objects resolves for you")))
    (let [r (present-view! world p :table (or title (if class (str (count about) " " class "s: " (str/join ", " fields)) (str (count about) " objects: " (str/join ", " fields)))) data about (merge opts (select-keys args [:title-refs :data-refs])) message (audience db p args opts))]
      (cond-> r (and (:ok r) (seq skipped)) (assoc-in [:ok :result :skipped] skipped)))))

(defmethod actions/handle! :view.graph-of
  [_ world p {:keys [root depth title] :as args} {:keys [message] :as opts}]
  (let [db (act/db world)
        {:keys [data about err]} (view/graph-of db p {:root root :depth depth})]
    (when err (throw (ex-info (:message err) {:err err})))
    (present-view! world p :graph (or title (str "around " (view/name-of db (->uuid root)) (when (= 2 (long (or depth 1))) ", depth 2")))
                   data about (merge opts (select-keys args [:title-refs :data-refs])) message (audience db p args opts))))

;; ---------------------------------------------------------------- approval gates

(defn- writable-adapter!
  "The Adapter v names (an id or a name) when p may write it, else throws."
  [world db p v]
  (let [id (or (->uuid v) (resolve/lookup db p (str v)))]
    (when-not (and id (= :adapter (:object/class (objects/entity db id)))
                   (resolve/resolve! world db p (act/id-name id) :write))
      (fail! :unresolved (str "no Adapter " v " you may write")))
    id))

(defn- writable-principal!
  "The Principal v names (an id or a name) when p may write it, else throws."
  [world db p v]
  (let [target (or (->uuid v) (resolve/lookup db p (str v)))]
    (when-not (and target (= :principal (:object/class (objects/entity db target)))
                   (resolve/resolve! world db p (act/id-name target) :write))
      (fail! :unresolved (str "no Principal " v " you may write")))
    target))

(def ^:private approve-wait-ms "How long a card waits for what it approved before the answer says running." 2000)

(defmethod actions/handle! :task.approve
  [_ world p {:keys [task note remember]} opts]
  (when remember (authority/require-person! (act/db world) p opts))
  (let [r (gates/approve! world p {:task task :note note :wait-ms approve-wait-ms})]
    (if (and remember (:ok r))
      (assoc-in r [:ok :result :rule] (gates/remember! world p task remember))
      r)))

(defmethod actions/handle! :task.decide-many
  [_ world p args opts]
  (authority/require-person! (act/db world) p opts)
  (when-not (or (seq (:tasks args)) (:filter args)) (fail! :invalid-args "name `tasks` or a `filter`"))
  {:ok {:result (gates/decide-many! world p args)}})

(defmethod actions/handle! :approval.rule.add
  [_ world p {:keys [principal action match until uses note]} opts]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (when-let [why (rules/valid-match action match)] (fail! :invalid-args why))
    (let [target (writable-principal! world db p principal)
          rule (rules/add! world p target {:action action :match match :until until :uses uses :note note})]
      {:ok {:result {:principal target :rule rule} :object target}})))

(defmethod actions/handle! :approval.rule.remove
  [_ world p {:keys [principal rule]} opts]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (let [target (writable-principal! world db p principal)]
      (if (rules/remove! world target rule)
        {:ok {:result {:principal target :removed rule} :object target}}
        (fail! :conflict (str "no rule " rule " on that Principal"))))))

(defmethod actions/handle! :approval.rule.list
  [_ world p {:keys [principal] :as args} {:keys [message] :as opts}]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (let [who (if principal
                [(writable-principal! world db p principal)]
                (->> (objects/objects-of-class db :principal)
                     (filter #(seq (rules/rules db %)))
                     (sort-by str)))
          columns [{:key "principal" :label "principal"} {:key "rule" :label "rule"} {:key "action" :label "action"}
                   {:key "match" :label "match"} {:key "used" :label "used" :align "right"}
                   {:key "left" :label "uses left" :align "right"} {:key "until" :label "until"} {:key "by" :label "by"}]
          rows (vec (for [t who r (rules/rules db t)]
                      [(name-of db t) (:id r) (:action r) (pr-str (:match r)) (or (:used r) 0)
                       (if (:uses-left r) (:uses-left r) "-") (or (:until r) "-") (name-of db (->uuid (:by r)))]))
          {:keys [to route]} (audience db p args opts)
          presented (when (and route (seq to))
                      (let [{:keys [ok]} (view/present! world p {:to to :route route :kind :table
                                                                 :title (str (count rows) " approval rule" (when (not= 1 (count rows)) "s"))
                                                                 :data {:columns columns :rows rows} :about (vec who)
                                                                 :in-reply-to message :via (:via opts)})]
                        ok))]
      {:ok {:result (cond-> {:columns (mapv :label columns) :rows rows :count (count rows)}
                      presented (assoc :view (:serialization presented) :message (:message presented)))}})))

;; ---------------------------------------------------------------- notifications, restart

(defmethod actions/handle! :gateway.create [_ world p args opts]
  (authority/require-person! (act/db world) p opts)
  (let [r (gateway/create! world args)]
    (bind-world! world (objects/root-id (act/db world)) (:name args) (:gateway r))
    {:ok {:result r :object (:gateway r)}}))
(defmethod actions/handle! :gateway.bind [_ world p args opts]
  (authority/require-person! (act/db world) p opts)
  {:ok {:result (gateway/bind! world p args)}})
(defmethod actions/handle! :gateway.unbind [_ world p args opts]
  (authority/require-person! (act/db world) p opts)
  {:ok {:result (gateway/unbind! world p args)}})
(defmethod actions/handle! :gateway.enable [_ world p args opts]
  (authority/require-person! (act/db world) p opts)
  {:ok {:result (gateway/enable! world p args)}})
(defmethod actions/handle! :gateway.list [_ world p _ _] {:ok {:result (gateway/list-of world p)}})
(defmethod actions/handle! :gateway.send [_ world p args opts]
  {:ok {:result (gateway/send-out! world p args opts)}})
(defmethod actions/handle! :speech.transcribe [_ world p args opts]
  (authority/require-person! (act/db world) p opts)
  {:ok {:result (speech/transcribe! world p args)}})
(defmethod actions/handle! :speech.say [_ world p args opts]
  (authority/require-person! (act/db world) p opts)
  {:ok {:result (speech/say! world p args)}})
(defmethod actions/handle! :notify [_ world p args _] (notify/send! world p args))
(defmethod actions/handle! :notify.list [_ world p args _] (notify/list-of world p args))
(defmethod actions/handle! :notify.read [_ world p args _] (notify/mark-read! world p args))

(defmethod actions/handle! :path.complete [_ world p {:keys [mount path]} _]
  ;; a file dialog asks on every key, so nothing here materializes an Object
  (let [db (act/db world)
        id (or (->uuid mount) (resolve/lookup db p (str mount)))
        locator (when id (field db id :adapter/locator))
        path (str path)
        cut (inc (str/last-index-of (str "/" path) "/"))
        dir (subs path 0 (dec cut))
        stem (subs path (dec cut))]
    (cond
      (not (and locator (= :filesystem (field db id :adapter/binds)) (resolve/resolves? db p (str "/" id) :read)))
      act/unresolved
      :else
      (let [hidden? (str/starts-with? stem ".")
            low (str/lower-case stem)
            entries (->> (:file/entries (fs/describe locator (if (str/blank? dir) "." dir)))
                         (filter #(str/starts-with? (str/lower-case %) low))
                         (filter #(or hidden? (not (str/starts-with? % ".")))))]
        {:ok {:result {:mount id :dir dir :stem stem :entries (vec entries)}}}))))

(defmethod actions/handle! :conversation.compressor [_ world p {:keys [principal]} _]
  (let [db (act/db world)
        id (writable-principal! world db p principal)]
    (act/set-field! world p {:object id :field :serialize/compressor :type :string :reader :both
                             :value @conversation/shipped-compressor})))

(defmethod actions/handle! :doc.show [_ world p args _] (manual/show world p args))
(defmethod actions/handle! :doc.candidates [_ world _ args _]
  {:ok {:result (manual/candidates (act/db world) args)}})
(defmethod actions/handle! :doc.overview [_ world p args _] (manual/overview world p args))
(defmethod actions/handle! :doc.contents [_ world p args _] (manual/contents world p args))
(defmethod actions/handle! :field.diff [_ world p args _] (changes/diff! world p args))

(defmethod actions/handle! :release.make [_ world _ args _] (release/make! world args))

(defmethod actions/preflight! :world.restart [_ world _ args] (release/refusal world args))

(defmethod actions/handle! :world.restart
  [_ world p {:keys [release] :as args} {:keys [via session?] :as opts}]
  ;; a socket reaches here without the gate, so the rule holds here too: a new kernel takes a person
  (let [db (act/db world)
        wrapped {:action "world.restart" :args args}]
    (or (release/refusal world args)
        (when (and (authority/new-release? db :act wrapped)
                   (not (authority/by-a-person? db p session? via)))
          (act/err :person-only "a new release takes a person's own approval"))
        (release/request! world p {:release release :reason (:reason args)} opts))))

(defmethod actions/handle! :approval.unsafe
  [_ world p {:keys [principal] :as args} opts]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (let [target (writable-principal! world db p principal)]
      (when-not (= :process (:principal/driven-by (objects/entity db target)))
        (fail! :invalid-args "unsafe mode is for a process Principal: a person has no gate"))
      (unsafe/arm! world p target args))))

(defmethod actions/handle! :approval.unsafe.off
  [_ world p {:keys [principal]} opts]
  (let [db (act/db world)
        id (->uuid principal)]
    (authority/require-person! db p opts)
    ;; world/unsafe lists Adapters out of their box and shared terminals too, and a client ends each
    ;; entry the same way
    (cond
      (and id (= :adapter (:object/class (objects/entity db id))))
      (unsafe/reconfine! world p (writable-adapter! world db p principal))
      (and id (authority/person-terminal? db id))
      (unsafe/unshare! world p id nil)
      :else (unsafe/disarm! world p (writable-principal! world db p principal)))))

(defmethod actions/handle! :adapter.unconfine
  [_ world p {:keys [adapter] :as args} {:keys [via session?]}]
  (let [db (act/db world)
        a (writable-adapter! world db p adapter)]
    (when-not (authority/by-a-person? db p session? via)
      (fail! :person-only "only a person lifts a sandbox, from their own session or on a card"))
    (when-not (= :host (field db a :adapter/binds))
      (fail! :invalid-args "only a shell Adapter runs in a sandbox"))
    ;; on a card the person who decided is the one who lifted it
    (unsafe/unconfine! world (if session? p (field db via :task/decided-by)) a args)))

(defmethod actions/handle! :adapter.confine
  [_ world p {:keys [adapter network read]} opts]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (let [a (writable-adapter! world db p adapter)
          put! (fn [k type v] (act/append! world (log/pending :field-set a p {:name k :type type :value v :reader :both})))]
      (when network (put! :adapter/network :string network))
      (when read (put! :adapter/read [:list :string] (vec read)))
      {:ok {:result {:adapter a :network (or network (field (act/db world) a :adapter/network) "model")
                     :read (vec (or read (field (act/db world) a :adapter/read) []))}
            :object a}})))

(defmethod actions/handle! :approval.rule.test
  [_ world p {:keys [principal rule match cmd adapter cwd]} _]
  (let [db (act/db world)
        target (or (->uuid principal) (resolve/lookup db p (str principal)))
        host (or (when adapter (adapter-by db p adapter))
                 (some #(when (= :host (field db % :adapter/binds)) %) (field db target :principal/adapters)))
        root (when host (field db host :adapter/locator))
        candidates (cond match [{:match match}]
                         rule (filter #(= rule (:id %)) (rules/rules db target))
                         :else (rules/rules db target))]
    (cond
      (nil? root) (fail! :invalid-args "no shell Adapter to test against: give `adapter`")
      (empty? candidates) {:ok {:result {:matches false :reason "no rule to test"}}}
      :else
      (let [results (map #(assoc (rules/match-command (:match %) {:cmd cmd :root root :cwd cwd}) :rule (:id %)) candidates)]
        {:ok {:result (or (first (filter :matches results)) (first results))}}))))

(defmethod actions/handle! :task.deny
  [_ world p {:keys [task reason]} _]
  (gates/deny! world p {:task task :reason reason}))


;; ---------------------------------------------------------------- the Model catalogue, cost, search

;; ---------------------------------------------------------------- choosing a Model

(defn- model-object!
  "The Model v names, readable by p, or throws."
  [world db p v]
  (let [id (or (->uuid v) (resolve/lookup db p (str v)))]
    (when-not (and id (= :model (:object/class (objects/entity db id)))
                   (resolve/resolves? db p (act/id-name id) :read))
      (fail! :invalid-args "that is not a Model you can read"))
    id))

(defn- evaluate-grant!
  "target may evaluate on model: a person chose it, and a person holds no :delegate, so the kernel
   grants as root, as it does for a card's decider."
  [world db target model]
  (when-not (resolve/resolves? db target (act/id-name model) :evaluate)
    (ok! (act/grant! world (gates/root-of db) {:grantee target :scope #{model} :ops #{:read :evaluate} :delegable false}))))

(defmethod actions/handle! :model.choose
  [_ world p {:keys [principal model id thread fallback clear]} opts]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (let [target (writable-principal! world db p principal)
          where (if thread (->uuid thread) target)
          k (cond thread :conversation/model fallback :principal/model-fallback :else :principal/model-choice)]
      (when-not (and where (objects/exists? db where)) (fail! :invalid-args "no such conversation"))
      (if clear
        (let [seq-no (act/append! world (log/pending :field-set where p {:name k :type :string :value "" :reader :both :cleared true}))]
          {:ok {:result {:principal target :field k :cleared true} :object where :seq seq-no}})
        (let [m (model-object! world db p model)
              known (field db m :model/available)]
          (when (and id (seq known) (not (some #{id} known)))
            (fail! :invalid-args (str id " is not among that Model's model/available")))
          (evaluate-grant! world db target m)
          (ok! (act/set-field! world p {:object where :field k :type :map :reader :both
                                        :value (cond-> {:model (str m)} id (assoc :id id))}))
          (let [mine (vec (field db target :principal/models))]
            (when-not (some #{m} mine)
              (ok! (act/set-field! world p {:object target :field :principal/models :type [:list [:ref :model]]
                                            :value (conj mine m) :reader :both}))))
          {:ok {:result {:principal target :model m :id id :field k} :object where}})))))

;; ---------------------------------------------------------------- Sign in with ChatGPT

(defmethod actions/handle! :model.chosen
  [_ world p {:keys [principal thread]} _]
  (let [db (act/db world)
        target (or (->uuid principal) (resolve/lookup db p (str principal)))
        _ (when-not (and target (resolve/resolves? db p (act/id-name target) :read))
            (fail! :invalid-args "that is not a Principal you can read"))
        state (objects/state db target)
        {:keys [model model-id fell-back-from source]} (models/for-wake db state (->uuid thread))
        fallback (get-in state [:object/fields :principal/model-fallback :field/value])]
    {:ok {:result (cond-> {:principal target :source source}
                    model (assoc :model model :name (models/label-of db model)
                                 :id (or model-id (field db model :model/id))
                                 :compress-at (conversation/compress-at db target (->uuid thread))
                                 :status (field db model :model/status)
                                 :plan (= :chatgpt-plan (field db model :model/api)))
                    (= :chatgpt-plan (some->> model (#(field db % :model/api))))
                    (assoc :signed-in (chatgpt/signed-in? (:data-dir world) model) :usage-url chatgpt/usage-url)
                    (models/window db model (or model-id (when model (field db model :model/id))))
                    (assoc :window (models/window db model (or model-id (field db model :model/id))))
                    fell-back-from (assoc :fell-back-from (models/label-of db fell-back-from))
                    fallback (assoc :fallback fallback))}}))

(defmethod actions/handle! :model.signin.begin
  [_ world p {:keys [port model]} opts]
  (authority/require-person! (act/db world) p opts)
  (let [db (act/db world)
        m (when model (model-object! world db p model))]
    {:ok {:result (chatgpt/begin! (:data-dir world) p {:port port :model m})}}))

(defn- plan-model-name
  "A free name for a new plan-backed Model."
  [db world-id]
  (first (remove #(objects/lookup db world-id %) (cons "chatgpt" (map #(str "chatgpt-" %) (iterate inc 2))))))

(defmethod actions/handle! :model.signin.finish
  [_ world p args opts]
  (authority/require-person! (act/db world) p opts)
  (let [{:keys [ok err]} (chatgpt/finish! p args)]
    (when err (throw (ex-info (:message err) {:err err})))
    (let [db (act/db world)
          existing (:model ok)
          known (when existing (chatgpt/read-record (:data-dir world) existing))]
      (when (and known (:subject known) (not= (:subject known) (:subject ok)))
        (fail! :sign-in-failed "that is a different ChatGPT account from this Model's: add it as a new Model"))
      (let [nm (when-not existing (plan-model-name db (objects/world-id db)))
            model (or existing
                      (let [{:keys [object]} (ok! (act/create! world p :model {:kind :model :format :openai-chat :residency :local
                                                                               :base-url chatgpt/api-base :model-id "chatgpt"
                                                                               :name nm :label "ChatGPT plan" :timeout-s 600
                                                                               :reasoning "off"}))]
                        (bind-world! world p nm object)
                        object))]
        (chatgpt/store! (:data-dir world) model ok)
        (let [probe (models/adopt-plan! world model)]
          {:ok {:result (cond-> {:model model :name (or nm (models/label-of (act/db world) model))
                                 :status (:status probe) :models (vec (or (:models probe) []))
                                 :account (:account ok) :first (nil? known)
                                 :usage-url chatgpt/usage-url}
                          (:error probe) (assoc :error (:error probe)))
                :object model}})))))

(defmethod actions/handle! :model.signout
  [_ world p {:keys [model]} opts]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (let [m (model-object! world db p model)
          r (chatgpt/sign-out! (:data-dir world) m)]
      (models/probe! world m)
      {:ok {:result (assoc (or r {:revoked false}) :model m) :object m}})))

(defmethod actions/handle! :model.create
  [_ world p {:keys [name url model-id key-env reasoning price-in price-out context-tokens label images stt tts voice] :as args} _]
  (when (models/secret-in-args? args)
    (fail! :secret-in-args "never put a credential in args: key-env names the environment variable that holds it"))
  (when-not (re-find #"^https?://" (str url)) (fail! :invalid-args "url is an http(s) base url ending in /v1"))
  (let [{:keys [object seq]} (ok! (act/create! world p :model (cond-> {:kind :model :format :openai-chat :residency :tenant
                                                                         :base-url url :model-id model-id :name name
                                                                         :label (or label name) :timeout-s 600 :max-tokens 16384
                                                                         :reasoning (or reasoning "auto")}
                                                                  key-env (assoc :key-env key-env)
                                                                  (number? price-in) (assoc :price-in price-in)
                                                                  (number? price-out) (assoc :price-out price-out)
                                                                  (number? context-tokens) (assoc :context-tokens context-tokens))))
        _ (bind-world! world p name object)
        _ (doseq [[k v] {:model/stt stt :model/tts tts :model/voice voice} :when v]
            (ok! (act/set-field! world p {:object object :field k :type :string :value v :reader :both})))
        _ (when (true? images) (ok! (act/set-field! world p {:object object :field :model/images :type :boolean :value true :reader :both})))
        probe (models/probe! world object)]
    {:ok {:result (cond-> {:model object :name name :status (:status probe) :models (vec (or (:models probe) []))}
                    key-env (assoc :key-env key-env)
                    (:error probe) (assoc :error (:error probe)))
          :object object :seq seq}}))

(defmethod actions/handle! :model.probe
  [_ world p {:keys [model]} _]
  (let [db (act/db world)
        e (or (->uuid model) (resolve/lookup db p (str model)))]
    (when-not (and e (= :model (:object/class (objects/state db e)))) (fail! :unresolved (str "no Model " model)))
    (when-not (resolve/resolve! world db p (act/id-name e) :write) (fail! :unresolved "you do not write that Model"))
    (let [r (models/probe! world e)]
      {:ok {:result (cond-> {:model e :status (:status r) :models (vec (or (:models r) [])) :probed-at (java.util.Date.)} (:error r) (assoc :error (:error r)))
            :object e}})))

(defmethod actions/handle! :cost.report
  [_ world p {:keys [by since] :as args} {:keys [message] :as opts}]
  (let [db (act/db world)
        since (when (string? since) (try (java.util.Date/from (java.time.Instant/parse since)) (catch Exception _ nil)))
        {:keys [columns rows about count]} (cost/report db p {:by by :since since})
        title (str "cost by " (or by "principal") (when since (str " since " (.toInstant ^java.util.Date since))) ": " count " evaluations")
        r (present-view! world p :table title {:columns columns :rows rows} about opts message (audience db p args opts))]
    (cond-> r (:ok r) (assoc-in [:ok :result :evaluations] count))))

(defn- as-result [{:keys [ok err]}] (if err {:err err} {:ok {:result ok}}))

(defmethod actions/handle! :code.search [_ world p args _] (as-result (code/search! world p args)))

(defmethod actions/handle! :code.outline [_ world p args _] (as-result (code/outline! world p args)))

(defmethod actions/preflight! :file.edit [_ world p args]
  (not-empty (code/edit-refusal (act/db world) p args)))

(defmethod actions/handle! :file.edit
  [_ world p args {:keys [via]}]
  (as-result (code/edit! world p (assoc args :via via))))

(defmethod actions/handle! :search.find
  [_ world p args _]
  (let [{:keys [ok err]} (search/search! world p args)]
    (if err {:err err} {:ok {:result ok}})))


;; ---------------------------------------------------------------- credentials and pairing

(defmethod actions/handle! :token.issue
  [_ world p args opts]
  (auth/issue! world p args opts))

(defmethod actions/handle! :token.revoke
  [_ world p args _]
  (auth/revoke! world p args))

(defmethod actions/handle! :token.list
  [_ world p args {:keys [message] :as opts}]
  ;; the rows are always in the result; the table view goes out when there is an audience
  (let [db (act/db world)
        {:keys [columns rows about]} (auth/list-rows world p args)
        title (str (count rows) " credential" (when (not= 1 (count rows)) "s") (when (:principal args) (str " of " (:principal args))))
        {:keys [to route]} (audience db p args opts)
        presented (when (and route (seq to))
                    (let [{:keys [ok]} (view/present! world p {:to to :route route :kind :table :title title
                                                               :data {:columns columns :rows rows} :about about
                                                               :in-reply-to message :via (:via opts) :hop (when (:via opts) 1)})]
                      ok))]
    {:ok {:result (cond-> {:columns (mapv :label columns) :rows rows :count (count rows)}
                    presented (assoc :view (:serialization presented) :message (:message presented)))}}))

(defmethod actions/handle! :pair.begin
  [_ world p args opts]
  (auth/begin! world p args opts))

;; ---------------------------------------------------------------- other worlds

(defn- world-adapter
  "The world Adapter `world` names: an Adapter id or name, a `world.<name>` or `bridge.<name>`
   binding, or a bare name; nil when none."
  [db p v]
  (let [wid (objects/world-id db)
        n (str v)
        candidates (concat [(->uuid v) (resolve/lookup db p n)]
                           (map #(objects/lookup db wid %) [(str "world." n) n (str/replace n #"^bridge\." "world.")]))
        adapter? (fn [id] (and id (= :adapter (:object/class (objects/state db id))) (= :world (field db id :adapter/binds)) id))
        bridge? (fn [id] (when (and id (= :principal (:object/class (objects/state db id))))
                           (some adapter? (field db id :principal/adapters))))]
    (some (fn [id] (or (adapter? id) (bridge? id))) candidates)))

(defn- bridge-of
  "The bridge Principal whose principal/adapters names the Adapter, or nil."
  [db adapter]
  (some (fn [pr] (when (contains? (set (field db pr :principal/adapters)) adapter) pr))
        (sort-by str (objects/objects-of-class db :principal))))

(defn- shared-route
  "A Route whose route/members hold both a and b, or nil."
  [db a b]
  (some (fn [r] (when (and (contains? (set (field db r :route/members)) a) (contains? (set (field db r :route/members)) b)) r))
        (field db a :principal/routes)))

(defn- host-port [url] (second (re-find #"^wss?://([^/]+)" (str url))))

(defn this-world?
  "Is this name or URL the world itself? A world may link to another on the same host, on another port,
   so a URL counts only when host and port both match."
  [world remote url]
  (let [f (objects/fields (act/db world) (:world-id world))
        v (fn [k] (get-in f [k :field/value]))
        own-url (v :world/url)
        port (get-in world [:config :port])
        host (v :world/host)]
    (boolean (or (and (seq (str remote)) (contains? (set (keep identity [(v :world/name) host])) (str remote)))
                 (and url own-url (= (host-port url) (host-port own-url)))
                 (and url port (contains? (set (keep #(when % (str % ":" port)) [host "localhost" "127.0.0.1"])) (host-port url)))))))

(defmethod actions/handle! :world.link
  [_ world p {:keys [name url key-env token-file] :as args} {:keys [via]}]
  (when (models/secret-in-args? (dissoc args :token-file)) (fail! :secret-in-args "never put a credential in args: key-env names the environment variable, token-file the file"))
  (when-not (re-find #"^wss?://" (str url)) (fail! :invalid-args "url is the other world's ws endpoint, ws://host:port/ws"))
  (when (this-world? world name url) (fail! :conflict (str "that is this world: " name " is where you already run; use send on a Route, and a shell Adapter for local paths")))
  (when-not (or key-env token-file) (fail! :invalid-args "give key-env (an environment variable name) or token-file"))
  (let [db (act/db world)
        wid (:world-id world)
        aname (str "world." name)
        bname (str "bridge." name)
        _ (when (or (objects/lookup db wid aname) (objects/lookup db wid bname)) (fail! :conflict (str name " is already linked")))
        adapter (:object (ok! (act/create! world p :adapter {:binds :world :locator url :sync :both :kind :world :name aname})))
        _ (bind-world! world p aname adapter)
        _ (when key-env (ok! (act/set-field! world p {:object adapter :field :adapter/key-env :type :string :value key-env :reader :both :via via})))
        _ (when token-file (ok! (act/set-field! world p {:object adapter :field :adapter/token-file :type :string :value (str token-file) :reader :agent :via via})))
        ;; the bridge: a process Principal with no Model; its loop forwards (knowledge.world.loop.core)
        bridge (:object (ok! (act/create! world p :principal {:driven-by :process :name bname} {:return-token? false})))
        _ (bind-world! world p bname bridge)
        _ (ok! (act/set-field! world p {:object bridge :field :principal/adapters :type [:list [:ref :adapter]] :value [adapter] :reader :both :via via}))
        _ (ok! (act/set-field! world p {:object bridge :field :instructions/charter :type :string :reader :both :via via
                                        :value (str "You are the bridge to the world " name " at " url ". A Message to you on your Route is forwarded to root there and its answer relayed back; you do not evaluate.")}))
        _ (ok! (act/set-field! world p {:object bridge :field :loop/accepts-from :type [:list [:ref :principal]] :value [p] :reader :both :via via}))
        transport (transport-of db p)
        route-name (str (name-of db p) "." bname)
        r (:object (ok! (act/create! world p :route {:transport transport :address bname :members [p bridge] :name route-name})))
        _ (bind-world! world p route-name r)
        _ (append-refs! world p p :principal/routes :route [r] via)
        _ (append-refs! world p bridge :principal/routes :route [r] via)
        _ (ok! (act/grant! world p {:grantee bridge :scope #{r} :ops #{:read :transmit} :delegable false}))
        _ (ok! (act/grant! world p {:grantee bridge :scope #{adapter} :ops #{:read :evaluate} :delegable false}))
        {:keys [seq]} (ok! (act/grant! world p {:grantee bridge :scope #{bridge} :ops #{:read :write} :delegable false}))]
    {:ok {:result {:adapter adapter :bridge bridge :route r :name name :url url} :object adapter :seq seq}}))

(defn- relay!
  "The bridge sends the remote answer (or the failure) here, on its Route with `to`, in-reply-to
   what was being answered."
  [world bridge to r in-reply-to text]
  (let [{:keys [ok err]} (route/send! world bridge {:to #{to} :route r :text text :in-reply-to in-reply-to})]
    (when err (timbre/warn "bridge could not relay" {:bridge bridge :err err}))
    ok))

(defn relay-text
  "What the bridge says about a round trip."
  [name {:keys [reply err timed-out remote-principal]}]
  (cond err (str "[" name "] could not deliver: " (:code err) " " (:message err))
        timed-out (str "[" name "] no answer yet (sent as " (some-> remote-principal str (subs 0 8)) "); the remote side may still reply")
        :else (str (:text reply))))

(defmethod actions/handle! :world.send
  [_ world p {:keys [to route text timeout-s] remote :world} {:keys [message]}]
  (let [db (act/db world)
        _ (when (this-world? world remote nil) (fail! :conflict (str "that is this world: " remote " is where you already run; use send on a Route")))
        adapter (or (world-adapter db p remote) (fail! :unresolved (str "no world Adapter " remote ": world.link it first")))
        _ (when-not (resolve/resolve! world db p (act/id-name adapter) :evaluate) (fail! :unresolved "you do not evaluate on that world Adapter"))
        bridge (or (bridge-of db adapter) (fail! :unresolved "that world Adapter has no bridge Principal"))
        here (or (shared-route db p bridge) (fail! :unresolved "no Route between you and the bridge"))
        wname (str/replace (or (some (fn [[ns n]] (when (= ns (:world-id world)) n)) (objects/names db adapter)) "world") #"^world\." "")
        timeout-ms (* 1000 (long (or timeout-s 120)))]
    ;; the round trip runs off this thread: the bridge relays when the answer lands
    (.start (Thread/ofVirtual) ^Runnable
            (fn [] (let [r (wadapter/send-and-await! (act/db world) adapter {:to to :route route :text text :timeout-ms timeout-ms})]
                     (relay! world bridge p here message (relay-text wname r)))))
    {:ok {:result (cond-> {:adapter adapter :bridge bridge :route here :pending true} to (assoc :to to))}}))

(defn forward!
  "A bridge's wake (no Model, a world Adapter): forward the Message to root on the other world and
   relay the answer to the sender on the same Route. Returns the round trip map."
  [world bridge {:keys [message route]}]
  (let [db (act/db world)
        adapter (some (fn [a] (when (= :world (field db a :adapter/binds)) a)) (field db bridge :principal/adapters))
        sender (field db message :message/from)
        wname (str/replace (or (some (fn [[ns n]] (when (= ns (:world-id world)) n)) (objects/names db adapter)) "world") #"^world\." "")
        r (wadapter/send-and-await! db adapter {:text (str (field db message :message/text))})]
    (relay! world bridge sender route message (relay-text wname r))
    r))

;; ---------------------------------------------------------------- object.rename

(defn- prefix-grants
  "Grants scoped to a name prefix: they follow the name, not the Object, so a rename leaves them behind."
  [db n]
  (vec (for [g (objects/objects-of-class db :grant)
             :when (= n (:prefix (:grant/scope (objects/state db g))))]
         g)))

(defn- object-id
  "An id or a name the caller can see, as an Object id, or nil."
  [db p object]
  (or (->uuid object) (resolve/lookup db p (str object))))

(defn- protected-reason
  "Why this Object can never be retired, or nil."
  [db id]
  (cond (= id (objects/world-id db)) "the world cannot be retired"
        (= id (objects/root-id db)) "root cannot be retired"
        (= id (gates/first-person db)) "the first person cannot be retired"
        (= :grant (:object/class (objects/entity db id))) "a Grant is revoked, not retired"
        (some (fn [[_ n]] (str/starts-with? n "schema.")) (objects/names db id)) "a schema Object cannot be retired"))

(defn- dependents
  "Live Principals still using a Model, Adapter or Route: [id ...]."
  [db id]
  (let [k (case (:object/class (objects/entity db id))
            :model :principal/models :adapter :principal/adapters :route :principal/routes nil)]
    (when k
      (vec (sort-by str (for [p (objects/objects-with-field db k)
                              :when (and (not (objects/retired? db p))
                                         (contains? (set (get-in (objects/fields db p) [k :field/value])) id))]
                          p))))))

(defn- retire-refusal
  "err conflict when the Object may not be retired as asked, else nil."
  [db id force?]
  (let [why (protected-reason db id) deps (dependents db id)]
    (cond why (act/err :conflict why)
          (objects/retired? db id) (act/err :conflict "already retired")
          (and (seq deps) (not force?))
          (act/err :conflict (str "still used by " (str/join ", " (map #(or (first (sort (map second (objects/names db %)))) (str %)) deps))
                                  "; pass force to retire it anyway")))))

(defmethod actions/preflight! :object.retire [_ world p {:keys [object force]}]
  (let [db (act/db world)]
    (when-let [id (object-id db p object)]
      (when (objects/exists? db id) (retire-refusal db id force)))))

(defmethod actions/handle! :object.retire
  [_ world p {:keys [object reason force]} _]
  (let [db (act/db world)
        id (object-id db p object)
        {:keys [grant]} (when (and id (objects/exists? db id) (not (objects/retired? db id)))
                          (resolve/resolve! world db p (act/id-name id) :write))]
    (cond
      (and id (objects/retired? db id)) (act/err :conflict "already retired")
      (not grant) act/unresolved
      :else
      (or (retire-refusal db id force)
          (let [names (vec (sort (objects/names db id)))
                deps (dependents db id)]
            ;; a running wake ends first: its Evaluation is read by id afterwards like anything else
            (when (= :principal (:object/class (objects/entity db id)))
              (try (perform/perform! world p {:object id :action :cancel}) (catch Exception _ nil)))
            ;; names are unbound below; this is how a client still shows what it was called
            (act/append! world (log/pending :field-set id p {:name :object/last-names :type [:list :string]
                                                             :value (mapv second names) :reader :both}
                                            :delegation grant))
            (doseq [[ns n] names] (act/drop-name! world p grant ns id n))
            (let [seq-no (act/append! world (log/pending :retired id p (cond-> {:names names} reason (assoc :reason reason))
                                                         :delegation grant))]
              {:ok {:result (cond-> {:object id :retired true :names (mapv second names)}
                              (seq deps) (assoc :dependents deps))
                    :object id :seq seq-no}}))))))

(defmethod actions/handle! :object.restore
  [_ world p {:keys [object]} _]
  (let [db (act/db world)
        id (->uuid object)
        grant (when (and id (objects/retired? db id))
                (some-> (first (resolve/grants-for db p id (act/id-name id) :write (java.util.Date.))) :object/id))]
    (cond
      (and id (objects/exists? db id) (not (objects/retired? db id))) (act/err :conflict "that Object is not retired")
      (nil? grant) act/unresolved
      :else
      (let [names (->> (log/read-stream (:log world) id 0 100000)
                       (filter #(= :retired (:event/kind %))) last :event/body :names)
            free (filterv (fn [[ns n]] (nil? (objects/lookup db ns n))) names)
            taken (filterv (fn [[ns n]] (some? (objects/lookup db ns n))) names)
            seq-no (act/append! world (log/pending :restored id p {:names free} :delegation grant))]
        (doseq [[ns n] free] (act/restore-name! world p grant ns id n))
        {:ok {:result (cond-> {:object id :restored true :names (mapv second free)}
                        (seq taken) (assoc :names-taken (mapv second taken)))
              :object id :seq seq-no}}))))

(defmethod actions/handle! :field.clear
  [_ world p {:keys [object field]} ctx]
  (let [db (act/db world)
        id (object-id db p object)
        k (keyword (let [s (str field)] (if (str/starts-with? s ":") (subs s 1) s)))
        {:keys [grant]} (when (and id (objects/exists? db id)) (resolve/resolve! world db p (act/id-name id) :write))
        f (when grant (get (objects/fields db id) k))
        refused (authority/clear-refusal k (authority/person-session? db p (:session? ctx)))]
    (cond
      (nil? grant) act/unresolved
      (nil? f) (act/err :conflict (str "no Field " (subs (str k) 1) " on that Object"))
      refused refused
      :else
      (let [prev (:field/value f)
            seq-no (act/append! world (log/pending :field-set id p
                                                   (cond-> {:name k :type :string :value "" :reader (or (:field/reader f) :both) :cleared true}
                                                     (and (some? prev) (not (bytes? prev))) (assoc :previous prev))
                                                   :delegation grant))]
        {:ok {:result {:object id :field k :cleared true} :object id :seq seq-no}}))))

(defmethod actions/handle! :object.rename
  [_ world p {:keys [object] new-name :name} _]
  (let [db (act/db world)
        id (or (->uuid object) (resolve/lookup db p (str object)))
        {:keys [grant]} (when id (resolve/resolve! world db p (act/id-name id) :write))
        _ (when-not grant (fail! :unresolved "you cannot write that Object"))
        _ (when (or (str/blank? (str new-name)) (re-find #"\s" (str new-name)))
            (fail! :invalid-args "a name is one word"))
        ws (:world-id world)
        names (objects/names db id)
        was (or (some (fn [[ns n]] (when (= ns ws) n)) names) (first (sort (map second names))))
        _ (when-not was (fail! :conflict "that Object has no name to change: bind one"))
        spaces (vec (for [[ns n] names :when (= n was)] ns))
        _ (when (some #(some? (objects/lookup db % new-name)) (conj spaces ws))
            (fail! :conflict (str "the name " new-name " is taken")))
        holders (distinct (remove nil? [ws (:principal/namespace (objects/entity db id))]))
        related (vec (sort (distinct (for [ns holders [n _] (objects/bound-names db ns)
                                           :when (str/starts-with? n (str was "."))]
                                       n))))
        seq-no (reduce (fn [_ ns] (act/rebind! world p grant ns id was new-name)) nil spaces)]
    {:ok {:result {:object id :name new-name :was was :related related :prefix-grants (prefix-grants db was)}
          :object id :seq seq-no}}))

;; ---------------------------------------------------------------- the queue (decisions 2026-10-31)

(defn- queue-rights!
  "The Message's sender, or anyone who can write the Principal, moves a waiting wake."
  [world p principal message]
  (let [db (act/db world)]
    (when-not (or (= p (get-in (objects/fields db message) [:message/from :field/value]))
                  (resolve/resolves? db p (act/id-name principal) :write))
      (fail! :unresolved "only the Message's sender, or someone who can write the Principal, moves it"))))

(defmethod actions/handle! :loop.dequeue
  [_ world p {:keys [principal message]} _]
  (let [principal (->uuid principal) message (->uuid message)]
    (queue-rights! world p principal message)
    (if (queue/dequeue! world (act/db world) principal message)
      {:ok {:result {:principal principal :message message :dequeued true}
            :object message
            :seq (log/append!! (:log world) (log/pending :field-set message (:world-id world)
                                                         {:name :message/dequeued :type :boolean :value true :reader :both}))}}
      (act/err :conflict "that Message is not waiting: it has started, or was never queued"))))

(defmethod actions/handle! :loop.reorder
  [_ world p {:keys [principal message position]} _]
  (let [principal (->uuid principal) message (->uuid message)]
    (queue-rights! world p principal message)
    (if (queue/reorder! world (act/db world) principal message position)
      {:ok {:result {:principal principal
                     :queue (mapv (comp str :message) (queue/waiting world principal))}
            :object principal}}
      (act/err :conflict "that Message is not waiting: it has started, or was never queued"))))
