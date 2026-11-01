(ns knowledge.world.loop.ops
  "The model acts (spec 9.8 steps 5 and 6, decisions 2026-10-30 'model acts through protocol ops').
   A Model's outcome text may carry fenced blocks tagged `ops`, one JSON object per line in the
   protocol's JSON form, `{\"op\": ..., \"args\": {...}}`; OpenAI tool_calls map to the same shape.
   Each line is validated against the wire schema (:ke/Request, ADR 0002) and executed through the
   same multimethod a client session reaches, as the loop's Principal, so resolution and Grants
   apply unchanged (ground rule 4). Session ops (attach, detach, subscribe, unsubscribe) and the
   ack frame have no meaning inside a loop and answer err not-allowed-in-loop. The caps are Fields
   on the Principal, loop/max-ops (16) and loop/max-iterations (8).

   Text outside the fences is the reply draft; an outcome without ops is the final reply."
  (:require [clojure.string :as str]
            [knowledge.world.thread :as thread]
            [malli.core :as m]
            [knowledge.schema.core :as sc]
            [clojure.walk :as walk]
            [datascript.core :as d]
            [jsonista.core :as j]
            [knowledge.schema.protocol :as proto]
            [knowledge.schema.wire :as wire]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.adapters.model-api :as model]
            [knowledge.world.code :as code]
            [knowledge.world.refs :as refs]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.protocol.codec :as codec]
            [knowledge.world.protocol.ops :as ops]
            [knowledge.world.scratch.policy :as policy]))

(def default-max-ops 16)         ; loop/max-ops on the Principal overrides
(def default-max-iterations 8)   ; turns any wake gets; loop/max-iterations on the Principal makes it a hard limit
(def default-max-turns 30)       ; how far progress can carry a wake past that; loop/max-turns overrides
(def progress-grace 4)           ; turns a wake may go on after the last that found or changed something

(def not-allowed #{:attach :detach :subscribe :unsubscribe :ack})

(def fence #"(?s)```ops[ \t]*\n(.*?)```")
;; GLM sometimes writes its ops as a literal tool-call tag in the content instead of a fence (seen live,
;): `<tool_call>ops` then the JSON lines, closed by </tool_call> or the end of the text.
(def tag-fence #"(?s)<tool_call>ops[ \t]*\n(.*?)(?:</tool_call>|\z)")
;; a whole reply of `read {"object": "2b"} ||| search.find {...}`, ops in a form nothing asked for (seen
;; live: it reached a person as the answer). Only a reply that is nothing else counts.
(def plain-op #"^([A-Za-z][\w.-]*)[ \t]+(\{.*\})$")

(defn- plain-lines
  "The op lines of a reply made only of ops, one to a line or joined by |||: `name {json}` or the
   JSON itself with no fence (seen live, sent to a person as the answer). Nil for anything else."
  [text tag-line]
  (let [segments (remove str/blank? (map str/trim (str/split (str text) #"\|\|\||\n")))
        lines (map (fn [seg]
                     (cond (re-find #"^\{\s*\"op\"" seg) seg
                           (re-find plain-op seg) (let [[_ nm body] (re-find plain-op seg)] (tag-line nm body))))
                   segments)]
    (when (and (seq segments) (every? some? lines)) lines)))

(def ^:private op-json-line #"^\s*\{\s*\"op\"\s*:")

;; `<tool_call>~atlas.shell{"cmd":"ls"}`: the Adapter's name as the tag, seen live on most wakes of a
;; narrow Principal. A shell is asked to run; anything else is asked to read.
(def tag-adapter #"(?s)<tool_call>~([\w.-]+)[ \t]*(\{.*?\})[ \t\n]*(?=<tool_call>|</tool_call>|\z)")

;; `ops` alone on a line, then a ```json fence (seen live, a whole wake of it: every answer reached the
;; person as raw JSON). The word on its own line is required, or a reply showing JSON would run it.
(def word-fence #"(?s)(?:\A|(?<=\n))[ \t]*ops[ \t]*\n[ \t]*```(?:json)?[ \t]*\n(.*?)```")
(def open-fence #"(?m)^[ \t]*```ops[ \t]*$\n?")   ; an opener with no closer: stripped, counted
;; (report 2026-10-30, finding 8): an opener glued to prose with no closer
;; (`…before acting.```ops` then JSON lines to the end) is still the model acting: the JSON lines
;; that follow are ops, the opener is counted as malformed, the prose before it stays the draft
(def glued-open-fence #"(?s)```ops[ \t]*\n?(\{.*)\z")
;; GLM's other literal framing: the action name as the tag, `<tool_call>act<tool_call>{...}` (or a
;; dotted action name), one JSON object, closed by </tool_call> or the end of the text
(def tag-call #"(?s)<tool_call>([A-Za-z][\w.-]*)<tool_call>[ \t\n]*(\{.*?\})[ \t\n]*(?:</tool_call>|\z)")

;; and two more, seen live 2026-10-31: `<tool_call>op read<tool_call>args {...}</arg_value></tool_call>`,
;; and the model's native pairs `<tool_call>read<arg_key>name</arg_key><arg_value>x</arg_value></tool_call>`
(def tag-args #"(?s)<tool_call>op[ \t]+([A-Za-z][\w.-]*)[ \t\n]*<tool_call>[ \t\n]*args[ \t\n]*(\{.*?\})[ \t\n]*(?:</arg_value>)?[ \t\n]*(?:</tool_call>|\z)")
(def tag-pairs #"(?s)<tool_call>([A-Za-z][\w.-]*)[ \t\n]*((?:<arg_key>.*?</arg_key>[ \t\n]*<arg_value>.*?</arg_value>[ \t\n]*)+)(?:</tool_call>|\z)")
(def tag-pair #"(?s)<arg_key>(.*?)</arg_key>[ \t\n]*<arg_value>(.*?)</arg_value>")

(defn- json-or-string [s]
  (try (j/read-value s j/keyword-keys-object-mapper) (catch Exception _ s)))

(defn- tag-line
  "One op line from a literal tag: the op it named, or act when the args carry an action (the model
   wrote op read over act args, seen live)."
  [nm args]
  (j/write-value-as-string {:op (if (and (map? args) (:action args)) "act" nm) :args args}))

(defn- explain [e]
  (let [d (ex-data e)]
    (str (ex-message e) (when (:explain d) (str " " (pr-str (:explain d)))))))

(def wire-ops (set (map name proto/ops)))

(defn- action-as-op
  "A line whose op is an action name (`principal.create`, `task.run`: not a wire op, dotted) is
   the same request as `act {action args}`; the model writes it that way often enough that the
   loop accepts it (decisions 2026-10-30 'named actions')."
  [m]
  (let [op (:op m)]
    (if (and (string? op) (not (wire-ops op)) (str/includes? op "."))
      {:op "act" :args {:action op :args (or (:args m) {})}}
      m)))

(def corrected
  "The requests an error points to, as tool calls a model can make. A test holds each to the tool
   schemas, so an error only ever quotes a shape the model can send."
  {:read-file ["read" {"name" "<mount>/<path>"}]
   :read-object ["read" {"object" "<handle>"}]
   :resolve ["resolve" {"name" "<name>"}]
   :run ["run" {"cmd" "<command>"}]
   :send ["send" {"to" ["<handle>"] "route" "<handle>" "text" "..."}]})

(defn- call [k]
  (let [[tool args] (corrected k)] (str tool " " (j/write-value-as-string args))))

(defn- hint
  "A parse error with what the model most likely meant, and the request that does it."
  [^String line ^String msg]
  (cond
    (re-find #"invalid dispatch value" msg)
    (str msg " (not an op. Use one of your tools; a command is " (call :run) ")")
    (re-find #":to \[\"invalid type\"\]" msg)
    (str msg " (`to` is a list of Principals, and a Route goes in `route`: " (call :send) ")")
    (re-find #"Invalid UUID string" msg)
    (str msg " (a reference is a short handle from this prompt, a bound name or a full id, and this is none of those. "
         "A mounted file: " (call :read-file) "; to see what a name denotes: " (call :resolve) ")")
    (re-find #":op \[\"missing required key\"\]" msg)
    (str msg " (" (call :resolve) ")")
    :else msg))

;; ---------------------------------------------------------------- what a model did not mean

(def ^:private blank-is-meant
  "Args where an empty value says something: a Field set to nothing, an edit that deletes."
  #{"value" "new" "data" "fields" "markdown"})

(def ^:private zero-is-unset "Counts no call means as 0." #{"limit" "last" "after" "timeout-s" "deadline-s"})

(defn- unmeant?
  "A placeholder: what a model fills in for an argument it has no use for."
  [k v]
  (or (nil? v)
      (and (string? v) (str/blank? v))
      (and (coll? v) (empty? v))
      (and (zero-is-unset k) (number? v) (zero? v))
      (and (= "range" k) (sequential? v) (every? #(and (number? %) (zero? %)) v))))

(defn clean-args
  "Tool or op arguments with placeholders gone, at every depth an action's args nest. The one place a
   blank argument is dropped: an optional argument is absent or it means something."
  [args]
  (if-not (map? args)
    args
    (into {} (keep (fn [[k v]]
                     (let [n (name k)]
                       (cond (blank-is-meant n) [k v]
                             (= "args" n) [k (clean-args v)] ; an action's args are required, even empty
                             ;; forgiven, not refused: 10 failures for `:string`, 5 for a range as a string
                             (and (= "type" n) (string? v) (str/starts-with? v ":")) [k (subs v 1)]
                             (and (= "range" n) (string? v) (re-find #"\d" v)) [k (mapv parse-long (re-seq #"\d+" v))]
                             (unmeant? n v) nil
                             (map? v) [k (clean-args v)]
                             :else [k v]))))
          args)))

;; ---------------------------------------------------------------- handles in ops

(def ^:private id-keys
  "Where an op or an action takes an id, at any depth."
  #{:object :to :route :in-reply-to :principal :adapter :task :tasks :token :model :members :about
    :objects :root :routes-from :thread :message :grantee :scope :namespace :stream :tags :pane :target})

(def ^:private literal-keys "Data, whatever its keys are called." #{:value :data :fields :args-prefix})

(defn- expand-id
  "A handle or a bound name as the id it stands for. A full id passes; what looks like a handle and is
   neither is an error that says what it could have meant, never a guess."
  [db p table v]
  (let [s (if (number? v) (str v) v)
        ;; `[[52|shell.local]]` where an id goes: the text form of a reference (seen live)
        s (if (string? s) (or (second (re-matches #"\[\[([^\]|]+)(?:\|[^\]]*)?\]\]" s)) s) s)]
    (cond
      (not (string? s)) v
      (re-matches refs/whole-uuid-re s) s
      (contains? table s)
      (let [named (resolve/lookup db p s)]
        (if (and named (not= (str named) (table s)))
          (throw (ex-info "ambiguous" {:unknown-id s :could-mean [(str s " is " (table s)) (str s " is also the name of " named)]}))
          (table s)))
      ;; a name where an id goes: what a person would write, and the model does (seen live, most wakes)
      (resolve/lookup db p s) (str (resolve/lookup db p s))
      (not (re-matches refs/handle-like-re s)) v
      :else (throw (ex-info "unknown" {:unknown-id s :could-mean (refs/could-mean table s)})))))

(defn- walk-ids [f m]
  (let [in-coll (fn [x] (if (map? x) (walk-ids f x) (f x)))]
    (into {} (map (fn [[k v]]
                    [k (cond (literal-keys k) v
                             (id-keys k) (cond (map? v) (walk-ids f v)
                                               (set? v) (into #{} (map in-coll) v)
                                               (sequential? v) (mapv in-coll v)
                                               :else (f v))
                             (map? v) (walk-ids f v)
                             (sequential? v) (mapv #(if (map? %) (walk-ids f %) %) v)
                             :else v)]))
          m)))

(defn- with-text
  "args with the text at k as display text, its spans at refs-k when there are any."
  [db p table args k refs-k]
  (let [v (get args k)]
    (if-not (and (string? v) (str/includes? v "[["))
      args
      (let [{:keys [text refs unresolved]} (refs/parse db p table v)]
        (cond-> (assoc args k text)
          (seq refs) (assoc refs-k refs)
          (seq unresolved) (assoc :unresolved-refs unresolved))))))

(defn- markdown-of
  "A view's markdown as display text, its spans at :data-refs."
  [db p table args]
  (let [md (get-in args [:data :markdown])]
    (if (string? md)
      (let [{:keys [text refs]} (refs/parse db p table md)]
        (cond-> (assoc-in args [:data :markdown] text) (seq refs) (assoc :data-refs refs)))
      args)))

(defn- text-args
  "Everything a person will read in an op's args, through references."
  [db p table op args]
  (let [t (fn [a k rk] (with-text db p table a k rk))
        view (fn [a] (-> a (t :title :title-refs) (as-> x (markdown-of db p table x))))]
    (case op
      :send (t args :text :refs)
      :set-field (t args :value :refs)
      :present (view args)
      :act (if (map? (:args args))
             (update args :args #(-> % (t :text :refs) view))
             args)
      args)))

(defn expand-args
  "An op's args with handles in the wake's table expanded and its text parsed. {:args} or {:err}.
   Without a table (a session's op, a caller outside a wake) nothing changes."
  [db p table op args]
  (cond
    (or (nil? table) (not (map? args))) {:args args}
    :else
    (try
      (let [op (keyword (name op))
            a (walk-ids #(expand-id db p table %) args)
            a (if (and (#{:read :resolve} op) (contains? table (:name a)))
                (assoc a :name (str "/" (table (:name a))))
                a)]
        {:args (text-args db p table op a)})
      (catch clojure.lang.ExceptionInfo e
        (if-let [id (:unknown-id (ex-data e))]
          {:err {:code :unknown-id :could-mean (:could-mean (ex-data e))
                 :message (str id " is no handle of this wake"
                               (when-let [c (seq (:could-mean (ex-data e)))] (str "; it could mean: " (str/join "; " c)))
                               ". Handles last one wake: use one from this prompt, or the full id.")}}
          (throw e))))))

(defn expand-ops
  "expand-args over parsed ops, {:op :args}: an op that fails carries :err, as a bad line does."
  [db p table ops]
  (mapv (fn [{:keys [op args] :as o}]
          (let [{a :args e :err} (expand-args db p table op args)]
            (if e (assoc o :err e) (assoc o :args a))))
        ops))

(defn- read-of-handle
  "`read {name \"/5c\"}` and `read {name \"5c/task/stdout\"}` are the Object 5c: a run's whole output
   is one read away, and 8 reads of it in one wake went to the file tree instead."
  [{:keys [op args] :as m}]
  (let [n (:name args)
        [_ a f b g] (when (and (= "read" op) (string? n))
                      (re-matches #"(?:/([0-9a-f-]{2,36})(?:/(task/[\w-]+))?|([0-9a-f-]{2,36})/(task/[\w-]+))" n))
        id (or a b)
        field (or f g)]
    (if id
      (assoc m :args (cond-> (-> args (dissoc :name) (assoc :object id)) field (assoc :field field)))
      m)))

(defn- parse-line
  "One JSON line -> {:op :args} validated, or {:raw :err}. Handles expand before the line is
   validated, since a handle is not yet an id."
  [{:keys [db p ids]} n ^String line]
  (try
    (let [m (action-as-op (j/read-value line j/keyword-keys-object-mapper))
          m (cond-> m (map? (:args m)) (update :args clean-args))
          ;; act around act, the whole call again as its args (seen live, twice in a turn)
          m (let [inner (get-in m [:args :args])]
              (if (and (= "act" (:op m)) (map? inner) (:args inner) (= (some-> (:action inner) name) (some-> (get-in m [:args :action]) name)))
                (assoc m :args inner)
                m))
          m (read-of-handle m)
          op (:op m)
          {a :args e :err} (if (and (string? op) (not (not-allowed (keyword op))))
                             (expand-args db p ids op (:args m))
                             {:args (:args m)})
          ;; `resolve` asks with an op of its own; a model looking a name up means read
          a (if (and (= "resolve" op) (map? a) (not (act/grant-ops (keyword (str (:op a)))))) (assoc a :op "read") a)
          ;; `atlas/` is the directory `atlas`: a model lists one that way, and got unresolved for it
          a (if (and (= "read" op) (map? a) (string? (:name a)) (re-find #"[^/]/+$" (:name a)))
              (update a :name #(str/replace % #"/+$" ""))
              a)
          ;; a read has two shapes: what is no id is a name, whichever argument it came in
          a (if (and (= "read" op) (map? a) (string? (:object a)) (not (re-matches refs/whole-uuid-re (:object a))))
              (-> a (assoc :name (:object a)) (dissoc :object))
              a)
          ;; `history {object}` is one Object's stream; left alone it was the whole log
          a (if (and (= "history" op) (map? a) (:object a))
              (-> a (assoc :stream (:object a)) (dissoc :object))
              a)
          m (assoc m :args a)]
      (cond
        (:refused m) {:raw line :err (update (:refused m) :code keyword)}
        e {:op (keyword op) :raw line :err e}
        (and (= "grant" op) (map? (:args m)) (act/invalid-ops (get-in m [:args :ops])))
        {:raw line :err (:err (act/invalid-ops (get-in m [:args :ops])))}
        (and (map? m) (string? op) (not (not-allowed (keyword op))))
        (let [req (wire/decode :ke/Request (j/write-value-as-string {:id (str "loop-" n) :op op :args (or (:args m) {})}))]
          {:op (:op req) :args (:args req)})
        (and (string? op) (not-allowed (keyword op)))
        {:raw line :err {:code :not-allowed-in-loop :message (str "op " op " has no meaning inside a loop")}}
        :else
        {:raw line :err {:code :invalid :message "a line must be {\"op\": ..., \"args\": {...}}"}}))
    (catch Exception e
      {:raw line :err {:code :invalid :message (hint line (explain e))}})))

;; and a third: the op's own JSON straight after the tag, `<tool_call>{"op":...}` (seen live: the run
;; was lost as prose, and the reply carried the raw tag)
(def tag-json #"(?s)<tool_call>[ \t\n]*(\{\s*\"op\".*?\})[ \t\n]*(?:</tool_call>|\z)")

(def ^:private tool-actions
  "Tools that are actions: a model calls what it means, and has no `act` envelope to get wrong."
  {"grep" "code.search" "outline" "code.outline" "edit" "file.edit" "run" "task.run" "projects" "projects" "add_project" "project.add"
   "delegate" "delegate" "progress" "task.progress" "ask" "task.ask" "text_back" "gateway.send"})

(defn- sole-shell
  "p's shell Adapter when it has exactly one, so `run` needs no id."
  [{:keys [db p]}]
  (when (and db p)
    (let [hosts (filter #(= :host (get-in (objects/fields db %) [:adapter/binds :field/value]))
                        (get-in (objects/fields db p) [:principal/adapters :field/value]))]
      (when (= 1 (count hosts)) (str (first hosts))))))

(defn- test-command
  "The test command recorded on p's shell, else what the project's own files say, or nil."
  [{:keys [db] :as wake}]
  (when-let [shell (sole-shell wake)]
    (let [fields (objects/fields db (parse-uuid shell))]
      (or (get-in fields [:adapter/test :field/value])
          (some-> (get-in fields [:adapter/locator :field/value]) code/test-command)))))

(def ^:private no-test-command
  {:code :no-test-command
   :message (str "this project has no test command that I can find: no Makefile, CMake, Cargo, npm, Go, Maven, "
                 "Gradle, pytest or tests/run.sh in its root. Find how it builds with outline, then `run` that "
                 "command, or say it has no tests.")})

(defn- unwrapped
  "Tool arguments as a map. Seen live: the whole op as the tool's name, and args nested in args as a
   JSON string; both are the op the model meant, and each cost a turn."
  [arguments]
  (let [a (if (string? arguments) (try (j/read-value arguments) (catch Exception _ arguments)) arguments)
        inner (when (and (map? a) (= 1 (count a))) (get a "args"))]
    (cond (string? inner) (try (j/read-value inner) (catch Exception _ a))
          (map? inner) inner
          :else a)))

(defn- missing-args
  "The required arguments of a tool call that are absent or blank: known before anything runs."
  [tool args]
  (when-let [f (some #(when (= tool (get-in % [:function :name])) (:function %)) model/op-tools)]
    (let [given (clean-args (if (map? args) args {}))]
      (remove #(contains? given %) (get-in f [:parameters :required])))))

(defn- missing-err [tool missing]
  (let [f (some #(when (= tool (get-in % [:function :name])) (:function %)) model/op-tools)
        args (map name (keys (get-in f [:parameters :properties])))]
    {:code :invalid-args
     :message (str tool " needs " (str/join " and " missing) ". Its arguments are " (str/join ", " args)
                   ": call it again with those, and leave out what you do not use.")}))

(defn- task-run-args
  "An `act` naming task.run, with its shell wherever the model put it: beside the args, as the object,
   or left out when there is only one, and that one wins over an `object` that is not a shell: `run`
   on a Principal or a Task raised a card, and failed once approved. 15 failed calls in one 12-task
   run were these three."
  [wake args]
  (let [inner (let [i (get args "args")] (if (map? i) i {}))
        adapter (some #(when-not (str/blank? (str %)) %)
                      [(get inner "adapter") (get args "adapter") (sole-shell wake) (get args "object")])]
    {"action" "task.run" "args" (cond-> inner adapter (assoc "adapter" adapter))}))

(def ^:private guessed-actions
  "Action names models reach for, as the tool that does it: 12 failures in one benchmark, and one
   wake called `code.read` five times over."
  {"code.read" "read" "file.read" "read" "code.list-dir" "outline" "code.list" "outline"
   "code.shell" "run" "code.run" "run" "shell.run" "run"})

(defn- sole-mount
  "p's one filesystem mount's name, or nil."
  [{:keys [db p]}]
  (when (and db p)
    (let [fields #(objects/fields db %)
          mounts (filter #(= :filesystem (get-in (fields %) [:adapter/binds :field/value]))
                         (get-in (fields p) [:principal/adapters :field/value]))]
      (when (= 1 (count mounts)) (some-> (first (objects/names db (first mounts))) second)))))

(defn- as-tool
  "A guessed action as the tool it meant: {:name :arguments}, or the call as it came."
  [wake {:keys [name arguments] :as call}]
  (let [a (unwrapped arguments)
        tool (when (and (= "act" name) (map? a)) (guessed-actions (str (get a "action"))))]
    (if-not tool
      call
      (let [inner (let [i (get a "args")] (if (map? i) i {}))
            path (or (get inner "name") (get inner "path") (get inner "file"))
            mount (sole-mount wake)
            full (if (and path mount (not (str/starts-with? (str path) (str mount "/"))) (not (str/starts-with? (str path) "/")))
                   (str mount "/" path)
                   path)]
        {:name tool :arguments (cond-> (dissoc inner "path" "file") path (assoc "name" full))}))))

(def ^:private test-timeout-s "A project's whole suite outruns a command's default two minutes." 900)

(defn- tool-line
  "One tool call as the JSON line parse-line reads."
  [wake call]
  (let [{:keys [name arguments]} (as-tool wake call)
        args (let [a (unwrapped arguments)]
               (if (and (= "act" name) (map? a) (#{"task.run" "shell" "run"} (str (get a "action")))) (task-run-args wake a) a))
        action (tool-actions name)
        missing (missing-args name args)]
    (cond
      (str/starts-with? (str/trim (str name)) "{") (str/trim name)
      (seq missing) (j/write-value-as-string {:op name :args {} :refused (missing-err name missing)})
      ;; the project's own command, whole: told it in a line, the model still ran it a step at a
      ;; time and stopped at a step (2 of 3 runs), or made up a command
      (and (= "test" name) (test-command wake))
      (j/write-value-as-string {:op "act" :args {:action "task.run"
                                                 :args {"adapter" (sole-shell wake) "cmd" (test-command wake)
                                                        "timeout-s" test-timeout-s}}})
      (= "test" name) (j/write-value-as-string {:op name :args {} :refused no-test-command})
      action (j/write-value-as-string
              {:op "act" :args {:action action
                                :args (if (and (= "run" name) (map? args) (str/blank? (str (get args "adapter"))))
                                        (cond-> args (sole-shell wake) (assoc "adapter" (sole-shell wake)))
                                        args)}})
      :else (j/write-value-as-string {:op name :args args}))))

(defn- spelled-as-tool
  "A line naming a tool as its op, `{\"op\":\"edit\",\"args\":{...}}`, is that tool's call. Seen live: 22
   of 94 ops in one wake were refused with no word of why, each a whole page rewritten and lost."
  [wake line]
  (let [m (try (j/read-value line j/keyword-keys-object-mapper) (catch Exception _ nil))
        op (when (map? m) (:op m))]
    (if (and (string? op) (or (tool-actions op) (= "test" op)) (map? (:args m)))
      (tool-line wake {:name op :arguments (j/write-value-as-string (:args m))})
      line)))

(defn parse
  "Outcome {:text :tool-calls} -> {:ops [...] :reply text}. ops are in order of appearance;
   each is {:op :args} or {:raw :err}. tool-calls are [{:name :arguments}] from the adapter."
  ([outcome] (parse outcome nil))
  ([{:keys [text tool-calls]} wake]
  (let [;; a third spelling of the same pairs, seen live: `<parameter name="k">v</arg_value>`
        text (-> (str text)
                 (str/replace #"<parameter name=\"([^\"]+)\">" "<arg_key>$1</arg_key><arg_value>")
                 (str/replace "</parameter>" "</arg_value>"))
        blocks (concat (map second (re-seq fence text)) (map second (re-seq tag-fence text)) (map second (re-seq word-fence text)))
        lines (->> blocks (mapcat str/split-lines) (map str/trim) (remove str/blank?)
                   (map #(spelled-as-tool wake %)))
        from-tools (map #(spelled-as-tool wake (tool-line wake %)) tool-calls)
        ;; `<tool_call>act<tool_call>{...}`: the tag names the op (an action name stands as one)
        from-tags (map (fn [[_ nm body]]
                         (if (= "ops" nm) "" (j/write-value-as-string {:op nm :args (try (j/read-value body j/keyword-keys-object-mapper) (catch Exception _ body))})))
                       (remove #(= "ops" (second %)) (re-seq tag-call text)))
        from-args (map (fn [[_ nm body]] (tag-line nm (json-or-string body))) (re-seq tag-args text))
        from-pairs (map (fn [[_ nm pairs]]
                          (tag-line nm (into {} (map (fn [[_ k v]] [(keyword (str/trim k)) (json-or-string (str/trim v))]) (re-seq tag-pair pairs)))))
                        (re-seq tag-pairs text))
        from-json (map (fn [[_ body]] (str/replace body #"\s*\n\s*" " ")) (re-seq tag-json text))
        without (-> text (str/replace fence "") (str/replace tag-fence "") (str/replace word-fence "") (str/replace tag-call "")
                    (str/replace tag-args "") (str/replace tag-pairs "") (str/replace tag-json ""))
        ;; a glued, unclosed opener followed by JSON lines: those lines are ops (counted as malformed)
        glued (second (re-find glued-open-fence without))
        glued-lines (when glued (->> (str/split-lines glued) (map str/trim) (filter #(str/starts-with? % "{"))))
        without (if glued (str/replace without glued-open-fence "") without)
        from-adapter (map (fn [[_ nm body]]
                            (let [a (json-or-string body)]
                              (if (and (map? a) (:cmd a))
                                (j/write-value-as-string {:op "act" :args {:object nm :action "run" :args a}})
                                (j/write-value-as-string {:op "read" :args (if (map? a) a {:name nm})}))))
                          (re-seq tag-adapter text))
        without (str/replace without tag-adapter "")
        from-plain (when-not (or (seq lines) (seq tool-calls) (seq from-adapter))
                     (or (plain-lines without (fn [nm body] (tag-line nm (json-or-string body))))
                         ;; op lines in any other wrapper: `<ops>` is the one seen last
                         (when-not (str/includes? without "```")
                           (seq (map str/trim (filter #(re-find op-json-line %) (str/split-lines without)))))))
        without (if from-plain
                  (str/trim (str/join "\n" (remove #(or (re-find op-json-line %) (re-find #"^\s*</?ops>\s*$" %))
                                                   (str/split-lines without))))
                  without)
        without (if (and from-plain (plain-lines without (fn [_ _] ""))) "" without)
        all (concat lines from-tags from-args from-pairs from-json glued-lines from-adapter from-plain from-tools)
        ;; a stray unclosed ```ops fence with no JSON after it is not an ops block: the marker goes, the
        ;; text stays, the Evaluation's context counts it (malformed-fences) so the nit is visible
        malformed (+ (count (re-seq open-fence without)) (if glued 1 0))
        reply (-> without (str/replace open-fence "") str/trim)]
    ;; whichever way it arrived: a bare `{"op":"edit",...}` line outside a fence lost a new file for
    ;; good, its wake refused three times over
    {:ops (vec (map-indexed (partial parse-line wake) (map #(spelled-as-tool wake %) (remove str/blank? all))))
     :reply reply
     :malformed-fences malformed})))

(defn fieldable
  "A value fit for a Field (schema Value): keyword keys, no sets, no symbols, no nils, no lazy
   seqs (vectors only). a resolve that found nothing, a history page and a search
   carried nils and seqs into evaluation/results, the field-set failed validation, and the whole
   wake died with `pending event invalid` (seen live on 7830, four times)."
  [v]
  (walk/postwalk (fn [x] (cond (map-entry? x) x
                               (set? x) (vec (sort-by str x))
                               (map? x) (into {} (keep (fn [[k v]] (when (some? v) [(if (keyword? k) k (keyword (str k))) v]))) x)
                               (vector? x) (vec (remove nil? x))
                               (and (sequential? x) (not (vector? x))) (vec (remove nil? x))
                               (symbol? x) (str x)
                               (char? x) (str x)
                               (ratio? x) (double x)
                               :else x))
                 v))

(defn storable?
  "Does v validate as a Field value (schema Value)?"
  [v]
  (m/validate [:ref :knowledge.schema.core/value] v {:registry (sc/registry)}))

(defn recordable
  "results as evaluation/results can hold them: each element fieldable, and one that still does not
   validate replaced by its op with err unrecordable and a clipped print, so a strange result never
   costs the wake (the Evaluation's record must land whatever came back)."
  [results]
  (mapv (fn [r]
          (let [f (fieldable r)]
            (if (storable? f)
              f
              {:op (str (:op r)) :ok false
               :err {:code :unrecordable
                     :message (str "the result could not be stored as a Field: " (let [s (pr-str r)] (subs s 0 (min 400 (count s)))))}})))
        results))

(defn- loop-ctx
  "The ctx a session handler expects, standing in for a socket: the loop's Principal is the
   session principal; via is the Evaluation the ops come from (set-field and send carry it);
   hop is the leg count a Message sent from here carries (message/hop); thread is the
   conversation this wake works for, which a send that answers nothing inherits;
   message and route are the wake's, which a named action defaults its audience from."
  ([world p via hop thread] (loop-ctx world p via hop thread nil nil))
  ([world p via hop thread message route]
  {:world world :via via :hop hop :thread thread :loop? true :message message :route route
   :conn (atom {:format :transit :session (atom {:id (str "loop-" p) :principal p :client-kind :loop})})}))

(def ^:private live-actions
  "Named actions on a live pane: read, send, read again is the point, so a repeat means a repeat."
  #{"pane.read" "pane.list" "pane.send" "pane.frob" "pane.resize" "pane.close"})

(def ^:private looking-actions
  "Named actions that only look, whose answer turns on every argument. As making ops they had no
   required args to tell them apart, so a wake's second outline got its first back, whatever it
   named: the benchmark's `outline` results that were not visible, 13 of them in one task."
  #{"code.search" "code.outline" "projects"})

(defn- action-of [args] (some-> (:action args) name))

(defn- making?
  "Does the op make something, so that repeating it within a wake duplicates it? A named action or
   present; a send has its own rule; never a read, resolve, history, set-field
   (idempotent), a run/index/cancel on an Object (the model may mean it twice) or a live pane."
  [op args]
  (or (= :present op)
      (and (= :act op) (map? args) (:action args) (nil? (:object args)) (nil? (:class args))
           (not (live-actions (action-of args)))
           (not (looking-actions (action-of args))))))

(defn repeat-key
  "What makes two making ops the same: for a named action its name and required
   args only (optional ones such as in-reply-to or title do not count), for present the audience
   and the content, else the whole args."
  [op args]
  (let [a (fieldable args)
        action (:action a)
        action-name (when action (if (keyword? action) (name action) (str action)))]
    (cond
      (and (= :act op) (= "file.edit" action-name)) [action-name (:args a)] ; `old` says which edit
      (and (= :act op) action-name)
      (if-let [req (actions/required-args action-name)]
        [action-name (select-keys (if (map? (:args a)) (:args a) {}) req)]
        [action-name (:args a)])
      (= :present op) (select-keys a [:to :route :kind :title :data])
      :else a)))

(defn- changes-files? [d]
  (and (= "act" (str (:op d))) (#{"file.edit" "task.run"} (action-of (:args d)))))

(defn repeat-of
  "The earlier successful result of a making op equal (by repeat-key) to one in this wake, or nil.
   A run counts only while nothing has changed since it: edit, test, edit, test is the work, and the
   second `make test` is not the first one's answer."
  [done op args]
  (if (making? op args)
    (let [k (repeat-key op args)
          same? (fn [d] (and (= (name op) (str (:op d))) (= k (repeat-key (keyword (str (:op d))) (:args d)))))]
      (if (= "task.run" (action-of args))
        (first (filter same? (take-while #(or (same? %) (not (changes-files? %))) (reverse done))))
        (first (filter same? done))))
    ;; a person's approval ran this exact op once: asking again would only be a second card
    (when (and (= :act op) (map? args) (live-actions (action-of args)))
      (let [a (fieldable args)]
        (some (fn [d] (when (and (= "act" (str (:op d))) (:approved (:result d)) (= a (:args d))) d)) done)))))

(defn- send-key
  "What makes two sends the same: recipients, Route and text (trimmed). to may be a set (decoded
   args) or a vector (a result's fieldable args)."
  [{:keys [to route text]}]
  [(set (map str (if (coll? to) to [to]))) (str route) (str/trim (str text))])

(defn prior-sends
  "The sends this wake already made, from the evaluation/results of the Evaluations that ran for
   the same Message: {send-key message-id}. The log is the memory; the loop reads
   it rather than keeping state between iterations."
  [db message]
  (if-not message
    {}
    (let [evals (d/q (quote [:find [?id ...] :in $ ?m :where
                             [?f :field/name :evaluation/message] [?f :field/value ?m]
                             [?o :object/fields ?f] [?o :object/id ?id]])
                     db message)]
      (into {}
            (for [e evals
                  r (get-in (objects/fields db e) [:evaluation/results :field/value])
                  :let [m (get-in r [:result :message])]
                  :when (and (= "send" (str (:op r))) (:ok r) (string? (not-empty (str m))))]
              [(send-key (:args r)) (str m)])))))

(defn- objects-with
  "Object ids of class c whose Field k holds v, with their created seq."
  [db c k v]
  (d/q '[:find ?id ?s :in $ ?c ?k ?v :where
         [?f :field/name ?k] [?f :field/value ?v]
         [?o :object/fields ?f] [?o :object/class ?c] [?o :object/id ?id] [?o :object/created-seq ?s]]
       db c k v))

(defn run-record
  "The kernel's record of a run Task, inline: exit, duration, the tails of its output, what failed."
  [db ran]
  (let [f (objects/fields db ran) v (fn [k] (get-in f [k :field/value]))
        n (if (= 0 (v :task/exit)) 5 12)
        tail (fn [x] (when (string? x) (let [ls (remove str/blank? (str/split-lines x))] (str/join "\n" (take-last n ls)))))
        out (fn [k k-tail] (or (tail (v k-tail)) (tail (v k))))]
    (cond-> {:task ran :exit (v :task/exit) :duration-ms (v :task/duration-ms) :command (v :task/command)}
      (out :task/stdout :task/stdout-tail) (assoc :stdout-tail (out :task/stdout :task/stdout-tail))
      (out :task/stderr :task/stderr-tail) (assoc :stderr-tail (out :task/stderr :task/stderr-tail))
      (v :task/failing) (assoc :failing (v :task/failing))
      (v :task/timed-out) (assoc :timed-out true))))

(defn thread-record
  "Every op with a result anywhere in this Message's thread: the ok results of the
   Principal's earlier wakes for Messages of the thread, and the ops its approval Tasks in the thread
   executed (an approved run is a result the model asked for and must not ask for again: seen live,
   a developer re-issued the same grep after every approval result, a card each time). Each is
   {:op :args :ok true :result :seq} like a result of this wake, so repeat-of answers from it."
  [db p message]
  (if-not message
    []
    (let [thread (thread/thread-of db message)
          ;; since the person last spoke: a conversation is one thread for hours, and asking for the
          ;; tests again must run them again, not answer from an hour ago
          msgs (set (thread/current-exchange db thread))
          from-evals (for [m msgs
                           [e s] (objects-with db :evaluation :evaluation/message m)
                           :when (= p (:object/created-by (objects/entity db e)))
                           r (get-in (objects/fields db e) [:evaluation/results :field/value])
                           :when (and (:ok r) (not (:repeated r)))]
                       [s r])
          from-tasks (for [m msgs
                           [t s] (objects-with db :task :task/message m)
                           :let [f (objects/fields db t) v (fn [k] (get-in f [k :field/value]))]
                           :when (and (= "approval" (v :task/kind)) (= p (v :task/requested-by))
                                      (#{:done :approved} (some-> (v :task/state) name keyword)))
                           :let [{:keys [op args]} (v :task/op)
                                 ran (v :task/ran)]]
                       [s {:op (str op) :args args :ok true :seq s
                           :result (cond-> {:approved t :note "approved and executed earlier in this thread; this is its result"}
                                     (uuid? ran) (merge (run-record db ran))
                                     (map? (v :task/result)) (assoc :outcome (:text (v :task/result))))}])]
      ;; in the order they happened: repeat-of walks back to the last change, and a build that failed
      ;; must not answer for the build after the fix
      (mapv second (sort-by first (concat from-evals from-tasks))))))

(defn- re-read
  "The result of this same read in what the model can still see, with nothing but reads since: the
   Object is as it was and the model has it. Seen live: one file read four times, then the edit
   claimed, not made. Only the last turn's results come back whole, so a read two turns old is gone
   and reading it again is right: told `it is above`, GLM reported the file's content never shown."
  [shown op args]
  (when (= :read op)
    (let [a (fieldable args)
          reading? (fn [d] (#{"read" "resolve" "history"} (str (:op d))))]
      (some (fn [d] (when (and (= "read" (str (:op d))) (= a (:args d)) (not (:repeated d))) d))
            (take-while reading? (reverse shown))))))

(defn- pending-result? [r] (= :approval-pending (some-> (get-in r [:err :code]) name keyword)))

(defn execute!
  "Run parsed ops in order as p. Returns results, one per op, in order:
   {:op :args :ok true :result :seq} or {:op :args :ok false :err {:code :message}}."
  ([world p via parsed-ops] (execute! world p via 1 parsed-ops {}))
  ([world p via hop parsed-ops] (execute! world p via hop parsed-ops {}))
  ([world p via hop parsed-ops {:keys [max-ops message route thread done shown]}]
  (let [ctx (loop-ctx world p via hop thread message route)
        db (act/db world)
        ;; what this wake did so far, plus what the thread already holds a result for
        wake-done (vec (or done []))
        done (into (vec (thread-record db p message)) wake-done)
        max-ops (long (or max-ops default-max-ops))
        ;; a send repeated within the wake, by an earlier iteration or earlier in this block, is
        ;; the same Message: answered with its id and duplicate true, never sent twice
        sent (atom (prior-sends db message))]
    (let [run-one
        (fn [i {:keys [op args raw err]}]
          ;; decided once: a mode ending between two looks must not run an op with no Task
          (let [args (cond->> (gates/pane-form op args) op (code/qualified (act/db world) p op))
                admitted (delay (when-not (:scratch world) (gates/admission (act/db world) p op args message)))]
            (cond
              err {:op (or (some-> op name) raw) :ok false :err err}
              ;; an off-the-record session: only what stays inside it (knowledge.world.scratch.policy)
              (and (:scratch world) (policy/refused world db op args))
              {:op (name op) :args (fieldable args) :ok false :err (fieldable (policy/refused world db op args))}
              (>= i max-ops) {:op (name op) :args (fieldable args) :ok false
                              :err {:code :invalid :message (str "more than " max-ops " ops in one iteration (loop/max-ops)")}}
              ;; an op that makes something (a named action, send, present) identical to one that already
              ;; succeeded in this wake is not run again: the model is told, with the earlier result (seen
              ;; live: the same view sent three times in three iterations)
              (repeat-of done op args)
              (let [d (repeat-of done op args)]
                {:op (name op) :args (fieldable args) :ok true :repeated true
                 :result (assoc (if (map? (:result d)) (:result d) {:result (:result d)})
                                :repeated true
                                :note (str "already done " (if (:approved (:result d)) "earlier in this thread (approved and run)" "this wake")
                                           " at seq " (:seq d) "; not run again, do not repeat it: use this result"))
                 :seq (:seq d)})
              (re-read (or shown wake-done) op args)
              {:op "read" :args (fieldable args) :ok true :repeated true
               :result {:note "unchanged since you read it this wake: it is above. Act on it, or answer."}}
              (and (= :send op) (get @sent (send-key args)))
              {:op "send" :args (fieldable args) :ok true
               :result {:message (str (get @sent (send-key args))) :duplicate true
                        :note "already sent in this wake; its answer, when it comes, wakes you"}}
              ;; an action that cannot run (schema, delegate, not narrower) is refused here,
              ;; before the gate: a person never approves an op that then fails (seen live three times)
              (and (= :act op) (map? args) (:action args) (nil? (:object args)) (actions/preflight world db p args))
              (let [e (actions/preflight world db p args)]
                {:op (name op) :args (fieldable args) :ok false :err (fieldable (:err e))})
              (and (= :grant op) (map? args) (not (:scratch world)) (not (act/grant-resolves? db p args)))
              {:op (name op) :args (fieldable args) :ok false :err (fieldable (:err act/unresolved))}
              ;; approval gates (decisions 2026-10-30): a gated op becomes a pending Task and the wake ends
              ;; (loop.core wait-for-approval!); a person decides it later (task.approve, task.deny)
              (and (not (:scratch world)) (gates/gated? db p op args) (not @admitted))
              (let [e (gates/gate! world p {:op op :args args :via via :message message :route route})]
                {:op (name op) :args (fieldable args) :ok false :err (fieldable e)})
              :else
              ;; a person's standing rule or unsafe mode lets a gated op run now, with a Task as its record
              (let [rule @admitted
                    {:keys [ok err] :as r} (try (if rule
                                                  (gates/admit! world p rule {:op op :args args :via via :message message})
                                                  (ops/handle ctx op args))
                                                (catch Throwable t {:err {:code :invalid :message (str (ex-message t))}}))]
                (if err
                  {:op (name op) :args (fieldable args) :ok false :err (fieldable err)}
                  (do (when (and (= :send op) (uuid? (:message ok))) (swap! sent assoc (send-key args) (str (:message ok))))
                      (cond-> {:op (name op) :args (fieldable args) :ok true :result (fieldable (codec/plain (dissoc ok :seq)))}
                        (:seq ok) (assoc :seq (:seq ok)))))))))
        ]
      ;; a block runs in order and a card stops it: seen live, three alternative edits in one block
      ;; raised three cards, and approving each applied all three
      (first
        (reduce (fn [[results i] parsed]
                  (let [result (if (some pending-result? results)
                                 {:op (or (some-> (:op parsed) name) (:raw parsed)) :args (fieldable (:args parsed)) :ok false
                                  :err {:code :held :message "not run: it stood behind an op waiting for a person. Ask again after the decision if it is still needed."}}
                                 (run-one i parsed))]
                    [(conj results result) (inc i)]))
                [[] 0]
                parsed-ops))))))
