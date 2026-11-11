(ns knowledge.world.boot.kernel-zero
  "Kernel-zero boot (spec 17.3, 5.9, 4.3). On an empty log the world creates its first objects:
   the world Principal (driven-by :none, the actor of boot events; its id is the world id), the
   root Principal with the root Grant (scope :everything, every op, delegable, grantor world),
   one Object per schema in the registry (fields schema/name, schema/key, schema/form and
   instructions where shipped), the shipped instruction Fields on root, and data/root.token.
   Idempotent: a non-empty log means boot already happened.
   Then `run-steps!` runs the six steps of 17.3 as root, through the same act, serialize and
   evaluate functions a client reaches over the wire (ground rule 4), plus step 7 (decisions
   2026-10-30 'root knows the harness'): the repository mounted as `harness`, a shell Adapter
   `shell.local`, harness/index and harness/head on root, root's first read through the mount and
   first Task; step 4 creates root.heartbeat, an interval Trigger root owns, armed when
   root's policy/heartbeat-s is positive. Step 5 is answered by the resident loop."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.walk :as walk]
            [knowledge.schema.core :as c]
            [knowledge.world.kinds :as kinds]
            [knowledge.schema.objects :as schema-objects]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates-defaults :as gates-defaults]
            [knowledge.world.act.handlers]
            [knowledge.world.act.perform :as perform]
            [knowledge.world.authority :as authority]
            [knowledge.world.models :as models]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.memory :as memory]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.tokens :as tokens]
            [taoensso.timbre :as timbre])
  (:import [java.nio.file Files]
           [java.nio.file.attribute PosixFilePermissions]))

(def all-ops (set (rest c/Op)))

(def instruction-files
  {:instructions/charter "charter.md"
   :instructions/ethos "ethos.md"
   :instructions/ground-rules "ground-rules.md"
   :instructions/protocol "protocol.md"
   :instructions/harness "harness.md"
   ;; the charter a developer Principal is created with (root fills the brackets)
   :instructions/charter-template/developer "charter-template-developer.md"})

(defn bound-name
  "World-namespace name of a schema Object: :ke/Event -> schema.Event, :ke.args/attach -> schema.args.attach."
  [k]
  (let [ns (namespace k)]
    (str "schema." (when (not= ns "ke") (str (subs ns 3) ".")) (name k))))

(defn form-string
  "The schema form as EDN text; function-valued properties (e.g. :dispatch) print as their name."
  [form]
  (pr-str (walk/postwalk #(if (fn? %) (symbol (.getName (class %))) %) form)))

(defn shipped-instruction
  "The text of a shipped instruction resource (kernel-zero and principal.create ship them)."
  [file]
  (some-> (io/resource (str "knowledge/world/boot/instructions/" file)) slurp))

(defn write-root-token!
  "data/root.token, mode 0600. Returns the token."
  [data-dir]
  (let [token (session/new-token)
        f (io/file data-dir "root.token")]
    (spit f token)
    (try (Files/setPosixFilePermissions (.toPath f) (PosixFilePermissions/fromString "rw-------"))
         (catch Exception _ nil))
    token))

(defn kernel-zero!
  "Create the kernel-zero objects if the log is empty. Returns {:root id :root-grant id} or nil."
  [{:keys [log world-id data-dir]}]
  (when (zero? (log/head log))
    (timbre/info "kernel-zero: empty log, creating the first objects" {:world world-id})
    (let [world world-id
          root (random-uuid)
          root-grant (random-uuid)
          ap (fn [e] (log/append!! log e))]
      ;; the world Principal: driven by nothing, the actor of boot
      (ap (log/pending :created world world {:class :principal :fields {:principal/driven-by :none :principal/namespace world}}))
      ;; root and the root Grant (spec 5.9)
      (ap (log/pending :created root world {:class :principal :fields {:principal/driven-by :process :principal/namespace root}}))
      (ap (log/pending :created root-grant world {:class :grant
                                                   :fields {:grant/grantor world :grant/grantee root :grant/scope :everything
                                                            :grant/ops all-ops :grant/delegable true}}))
      (ap (log/pending :granted root-grant world {:grant root-grant :grantee root :ops all-ops :scope :everything :delegable true}
                       :streams #{world root}))
      (ap (log/pending :bound world world {:namespace world :name "world" :object world}))
      (ap (log/pending :bound world world {:namespace world :name "root" :object root} :streams #{root}))
      ;; root is found by this Field, never by its name, so root can be renamed (decisions 2026-10-31)
      (ap (log/pending :field-set world world {:name :world/root :type [:ref :principal] :value root :reader :both}))
      ;; schemas are Objects (spec 4.3, principle 7)
      (doseq [{:keys [key form instruction]} (schema-objects/schema-objects)]
        (let [id (random-uuid)
              n (bound-name key)]
          (ap (log/pending :created id world {:class :object
                                              :fields (cond-> {:schema/name n :schema/key key :schema/form (form-string form)}
                                                        instruction (assoc :instructions instruction)
                                                        true (merge (kinds/fields-for key)))}))
          (ap (log/pending :bound world world {:namespace world :name n :object id} :streams #{id}))))
      ;; shipped instruction Fields on root
      (doseq [[k file] instruction-files]
        (when-let [text (shipped-instruction file)]
          (ap (log/pending :field-set root world {:name k :type :string :value text :reader :both}))))
      ;; root's credential: the file delivers it once, the hash lives on root, visible under the root Grant
      (let [token (write-root-token! data-dir)]
        (ap (log/pending :field-set root world {:name :principal/token-hash :type :string
                                                :value (session/token-hash token) :reader :agent
                                                :visibility root-grant}))
        ;; the credential as an Object (data/root.token)
        (tokens/record! {:log log :world-id world} {:principal root :secret token :label "root.token" :issued-by world}))
      (timbre/info "kernel-zero: done" {:head (log/head log) :root root})
      {:root root :root-grant root-grant})))

(defn remint-tokens!
  "Credentials for a world that has none on disk (a fork or an import, ; tokens never
   travel with a log): for every Principal carrying principal/token-hash whose token file is
   missing, mint a token, write data/<name>.token (root.token for root), and set the hash on the log
   with the world as actor. Returns the names re-minted."
  [{:keys [log world-id data-dir] :as world}]
  (let [db (act/db world)
        names (objects/bound-names db world-id)
        root (objects/root-id db)]
    (vec (for [p (objects/objects-with-field db :principal/token-hash)
               :let [n (or (some (fn [[nm id]] (when (= id p) nm)) names) (str p))
                     ;; root.token names the role, whatever root is called now
                     f (io/file data-dir (if (= p root) "root.token" (act/token-file-name n)))]
               :when (not (.exists f))]
           (let [token (session/new-token)]
             (spit f token)
             (try (Files/setPosixFilePermissions (.toPath f) (PosixFilePermissions/fromString "rw-------"))
                  (catch Exception _ nil))
             ;; the parent's or the exporter's credentials must not open this world
             (tokens/revoke-all! world p world-id "re-minted: fork or import")
             (log/append!! log (log/pending :field-set p world-id {:name :principal/token-hash :type :string
                                                                   :value (session/token-hash token) :reader :agent}))
             (tokens/record! world {:principal p :secret token :label (str n " (re-minted)") :issued-by world-id})
             (timbre/info "token re-minted" {:principal n :file (str f)})
             n)))))

(defn world-identity!
  "world/name and world/url on the world Principal: from KE_WORLD_NAME and
   KE_PUBLIC_URL, else the host name and ws://<host>:<port>/ws; written by the world when missing or
   changed, so `hello` and a pairing code can say where this world is."
  [{:keys [log world-id config port] :as world}]
  (let [host (or (try (.getHostName (java.net.InetAddress/getLocalHost)) (catch Exception _ nil)) "localhost")
        name (or (:world-name config) (first (str/split host #"\.")))
        ;; an ephemeral port (0 in the config: tests) is not a stable address; hello computes it live
        stable? (boolean (or (:public-url config) (pos? (long (or (:port config) 0)))))
        url (or (:public-url config) (str "ws://" host ":" port "/ws"))
        fields (objects/fields (act/db world) world-id)]
    (doseq [[k v] (cond-> {:world/name name :world/host (first (str/split host #"\."))} stable? (assoc :world/url url))
            :when (not= v (get-in fields [k :field/value]))]
      (log/append!! log (log/pending :field-set world-id world-id {:name k :type :string :value v :reader :both})))
    {:name name :url url}))

(defn mark-fork!
  "A fork's world Principal says so: world/parent (the parent world's id, which this
   fork shares as its own), world/forked-at (the seq), world/fork-id. Written once, by the world."
  [{:keys [log world-id fork] :as world}]
  (let [fields (objects/fields (act/db world) world-id)
        at (long (get-in fork [:parent :seq]))]
    (when (not= at (get-in fields [:world/forked-at :field/value]))
      (log/append!! log (log/pending :field-set world-id world-id {:name :world/parent :type [:ref :principal]
                                                                   :value (or (get-in fork [:parent :world]) world-id) :reader :both}))
      (log/append!! log (log/pending :field-set world-id world-id {:name :world/forked-at :type :number :value at :reader :both}))
      (log/append!! log (log/pending :field-set world-id world-id {:name :world/fork-id :type :string
                                                                   :value (str (or (:id fork) (random-uuid))) :reader :both}))
      (timbre/info "fork marked" {:forked-at at}))))

(defmulti step!
  "One of the six kernel-zero steps (spec 17.3), run as root's first acts:
   :name-itself :read-and-write-instructions :bind-model-and-evaluate :arm-heartbeat
   :answer-listener :first-person."
  (fn [_world k] k))

(defmethod step! :default [_ k]
  (throw (ex-info "kernel-zero step not implemented" {:step k})))

(def steps [:name-itself :actions :read-and-write-instructions :bind-model-and-evaluate :arm-heartbeat :answer-listener :first-person :notifications :manual :know-the-harness :memory-defaults :approval-defaults :root-leash :working-copy])

(defn- ok!
  "Unwrap an act result or throw with the step."
  [step {:keys [ok err]}]
  (if err (throw (ex-info (str "kernel-zero step " (name step) " failed: " (:message err)) {:step step :err err})) ok))

(defn root-of [world] (objects/root-id (act/db world)))

(defn- root-name [world n] (objects/lookup (act/db world) (root-of world) n))

(defn model-args
  "The Model from KE_MODEL_URL / KE_MODEL_ID (spec 11.5): residency from KE_MODEL_RESIDENCY,
   default :external; the key is read from the environment variable model/key-env names, never stored.
   Without a url: a :stub Model."
  [{:keys [model-url model-id model-residency]}]
  (if model-url
    {:kind :model :format :openai-chat :residency (or (some-> model-residency str/lower-case keyword) :external) :base-url model-url
     :model-id (or model-id "default") :key-env "KE_MODEL_KEY" :name "root.model" :label "root.model"
     :timeout-s 600 :max-tokens 16384 :reasoning :auto}
    {:kind :model :format :openai-chat :residency :local :model-id "stub" :name "root.model" :label "root.model"
     :timeout-s 600 :max-tokens 16384 :reasoning :auto}))

(defmethod step! :name-itself [{:keys [world-id config] :as world} step]
  ;; 17.3 step 1: bind root, root.workspace, root.model, root.route; the listener transport Adapter
  (let [root (root-of world)
        create (fn [class args] (:object (ok! step (act/create! world root class args))))
        ws (create :principal {:driven-by :none :name "root.workspace"})
        adapter (create :adapter {:binds :transport :locator "listener://" :sync :both})
        model (create :model (model-args config))
        route (create :route {:transport adapter :address "root" :name "root.route"
                              :template (shipped-instruction "route-template.md")})]
    (ok! step (act/bind! world root {:namespace root :name "root" :object root}))
    (ok! step (act/bind! world root {:namespace world-id :name "transport.listener" :object adapter}))
    (ok! step (act/bind! world root {:namespace world-id :name "root.route" :object route}))
    (ok! step (act/set-field! world root {:object root :field :principal/workspaces :type [:list [:ref :principal]] :value [ws] :reader :both}))
    (ok! step (act/set-field! world root {:object root :field :principal/models :type [:list [:ref :model]] :value [model] :reader :both}))
    (ok! step (act/set-field! world root {:object root :field :principal/routes :type [:list [:ref :route]] :value [route] :reader :both}))
    (ok! step (act/set-field! world root {:object ws :field :workspace/holder :type [:ref :principal] :value root :reader :both}))
    {:workspace ws :model model :route route :adapter adapter}))

(defmethod step! :actions [world _]
  ;; plan §19.1 W3 (decisions 2026-10-30 'named actions'): the shipped action types as Objects bound
  ;; action/<name> in the world namespace, created by root; idempotent, so an older world gains the
  ;; ones it lacks at start
  {:created (actions/ensure! world (root-of world))})

(defmethod step! :read-and-write-instructions [{:keys [world-id] :as world} step]
  ;; 17.3 step 2: read the shipped instruction Fields (one field-read each, on root's stream), write instructions/own
  (let [root (root-of world)
        db (act/db world)
        mine (ok! step (act/read! world root {:object root}))
        schema-reads (for [k (keys schema-objects/instructions)
                           :let [id (objects/lookup db world-id (bound-name k))]
                           :when id]
                       (ok! step (act/read! world root {:object id :field :instructions})))
        read-names (concat (filter #(= "instructions" (namespace %)) (keys (:object/fields mine)))
                           (map #(bound-name %) (keys schema-objects/instructions)))
        route (root-name world "root.route")
        model (root-name world "root.model")
        own (str "I am root, the first resident of this world, a Principal driven by process. "
                 "I hold the root Grant (scope everything, every op, delegable) and every other Grant descends from it.\n"
                 "I serve one Route, root.route [[" route "]], address root, transport listener: a Message delivered on it "
                 "wakes my loop, which serializes the Message, its sender, the Route's input tags and my own instruction Fields "
                 "for my Model root.model [[" model "]] and replies on the same Route, citing the Fields it read.\n"
                 "Before writing this I read: " (str/join ", " (map str read-names)) ".")]
    (doall schema-reads)
    (ok! step (act/set-field! world root {:object root :field :instructions/own :type :string :value own :reader :agent}))))

(def boot-max-tokens "The boot Evaluation's bound." 256)

(defmethod step! :bind-model-and-evaluate [world step]
  ;; 17.3 step 3: one Evaluation whose Serialization is root's own instruction Fields; not awaited
  (let [root (root-of world)
        model (root-name world "root.model")]
    ;; it proves the Model answers, no more: on a real model an unbounded call ran 16,384 tokens and
    ;; about 5 minutes alongside a person's first question
    (select-keys (ok! step (evaluate/evaluate! world {:principal root :working-set #{root} :model model :policy :one-shot
                                                      :reasoning :off :max-tokens boot-max-tokens}))
                 [:evaluation :serialization])))

(def heartbeat-default-s
  "trigger/every-s of root.heartbeat when policy/heartbeat-s is 0 (off): the cadence it would run at once armed."
  60)

(defn heartbeat-of
  "root.heartbeat, the Trigger bound in the world namespace, or nil."
  [world]
  (objects/lookup (act/db world) (:world-id world) "root.heartbeat"))

(defmethod step! :arm-heartbeat [{:keys [world-id] :as world} step]
  ;; 17.3 step 4: root's heartbeat is a Trigger of kind interval it owns, bound
  ;; root.heartbeat. Its cadence is root's Field policy/heartbeat-s: 0 (the shipped default) keeps it
  ;; disarmed with every-s 60 ready; a positive value arms it at that cadence. The test arms a 1 s
  ;; heartbeat through the arm op and watches it fire; production turns it on by setting the Field
  ;; and arming. Idempotent: an existing root.heartbeat is kept.
  (let [root (root-of world)
        db (act/db world)
        policy (get-in (objects/fields db root) [:policy/heartbeat-s :field/value])
        every (if (and (number? policy) (pos? policy)) (long policy) heartbeat-default-s)
        armed? (and (number? policy) (pos? policy))]
    (when-not (number? policy)
      (ok! step (act/set-field! world root {:object root :field :policy/heartbeat-s :type :number :value 0 :reader :both})))
    (let [trigger (or (heartbeat-of world)
                      (let [t (:object (ok! step (act/create! world root :trigger {:kind :interval :every-s every :armed armed?
                                                                                     :text "heartbeat: check pending Tasks, memory/notes against memory/budget, and loop/stalled of the Principals you created; act, or answer nothing."
                                                                                     :name "root.heartbeat"})))]
                        (ok! step (act/bind! world root {:namespace world-id :name "root.heartbeat" :object t}))
                        t))]
      {:trigger trigger :every-s every :armed armed?})))

(defmethod step! :answer-listener [_ _]
  ;; 17.3 step 5 is the resident loop's work (knowledge.world.loop.core): nothing to do at boot
  {:handled-by :loop})

(def first-person-charter
  (str "You are the first person in this world, a Principal driven by a person at a listener. "
       "You read and write everything here, speak to any Principal on its Route, and can make Principals "
       "and issue Grants yourself."))

(defmethod step! :first-person [world step]
  ;; 17.3 step 6: the first person, with a Grant strictly narrower than root's, handed the listener
  ;; (a token file). Since W3 it is one gesture, the action principal.create, run as root as a loop
  ;; would (session? false: the token goes to data/person.token, never into the result).
  (let [root (root-of world)
        {:keys [object result]} (ok! step (actions/perform-action! world root "principal.create"
                                                                   {:name "person" :driven-by "person" :charter first-person-charter
                                                                    :grant {:scope {:prefix "root"} :ops ["read" "reference" "transmit"]}}
                                                                   {:session? false}))
        ;; a person never needs setup (decisions 2026-10-31): the owner of a new world holds every op
        ;; but :delegate, everywhere. This step runs on a fresh world only, never in later-steps:
        ;; an existing world's Grants are its owner's to change. The world issues it, as it issued
        ;; root's: a person's authority sits beside root's and does not descend from it.
        owner (random-uuid)
        w (:world-id world)
        ;; delegable: a person makes a Principal without going through root
        ops #{:read :reference :transmit :write :evaluate :create :delegate}]
    (act/append! world (log/pending :created owner w {:class :grant
                                                      :fields {:grant/grantor w :grant/grantee object :grant/scope :everything
                                                               :grant/ops ops :grant/delegable true}}))
    (act/append! world (log/pending :granted owner w {:grant owner :grantee object :ops ops :scope :everything :delegable true}
                                    :streams #{w object}))
    {:person object :grant (:grant result) :owner-grant owner :route (:route result)
     :token-file (:token-file result)}))

(defmethod step! :know-the-harness [{:keys [world-id config] :as world} step]
  ;; decisions 2026-10-30: root is the bootstrap developer. The repository is mounted as `harness`
  ;; (an fs Adapter), a host Adapter is bound as `shell.local`, root indexes the mount onto itself,
  ;; reads its index and its harness, ethos and charter instructions, reads docs/ethos.md through
  ;; the mount, and runs its first command as a Task. Nothing here is a constant: every value is read.
  (let [root (root-of world)
        harness-root (fs/harness-root config)]
    (when-not harness-root
      (throw (ex-info "harness root not found: set KE_HARNESS_ROOT or run from inside the repository" {:step step})))
    (let [create (fn [class args] (:object (ok! step (act/create! world root class args))))
          ;; idempotent: a world whose earlier run of this step stopped half-way keeps its Adapters
          bound (fn [n] (objects/lookup (act/db world) world-id n))
          mount (or (bound "harness") (create :adapter {:binds :filesystem :locator harness-root :sync :pull :kind :storage}))
          shell (or (bound "shell.local") (create :adapter {:binds :host :locator (fs/work-root config) :sync :both :kind :shell}))]
      (ok! step (act/set-field! world root {:object mount :field :adapter/read-only :type :boolean :value true :reader :both}))
      (when-not (bound "harness") (ok! step (act/bind! world root {:namespace world-id :name "harness" :object mount})))
      (when-not (bound "shell.local") (ok! step (act/bind! world root {:namespace world-id :name "shell.local" :object shell})))
      (ok! step (act/set-field! world root {:object root :field :principal/adapters :type [:list [:ref :adapter]] :value [mount shell] :reader :both}))
      (let [indexed (ok! step (perform/perform! world root {:object mount :action :index :args {}}))]
        (doseq [k [:harness/index :instructions/harness :instructions/ethos :instructions/charter]]
          (ok! step (act/read! world root {:object root :field k})))
        (let [ethos (ok! step (act/read! world root {:name "harness/docs/ethos.md"}))
              run (ok! step (perform/perform! world root {:object shell :action :run :args {:cmd "git log --oneline -1"}}))]
          {:harness mount :shell shell :index-bytes (get-in indexed [:result :bytes]) :head (get-in indexed [:result :head])
           :ethos (:object/id ethos) :task (:object run) :exit (get-in run [:result :exit])})))))

(defmethod step! :notifications [{:keys [world-id] :as world} step]
  ;; the collection a client follows; any session reads where it is (world/notifications)
  (let [root (root-of world)
        id (:object (ok! step (act/create! world root :object {:fields {:instructions/own "Notifications to the people of this world: one Object each, on this stream. See the notify action."}})))]
    (ok! step (act/bind! world root {:namespace world-id :name "notifications" :object id}))
    (ok! step (act/set-field! world root {:object world-id :field :world/notifications :type [:ref :object] :value id :reader :both}))
    {:collection id}))

(defmethod step! :manual [{:keys [world-id] :as world} step]
  ;; the manual's collection; any session reads where it is (world/manual). upgrade! fills it.
  (let [root (root-of world)
        id (:object (ok! step (act/create! world root :object {:fields {:instructions/own "The manual: one Object for each topic. Read it with doc.show and doc.candidates."}})))]
    (ok! step (act/bind! world root {:namespace world-id :name "manual" :object id}))
    (ok! step (act/set-field! world root {:object world-id :field :world/manual :type [:ref :object] :value id :reader :both}))
    {:collection id}))

(defn routes-without-members
  "Routes born before route/members existed."
  [db]
  (vec (sort-by str (remove #(contains? (objects/fields db %) :route/members) (objects/objects-of-class db :route)))))

(defmethod step! :route-members [{:keys [log world-id] :as world} _]
  ;; route/members on every Route, computed from the Principals whose principal/routes
  ;; list it; the world is the actor, as for the shipped Fields. From here on act/set-field! maintains it.
  (let [db (act/db world)
        done (for [route (routes-without-members db)
                   :let [members (act/members-of-route db route)]]
               (do (log/append!! log (log/pending :field-set route world-id {:name :route/members :type [:list [:ref :principal]]
                                                                             :value (vec (sort-by str members)) :reader :both}))
                   [route (count members)]))]
    {:routes (into {} done)}))

(defn- models-without-reasoning
  "Models born before model/reasoning existed (decisions 2026-10-30 'reasoning is asked for')."
  [db]
  (vec (sort-by str (remove #(contains? (objects/fields db %) :model/reasoning) (objects/objects-of-class db :model)))))

(defmethod step! :model-reasoning [{:keys [log world-id] :as world} _]
  ;; model/reasoning :auto on every Model that predates the Field; the world is the actor, as for
  ;; the shipped Fields. A Model on :auto reasons only when a Message asks or after an op failed.
  (let [db (act/db world)
        done (for [model (models-without-reasoning db)]
               (do (log/append!! log (log/pending :field-set model world-id {:name :model/reasoning :type :keyword
                                                                              :value :auto :reader :both}))
                   model))]
    {:models (vec done)}))

(defn principals-without-memory
  "Process Principals lacking a memory default: root and the agents born before it."
  [db]
  (vec (sort-by str (for [p (objects/objects-of-class db :principal)
                          :when (= :process (:principal/driven-by (objects/entity db p)))
                          :when (seq (memory/missing (objects/fields db p)))]
                      p))))

(defmethod step! :memory-defaults [{:keys [log world-id] :as world} _]
  ;; 17.3 step 8: memory/notes, memory/policy, memory/budget and memory/retention-days
  ;; on every process Principal that lacks them, the world as actor, as for the shipped Fields; a
  ;; Principal created from here on is born with them (act create). Idempotent.
  (let [db (act/db world)
        done (for [p (principals-without-memory db)
                   :let [missing (memory/missing (objects/fields db p))]]
               (do (doseq [[k v] missing]
                     (log/append!! log (log/pending :field-set p world-id {:name k :type (memory/types k) :value v :reader :both})))
                   [p (vec (keys missing))]))]
    {:principals (into {} done)}))

(defn principals-without-policy
  "Process Principals lacking an approval policy (decisions 2026-10-30 'approval gates')."
  [db]
  (vec (sort-by str (for [p (objects/objects-of-class db :principal)
                          :when (= :process (:principal/driven-by (objects/entity db p)))
                          :when (seq (gates-defaults/missing (objects/fields db p)))]
                      p))))

(defmethod step! :approval-defaults [{:keys [log world-id] :as world} _]
  ;; 17.3 step 9: policy/approval and policy/approval-ttl-s on every process Principal lacking
  ;; them, the world as actor, as for the shipped Fields; a Principal created from here on is born
  ;; with them (act create). Idempotent.
  (let [db (act/db world)
        done (for [p (principals-without-policy db)
                   :let [missing (gates-defaults/missing (objects/fields db p))]]
               (do (doseq [[k v] missing]
                     (log/append!! log (log/pending :field-set p world-id {:name k :type (gates-defaults/types k) :value v :reader :both})))
                   [p (vec (keys missing))]))]
    {:principals (into {} done)}))

(defn principals-on-old-policy
  "Process Principals still on the default from before `shell` was gated."
  [db]
  (vec (sort-by str (for [p (objects/objects-of-class db :principal)
                          :when (= :process (:principal/driven-by (objects/entity db p)))
                          :when (= ["task.run" "grant" "principal.create"]
                                   (get-in (objects/fields db p) [:policy/approval :field/value]))]
                      p))))

(defmethod step! :shell-gate [{:keys [log world-id] :as world} _]
  ;; decisions 2026-10-31: a world born before `shell` joined the default let `act run` through with no
  ;; card. This only tightens, and only a policy still exactly the old default: anything a person set stands.
  (let [ps (principals-on-old-policy (act/db world))]
    (doseq [p ps]
      (log/append!! log (log/pending :field-set p world-id {:name :policy/approval :type (gates-defaults/types :policy/approval)
                                                            :value gates-defaults/default-policy :reader :both})))
    {:principals (count ps)}))

(defn- working-copy-unmounted [{:keys [config] :as world}]
  (let [work (:work-root config)]
    (when (and work (.isDirectory (java.io.File. (str work)))
               (nil? (objects/lookup (act/db world) (:world-id world) "src.fs")))
      work)))

(defmethod step! :working-copy [world _]
  ;; `harness` is the release this world runs, read-only, and root is who changes ORBIS: without a
  ;; mount of its own working copy, every edit was refused and only a shell could write it (seen live)
  (when-let [work (working-copy-unmounted world)]
    (let [root (objects/root-id (act/db world))]
      (actions/handle! :workspace.mount world root {:principal (str root) :path (str work) :name "src" :shell false} {}))))

(defmethod step! :root-leash [{:keys [log world-id config] :as world} _]
  ;; root is who runs, tests and configures everyone else, and who agents escalate to. A person ends the
  ;; leash with adapter.confine; this runs once, and a policy a person changed stands.
  (let [db (act/db world)
        wide? (not (false? (:root-leash config)))
        root (objects/root-id db)
        shell (objects/lookup db (:world-id world) "shell.local")
        put! (fn [object k type v]
               (log/append!! log (log/pending :field-set object world-id {:name k :type type :value v :reader :both})))]
    (put! root :principal/wide-leash :boolean wide?)
    (when (and wide? shell) (put! shell authority/leash-field :boolean true))
    (when (and wide? (= gates-defaults/default-policy (get-in (objects/fields db root) [:policy/approval :field/value])))
      (put! root :policy/approval (gates-defaults/types :policy/approval) []))
    {:root-leash wide?}))

(def ^:private old-template-sentence "Answer the Message in plain prose, briefly. Every claim about yourself or the world must cite the")

(defn routes-on-old-template
  "Routes whose template still tells the model to cite a Field for every claim."
  [db]
  (vec (sort-by str (for [r (objects/objects-of-class db :route)
                          :let [t (get-in (objects/fields db r) [:route/template :field/value])]
                          :when (and (string? t) (str/includes? t old-template-sentence))]
                      r))))

(defmethod step! :route-template [{:keys [log world-id] :as world} _]
  ;; decisions 2026-10-31: the shipped closing paragraph changes; text around it, which a person may
  ;; have written, stands.
  (let [db (act/db world)
        fresh (subs (shipped-instruction "route-template.md") (str/index-of (shipped-instruction "route-template.md") "Answer in plain prose"))
        rs (routes-on-old-template db)]
    (doseq [r rs]
      (let [t (get-in (objects/fields db r) [:route/template :field/value])]
        (log/append!! log (log/pending :field-set r world-id {:name :route/template :type :string :reader :both
                                                              :value (str (subs t 0 (str/index-of t old-template-sentence)) fresh)}))))
    {:routes (count rs)}))

(defn kinds-undescribed
  "Schema Objects lacking the plain words for their kind: [id Fields-to-write]."
  [db world-id]
  (vec (for [k (keys kinds/descriptions)
             :let [id (objects/lookup db world-id (bound-name k))
                   have (when id (objects/fields db id))
                   missing (when id (into {} (remove (fn [[f _]] (get-in have [f :field/value])) (kinds/fields-for k))))]
             :when (seq missing)]
         [id missing])))

(defn- harness-unmarked
  "The harness mount of a world born before adapter/read-only, or nil."
  [db world-id]
  (when-let [h (objects/lookup db world-id "harness")]
    (when (nil? (get-in (objects/fields db h) [:adapter/read-only :field/value])) h)))

(defmethod step! :harness-read-only [{:keys [log world-id] :as world} _]
  (when-let [h (harness-unmarked (act/db world) world-id)]
    (log/append!! log (log/pending :field-set h world-id {:name :adapter/read-only :type :boolean :value true :reader :both}))
    {:marked h}))

(defmethod step! :kinds [{:keys [log world-id] :as world} _]
  ;; decisions 2026-10-31: class/description on the schema Objects of a world born before them
  (let [todo (kinds-undescribed (act/db world) world-id)]
    (doseq [[id fields] todo [k v] fields]
      ;; a map Field's keys are keywords on the wire and in the log
      (log/append!! log (log/pending :field-set id world-id {:name k :type (kinds/types k) :reader :both
                                                             :value (if (map? v) (update-keys v keyword) v)})))
    {:described (count todo)}))

(defmethod step! :model-catalogue [{:keys [log world-id] :as world} _]
  ;; model/label, model/price-in, model/price-out, model/status on every Model
  ;; born before the catalogue; the world as actor, as for the shipped Fields. Idempotent.
  (let [db (act/db world)
        done (for [e (models/models-without-catalogue db)
                   :let [m (models/missing db e)]]
               (do (doseq [[k v] m]
                     (log/append!! log (log/pending :field-set e world-id {:name k :type (models/types k) :value v :reader :both})))
                   [e (vec (keys m))]))]
    {:models (into {} done)}))

(defn local-chatgpt-plans
  "ChatGPT plan Models created before hosted inference was honestly labelled external."
  [db]
  (vec (sort-by str (for [m (objects/objects-of-class db :model)
                          :let [fs (objects/fields db m)]
                          :when (and (= :chatgpt-plan (get-in fs [:model/api :field/value]))
                                     (= :local (get-in fs [:model/residency :field/value])))]
                      m))))

(defmethod step! :chatgpt-residency [{:keys [log world-id] :as world} _]
  ;; A hosted plan was formerly labelled local. Relabel each old record once, as an ordinary logged
  ;; Field write by the world; from then on a Principal's [:local] policy correctly refuses it.
  (let [models (local-chatgpt-plans (act/db world))]
    (doseq [model models]
      (log/append!! log (log/pending :field-set model world-id {:name :model/residency :type :keyword
                                                                 :value :external :reader :both})))
    {:models models}))

(defmethod step! :token-objects [world _]
  ;; a token Object labelled legacy for every single hash that has none
  {:principals (tokens/migrate-legacy! world)})

(defmethod step! :world-root [{:keys [log world-id] :as world} _]
  ;; a pointer, not authority: which Principal is root, for a world born before the Field existed
  (let [root (objects/root-id (act/db world))]
    (log/append!! log (log/pending :field-set world-id world-id {:name :world/root :type [:ref :principal]
                                                                 :value root :reader :both}))
    {:root root}))

(defmethod step! :world-names [world _]
  ;; a name grants nothing: people and loops are bound where every Principal can see who is speaking
  {:bound (vec (for [[id n] (act/unbound-in-world (act/db world))]
                 (do (act/bind-in-world! world id n) n)))})

(def later-steps
  "Steps added after worlds were already born, with the predicate that says a world has them
   (decisions 2026-10-30): on start, a world missing one runs it once, as root, like a fresh boot."
  {:actions (fn [world] (empty? (actions/missing (act/db world))))
   :arm-heartbeat (fn [world] (some? (heartbeat-of world)))
   :know-the-harness (fn [world] (some? (objects/lookup (act/db world) (:world-id world) "harness")))
   :notifications (fn [world] (some? (get-in (objects/fields (act/db world) (:world-id world)) [:world/notifications :field/value])))
   :manual (fn [world] (some? (get-in (objects/fields (act/db world) (:world-id world)) [:world/manual :field/value])))
   :route-members (fn [world] (empty? (routes-without-members (act/db world))))
   :model-reasoning (fn [world] (empty? (models-without-reasoning (act/db world))))
   :memory-defaults (fn [world] (empty? (principals-without-memory (act/db world))))
   :approval-defaults (fn [world] (empty? (principals-without-policy (act/db world))))
   :model-catalogue (fn [world] (empty? (models/models-without-catalogue (act/db world))))
   :chatgpt-residency (fn [world] (empty? (local-chatgpt-plans (act/db world))))
   :token-objects (fn [world] (empty? (tokens/legacy-without-object (act/db world))))
   :world-root (fn [world] (some? (get-in (objects/fields (act/db world) (:world-id world)) [:world/root :field/value])))
   :world-names (fn [world] (empty? (act/unbound-in-world (act/db world))))
   :kinds (fn [world] (empty? (kinds-undescribed (act/db world) (:world-id world))))
   :harness-read-only (fn [world] (nil? (harness-unmarked (act/db world) (:world-id world))))
   :shell-gate (fn [world] (empty? (principals-on-old-policy (act/db world))))
   :route-template (fn [world] (empty? (routes-on-old-template (act/db world))))
   :working-copy (fn [world] (nil? (working-copy-unmounted world)))
   :root-leash (fn [world] (let [db (act/db world)]
                             (some? (get-in (objects/fields db (objects/root-id db)) [:principal/wide-leash :field/value]))))})

(defn ensure-shipped-instructions!
  "Shipped instruction Fields on root, for a world born before some of them shipped: a missing one
   is written, and one still authored by the world (root never took it over) whose text changed
   with the ship is rewritten; root's own edits are kept. The world is the actor, as at boot.
   Returns the Field names written."
  [{:keys [log world-id] :as world}]
  (let [root (root-of world)
        current (objects/fields (act/db world) root)]
    (vec (for [[k file] instruction-files
               :let [text (shipped-instruction file)
                     f (get current k)]
               :when (and text (or (nil? f) (and (= world-id (:field/author f)) (not= text (:field/value f)))))]
           (do (log/append!! log (log/pending :field-set root world-id {:name k :type :string :value text :reader :both}))
               k)))))

(defn upgrade!
  "Run the later steps a started world lacks (after refreshing the shipped instruction Fields).
   Returns {step result-or-error} for those run."
  [world]
  (when-let [written (seq (ensure-shipped-instructions! world))]
    (timbre/info "kernel-zero: shipped instruction Fields written" {:fields written}))
  (into {}
        (for [[k done?] later-steps :when (not (done? world))]
          [k (try (let [r (step! world k)]
                    (timbre/info "kernel-zero later step" {:step k :result r})
                    r)
                  (catch Exception e
                    (timbre/warn "kernel-zero later step skipped" {:step k :reason (ex-message e)})
                    {:error (ex-message e)}))])))

(defn run-steps!
  "Run the steps in order as root; a failing or unbuilt step is logged and skipped so the world
   still starts. Returns {step result-or-error}."
  [world]
  (into {}
        (for [k steps]
          [k (try (let [r (step! world k)]
                    (timbre/info "kernel-zero step" {:step k :result r})
                    r)
                  (catch Exception e
                    (timbre/warn "kernel-zero step skipped" {:step k :reason (ex-message e)})
                    {:error (ex-message e)}))])))
