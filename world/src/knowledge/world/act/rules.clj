(ns knowledge.world.act.rules
  "Standing approval rules (decisions 2026-10-30). A rule on a Principal's policy/approval-rules lets
   an otherwise gated op run without a card; only a person makes one. A rule is
   {:id :action :match :until :uses-left :used :by :at :note}, all plain strings and numbers.

   A shell command is matched without a shell: it is tokenized strictly, and anything that would need
   a shell to mean what it says (an operator, a substitution, a redirect, a subshell) matches no rule
   and gets a card. The matcher answers why, so a person can see it before saving a rule. It fails
   towards a card: refusing a harmless command costs one click, admitting a harmful one costs the gate."
  (:require [clojure.string :as str]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.resolve.core :as resolve])
  (:import [java.nio.file Files LinkOption Path Paths]
           [java.time Instant]
           [java.util Date]))

(def rules-field :policy/approval-rules)
(def rules-type [:list :map])
(def shell-words #{"task.run" "shell"})

;; ---------------------------------------------------------------- the strict tokenizer

(def ^:private refused-chars
  "Anywhere in one stage, quoted or not: ; & | > < $ ` newline, a subshell, and the backslash that
   would hide any of them. A read-only pipeline is split first (match-command), stage by stage."
  #"[;&|><$`\n\r()\\]")

(defn tokenize
  "{:tokens [{:text :quoted?}]} or {:refuse reason}. Single and double quotes group; nothing expands."
  [cmd]
  (cond
    (not (string? cmd)) {:refuse "no command"}
    (str/blank? cmd) {:refuse "empty command"}
    (re-find refused-chars cmd) {:refuse (str "contains " (pr-str (re-find refused-chars cmd)) ", which needs a shell")}
    :else
    (loop [cs (seq cmd) cur nil quoted false q nil out []]
      (let [flush (fn [] (if cur (conj out {:text cur :quoted? quoted}) out))]
        (cond
          (nil? cs) (if q {:refuse "unterminated quote"} {:tokens (flush)})
          q (if (= (first cs) q)
              (recur (next cs) cur quoted nil out)
              (recur (next cs) (str cur (first cs)) quoted q out))
          (#{\' \"} (first cs)) (recur (next cs) (or cur "") true (first cs) out)
          (Character/isWhitespace ^char (first cs)) (recur (next cs) nil false nil (flush))
          (#{\{ \}} (first cs)) {:refuse "brace expansion needs a shell"}
          :else (recur (next cs) (str cur (first cs)) quoted nil out))))))

;; ---------------------------------------------------------------- presets

(def read-only-programs #{"ls" "cat" "head" "tail" "wc" "grep" "rg" "find" "file" "stat" "du" "tree" "pwd" "git" "sed"})
(def read-only-git #{"log" "diff" "status" "show" "blame"})
(def test-programs #{"make" "ctest" "cargo" "npm" "pytest"})

(def ^:private forbidden-flags
  "Flags that turn a reading program into a writing or executing one."
  {"find" #{"-exec" "-execdir" "-delete" "-ok" "-okdir" "-fprint" "-fprint0" "-fprintf" "-fls"}
   "rg" #{"--pre" "--pre-glob" "--hostname-bin"}
   "tree" #{"-o" "--output"}
   "file" #{"-C" "--compile"}
   "ctest" #{"-S" "-SP" "-SR" "-D" "-M" "-T" "--script" "--build-and-test" "--build-and-test-options"}
   "pytest" #{"-p" "-c" "--rootdir" "--confcutdir"}})

(defn- flag-of [t] (first (str/split t #"=" 2)))

(defn- forbidden-flag [program args]
  (some (fn [t] (when (contains? (get forbidden-flags program #{}) (flag-of t)) t)) args))

(defn- git-problem [args]
  (cond
    (empty? args) "git needs a subcommand"
    (not (read-only-git (first args))) (str "git " (first args) " is not read-only")
    :else (some (fn [t]
                  (when (or (#{"-c" "-o" "--ext-diff" "--textconv"} (flag-of t))
                            (str/starts-with? t "--output")
                            (str/starts-with? t "--exec"))
                    (str "git flag " t " is refused")))
                args)))

(defn- sed-problem
  "Only `sed -n` with a line address and `p`: any other script can write or run. `-i` never fits."
  [args]
  (when-not (and (= "-n" (first args)) (second args)
                 (re-matches #"(\d+|\$)(,(\d+|\$))?p" (second args))
                 (not-any? #(str/starts-with? % "-") (drop 2 args)))
    "only sed -n with a line range and p is read-only"))

(defn- preset-problem
  "nil when program and args fit the preset, else why not."
  [preset program args]
  (case (str preset)
    "read-only"
    (cond
      (not (read-only-programs program)) (str program " is not in the read-only preset")
      (= program "git") (git-problem args)
      (= program "sed") (sed-problem args)
      (forbidden-flag program args) (str "flag " (forbidden-flag program args) " is refused for " program)
      :else nil)
    "test"
    (cond
      (not (test-programs program)) (str program " is not in the test preset")
      (forbidden-flag program args) (str "flag " (forbidden-flag program args) " is refused for " program)
      (= program "make") (let [targets (remove #(str/starts-with? % "-") args)]
                           (cond (empty? targets) "make needs a test target"
                                 (every? #(re-matches #"test(-[\w.-]+)?" %) targets) nil
                                 :else (str "make target " (first targets) " is not test or test-*")))
      (#{"cargo" "npm"} program) (when-not (= "test" (first args)) (str program " " (first args) " is not a test run"))
      :else nil)
    (str "unknown preset " preset)))

;; ---------------------------------------------------------------- paths

(defn- ^Path ->path [s] (Paths/get (str s) (make-array String 0)))

(defn- sensitive?
  "A credential by its name: the shell may be rooted where keys live (the harness's own repository)."
  [s]
  (some (fn [seg] (or (#{".env" ".envrc" ".ssh" ".aws" ".netrc" ".npmrc" ".pypirc"} seg)
                      (str/starts-with? seg ".env.")
                      (re-find #"\.(token|pem|key)$" seg)
                      (str/starts-with? seg "id_rsa") (str/starts-with? seg "id_ed25519")))
        (str/split s #"/")))

(defn- candidates
  "Every string in a token that could name a path: the token, and the value inside an option."
  [t]
  (distinct
    (cond-> [t]
      (and (str/starts-with? t "-") (str/includes? t "=")) (conj (second (str/split t #"=" 2)))
      (re-matches #"-[A-Za-z].+" t) (conj (subs t 2)))))

(defn- inside? [^Path root ^Path p] (.startsWith p root))

(defn- real-inside?
  "When the path exists, its real location is inside the real root: a symlink does not leave."
  [^Path root ^Path p]
  (or (not (Files/exists p (make-array LinkOption 0)))
      (inside? (.toRealPath root (make-array LinkOption 0)) (.toRealPath p (make-array LinkOption 0)))))

(defn- path-problem
  "Why a token leaves the Adapter's root, or nil."
  [^Path root ^Path cwd {:keys [text quoted?]}]
  (some (fn [c]
          (try
            (let [p (.normalize (.resolve cwd (->path c)))]
              (cond
                (str/starts-with? c "~") (str c " names a home directory")
                (sensitive? c) (str c " looks like a credential")
                (and (not quoted?) (re-find #"^\.+[*?\[]" c)) (str c " can expand to a parent directory")
                (not (inside? root p)) (str c " leaves the Adapter's root")
                (not (real-inside? root p)) (str c " is a link out of the Adapter's root")
                :else nil))
            (catch java.nio.file.InvalidPathException _ nil)))
        (candidates text)))

(defn- recursive-read?
  "Does this program read file contents across a tree? find and tree only list names."
  [program args]
  (or (= "rg" program)
      (and (= "grep" program)
           (some #(or (= "--recursive" %) (= "--dereference-recursive" %)
                      (re-matches #"-[A-Za-z]*[rR][A-Za-z]*" %))
                 args))))

(defn- sensitive-near
  "The first credential by name within two levels of dir, or nil: a recursive read cannot be told
   to skip it, because the matcher never rewrites a command."
  [^Path dir]
  (letfn [(walk [^java.io.File f depth]
            (when-let [kids (and (pos? depth) (.listFiles f))]
              (or (some #(when (sensitive? (.getName ^java.io.File %)) (.getName ^java.io.File %)) kids)
                  (some #(when (and (.isDirectory ^java.io.File %) (not= ".git" (.getName ^java.io.File %)))
                           (walk % (dec depth)))
                        kids))))]
    (walk (.toFile dir) 2)))

(defn- outside-paths
  "A token that exists and lies outside the rule's paths, or nil; a pattern that names no file is not a path."
  [^Path root ^Path cwd paths tokens]
  (let [allowed (mapv #(.normalize (.resolve root (->path %))) paths)
        in? (fn [^Path p] (some #(.startsWith p ^Path %) allowed))]
    (if-not (in? cwd)
      (str "the working directory is outside " (pr-str paths))
      (some (fn [{:keys [text]}]
              (let [p (.normalize (.resolve cwd (->path text)))]
                (when (and (Files/exists p (make-array LinkOption 0)) (not (in? p)))
                  (str text " is outside " (pr-str paths)))))
            tokens))))

;; ---------------------------------------------------------------- matching a command

(defn- match-stage
  "Does one shell command, no pipeline, fit this match? {:matches bool :reason text}. root is the
   Adapter's locator, cwd the directory the run would use, relative to root or absolute."
  [{:keys [programs preset args-prefix paths]} {:keys [cmd root cwd]}]
  (let [{:keys [tokens refuse]} (tokenize cmd)
        no (fn [reason] {:matches false :reason reason})]
    (cond
      refuse (no refuse)
      (empty? tokens) (no "empty command")
      (and (empty? programs) (nil? preset)) (no "the rule names no program and no preset")
      :else
      (let [program (:text (first tokens))
            args (mapv :text (rest tokens))
            named? (contains? (set programs) program)
            ^Path root-path (.normalize (.toAbsolutePath (->path root)))
            ^Path cwd-path (.normalize (.resolve root-path (->path (or (not-empty cwd) "."))))]
        (cond
          (str/includes? program "/") (no (str program " names a path, not a program"))
          (str/includes? program "=") (no "an environment assignment needs a shell")
          (and (not named?) (nil? preset)) (no (str program " is not named by the rule"))
          (and (not named?) preset (preset-problem preset program args)) (no (preset-problem preset program args))
          (and named? (forbidden-flag program args)) (no (str "flag " (forbidden-flag program args) " is refused for " program))
          (and (seq args-prefix) (not= (vec args-prefix) (subvec args 0 (min (count args) (count args-prefix)))))
          (no (str "the arguments do not start with " (pr-str (vec args-prefix))))
          (not (inside? root-path cwd-path)) (no "the working directory leaves the Adapter's root")
          :else
          (if-let [problem (or (some #(path-problem root-path cwd-path %) (rest tokens))
                               (when (seq paths) (outside-paths root-path cwd-path paths (rest tokens)))
                               (when (recursive-read? program args)
                                 (when-let [secret (sensitive-near cwd-path)]
                                   (str "a recursive read here would reach " secret))))]
            (no problem)
            {:matches true :reason (if named? (str program " is named by the rule") (str "fits the " preset " preset"))}))))))

(defn- pipeline
  "The stages of a plain pipeline, or nil. A quote hiding a | splits badly and the stage then
   refuses as an unterminated quote, which fails towards a card."
  [cmd]
  (when (and (string? cmd) (str/includes? cmd "|") (not (str/includes? cmd "||")))
    (mapv str/trim (str/split cmd #"\|" -1))))

(defn- without-quiet-stderr
  "A preset's command with `2>/dev/null` and `2>&1` dropped: neither writes a file, and every model
   writes them, so each such read cost a card and ended its wake (seen live on most tasks). Only the
   match sees this; the command runs as written."
  [{:keys [preset]} cmd]
  (if (and (#{"read-only" "test"} preset) (string? cmd))
    (str/replace cmd #"\s+2>\s*(/dev/null|&1)(?=\s|$)" "")
    cmd))

(defn- without-leading-cd
  "`cd dir && command` as the command in dir, for a preset: models write it for most runs. dir is held
   to the root like any cwd; anything a shell would expand fails towards a card."
  [{:keys [preset]} {:keys [cmd cwd] :as run}]
  (let [[_ dir more] (when (and (#{"read-only" "test"} preset) (string? cmd))
                       (re-matches #"(?s)\s*cd\s+([^\s;&|~$`*?]+)\s*&&\s*(.+)" cmd))]
    (if dir
      (assoc run :cmd more :cwd (if (str/starts-with? dir "/") dir (str (or (not-empty cwd) ".") "/" dir)))
      run)))

(defn match-command
  "Does the shell command fit this match? {:matches bool :reason text}. A pipeline fits only the
   read-only preset, every stage on its own against the preset alone: `grep x . | head` needs no
   card, and a stage a named program would admit does not get in that way."
  [{:keys [preset] :as match} {:keys [cmd] :as run}]
  (let [run (without-leading-cd match (assoc run :cmd (without-quiet-stderr match cmd)))
        cmd (:cmd run)]
  (if-let [stages (pipeline cmd)]
    (if-not (#{"read-only" "test"} preset)
      {:matches false :reason "contains \"|\", which needs a shell"}
      ;; a test run may be filtered, `make test | tail`: what follows the first stage only reads
      (let [stage-match (dissoc match :programs :args-prefix)
            filter-match (assoc stage-match :preset "read-only")
            results (map-indexed (fn [i stage] (match-stage (if (zero? i) stage-match filter-match) (assoc run :cmd stage)))
                                 stages)]
        (or (first (remove :matches results))
            {:matches true :reason (str "every stage fits the " preset " preset")})))
    (match-stage match run))))

(defn infer-match
  "The narrowest useful match for a command a person just approved: its program, with the first
   argument when that is a subcommand."
  [cmd]
  (let [{:keys [tokens]} (tokenize cmd)]
    (when-let [program (:text (first tokens))]
      (cond-> {:programs [program]}
        (#{"git" "cargo" "npm"} program) (assoc :args-prefix (vec (take 1 (map :text (rest tokens)))))))))

;; ---------------------------------------------------------------- the rules of a Principal

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))

(defn rules [db p]
  (let [v (field db p rules-field)] (if (sequential? v) (vec v) [])))

(defn- live? [{:keys [until uses-left]} ^Date now]
  (and (or (nil? uses-left) (pos? uses-left))
       (or (nil? until)
           (try (.isAfter (Instant/parse (str until)) (.toInstant now)) (catch Exception _ false)))))

(defn word-fits? [rule-action word]
  (or (= (str rule-action) word) (and (shell-words (str rule-action)) (shell-words word))))

(defn run-of
  "{:cmd :cwd :adapter} of a gated run: task.run or act run on a host Adapter; else nil."
  [db p op args]
  (when (= :act (keyword (name op)))
    (let [action (some-> (:action args) name)
          inner (if (map? (:args args)) (:args args) {})
          handler (when action
                    (when-let [id (objects/lookup db (objects/world-id db) (str "action/" action))]
                      (some-> (field db id :action/handler) name)))]
      (cond
        (or (= action "task.run") (= handler "task.run"))
        {:cmd (:cmd inner) :cwd (:cwd inner)
         :adapter (or (->uuid (:adapter inner)) (resolve/lookup db p (str (:adapter inner))))}
        (= action "run")
        {:cmd (:cmd inner) :cwd (:cwd inner) :adapter (->uuid (:object args))}
        ;; a terminal is a command only when it is a whole line at a prompt the world can vouch for:
        ;; raw keys, a partial line, or a pane anyone has typed freely in have nothing to match
        (= action "pane.send")
        (let [pane (->uuid (:pane inner))
              text (:text inner)]
          (when (and pane (string? text) (nil? (:keys inner)) (not (field db pane :pane/dirty))
                     (re-matches #"[^\r\n\x00-\x1f\x7f]+[\r\n]" text))
            {:cmd (str/trim text) :cwd nil :adapter (field db pane :pane/target)}))))))

(defn- person-rule? [db {:keys [by]}] (authority/person? db by))

(defn- fits?
  "Does the op fit the rule? Only a shell run or a mount-backed write has anything to match on; any
   other gate word admits on the rule's action alone."
  [db p {:keys [match]} word op args]
  (cond
    (shell-words word)
    (let [{:keys [cmd cwd adapter]} (run-of db p op args)
          root (when adapter (field db adapter :adapter/locator))]
      (and root (string? cmd) (:matches (match-command match {:cmd cmd :root root :cwd cwd}))))
    (= "fs:write" word)
    (let [path (or (field db (->uuid (:object args)) :file/path)
                   (some-> (get-in args [:args :name]) str (str/replace #"^[^/]*/" "")))
          paths (:paths match)]
      (or (empty? paths) (and path (some #(str/starts-with? (str path) (str %)) paths))))
    :else true))

(defn admit
  "The first live rule of p, authored by a person, that lets this gated op through, or nil."
  [db p word op args]
  (let [now (Date.)]
    (first (filter (fn [r] (and (word-fits? (:action r) word) (live? r now) (person-rule? db r)
                                (fits? db p r word op args)))
                   (rules db p)))))

(defn- root-of [db] (objects/lookup db (objects/world-id db) "root"))

(defn- write-rules!
  "The rules Field, as kernel bookkeeping under root: the person's authority was checked at the action."
  [world p rs]
  (act/set-field! world (root-of (act/db world)) {:object p :field rules-field :type rules-type :value (vec rs) :reader :both}))

(defn add!
  "A rule on p by person. Returns the rule."
  [world person p {:keys [action match until uses note]}]
  (let [rule (cond-> {:id (str "r-" (subs (str (random-uuid)) 0 8)) :action (str action) :match (or match {})
                      :used 0 :by (str person) :at (str (Instant/now))}
               until (assoc :until (str until))
               uses (assoc :uses-left (long uses))
               note (assoc :note (str note)))]
    (write-rules! world p (conj (rules (act/db world) p) rule))
    rule))

(defn remove!
  "Drop rule id from p. true when it was there."
  [world p id]
  (let [rs (rules (act/db world) p)
        kept (remove #(= (str id) (:id %)) rs)]
    (when (not= (count rs) (count kept))
      (write-rules! world p kept)
      true)))

(defn used!
  "Count one use of the rule."
  [world p id]
  (write-rules! world p (map (fn [r] (if (= id (:id r))
                                       (cond-> (update r :used (fnil inc 0))
                                         (:uses-left r) (update :uses-left dec))
                                       r))
                             (rules (act/db world) p))))

(defn valid-match
  "Why this rule cannot work, or nil."
  [action {:keys [programs preset]}]
  (cond
    (str/blank? (str action)) "a rule needs an action"
    (and (shell-words (str action)) (empty? programs) (nil? preset))
    "a shell rule names `programs` or a `preset`"
    (and preset (not (#{"read-only" "test"} (str preset)))) (str "unknown preset " preset)
    :else nil))
