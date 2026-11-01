(ns knowledge.world.loop.worklog
  "A wake's working memory. Only the newest turn's results come back whole, so a 30-turn wake read a
   file, lost it two turns later and read it again until the turns ran out, with nothing written
  . In a wake that asks for a change the kernel keeps this
   note instead: what was read, what was changed, what ran and how it ended. It goes in the prompt
   every turn, inside a fixed size, and it decides when reading has gone on long enough. Pure: the
   loop owns the state."
  (:require [clojure.string :as str]))

(def open-chars "Of files read, how much of the newest stays verbatim in the prompt." 5000)
(def max-tail "Lines kept of a run's output." 12)
(def act-after "Turns with no change and no run before the note says to act." 5)
(def force-after "Turns before only changing tools are left, and one must be called." 7)
(def write-tools #{"edit" "run" "test"})

(defn work?
  "Does the Message ask for a change? A question stays a question, with its small prompt."
  [text]
  (boolean (re-find #"(?i)\b(fix|add|write|make|change|implement|create|update|remove|delete|refactor|rename|build|run the tests?|migrate)\b"
                    (str text))))

(defn- text-of
  "The string inside a read's result, and where it came from."
  [{:keys [args result]}]
  (let [fields (:object/fields result)
        v (fn [k] (get-in fields [k :field/value]))]
    (when (string? (v :file/text))
      {:name (str (or (:name args) (v :file/path) (:object args)))
       :text (v :file/text)
       :range (v :file/range)
       :lines (v :file/lines)})))

(defn- action [{:keys [op args]}]
  (when (= "act" (some-> op name)) (some-> (:action args) name)))

(defn- inner [{:keys [result]}]
  (if (and (map? result) (map? (:result result))) (:result result) result))

(defn- tail-of [s]
  (when (string? s)
    (let [ls (remove #(re-find #"^\s*$" %) (str/split-lines s))]
      (str/join "\n" (take-last max-tail ls)))))

(defn- run-key
  "What makes two runs the same: the command, or the `test` tool's own."
  [r]
  (let [res (inner r)]
    (str (or (:command res) (get-in r [:args :args :cmd]) (get-in r [:args :cmd]) (action r)))))

(def ^:private ran-nothing
  "A runner that exits 0 having run no test. Seen live: `--focus` on a name kaocha does not load, all
   591 skipped, reported as verified."
  #"(?i)all \d+ tests were skipped|no tests ran|\b0 tests\b|collected 0 items|no test files")

(def ^:private window-lines "Lines kept each side of an edit, in a file too long to hold whole." 40)

(defn- around
  "The file as the log holds it after an edit: whole if it fits, else the lines around the new text.
   Seen live: a changed file left the note, the next `old` was a guess, and two of six runs ended on
   no-match."
  [{:keys [text lines] :as current} new i room]
  (let [at (when (seq new) (str/index-of text new))]
    (if (<= (count text) room)
      (assoc current :turn i)
      (let [ls (vec (str/split-lines text))
            line (if at (inc (count (filter #{\newline} (subs text 0 at)))) 1)
            from (max 1 (- line window-lines))
            to (min (count ls) (+ line window-lines -1 (count (str/split-lines (str new)))))]
        {:name (:name current) :lines lines :turn i :range [from to]
         :text (str/join "\n" (subvec ls (dec from) to))}))))

(defn- held-as?
  "Does this name in the log mean the file an edit located? Models write a name bare or by its mount."
  [held located]
  (or (= held located) (str/ends-with? (str located) (str "/" held))))

(defn- add-one [log i {:keys [ok err] :as r}]
  (let [a (action r)
        res (inner r)]
    (cond
      (and ok (text-of r))
      (let [{:keys [name] :as f} (text-of r)]
        (-> log
            (assoc-in [:files name] (assoc f :turn i))
            (update :order #(conj (vec (remove #{name} %)) name))))

      (and ok (= "file.edit" a))
      (let [name (str (or (:name res) (get-in r [:args :args :name])))
            stale (filter #(held-as? % name) (:order log))
            ;; what was read of it is out of date: the file as it now stands takes its place
            current (some-> (:current r) (around (get-in r [:args :args :new]) i (:open-chars log open-chars)))
            log (-> log
                    (update :edits (fnil conj []) {:turn i :name name :diff (some-> (:diff res) str)})
                    (update :files #(apply dissoc % stale))
                    (update :order #(vec (remove (set stale) %))))]
        (cond-> log
          current (-> (assoc-in [:files name] (assoc current :name name))
                      (update :order conj name))))

      (and ok (map? res) (contains? res :exit))
      (update log :runs (fnil conj [])
              {:turn i :cmd (str (or (:command res) (get-in r [:args :args :cmd])))
               :exit (:exit res) :task (:task res) :failing (:failing res) :key (run-key r) :box (:box res)
               :no-tests (boolean (re-find ran-nothing (str (:stdout res) "\n" (:stderr res) "\n"
                                                            (:stdout-tail res) "\n" (:stderr-tail res))))
               :timed-out (or (:timed-out res) (= -1 (:exit res)))
               :out (or (tail-of (:stdout res)) (:stdout-tail res))
               :err (or (tail-of (:stderr res)) (:stderr-tail res))})

      (and ok (#{"code.outline" "code.search"} a))
      (update log :looks (fnil conj [])
              (str a " " (or (get-in r [:args :args :pattern]) (get-in r [:args :args :name]) "") ))

      ;; a card is not a refusal: its result comes back with the decision
      (and err (= "file.edit" a) (not (#{"approval-pending" "held"} (some-> (:code err) name))))
      (update log :failed (fnil conj []) (str "turn " i ": " (get-in r [:args :args :name]) ": " (:message err)))

      :else log)))

(def ^:private looking
  "A command that only looks: `sed -n` and `wc` in a shell are reading by another name."
  #"^\s*(cd [^;&|]+(&&|;)\s*)?(ls|cat|head|tail|wc|grep|rg|find|sed -n|awk|git (log|status|diff|show)|file|stat|pwd)\b")

(defn- looks? [cmd] (boolean (re-find looking (str cmd))))

(defn- acted? [{:keys [ok] :as r}]
  (and ok (or (= "file.edit" (action r))
              (let [res (inner r)]
                (and (map? res) (contains? res :exit)
                     (not (re-find looking (str (or (:command res) (get-in r [:args :args :cmd])))))))))) 

(def ^:private fix-turns "Turns held to fixing before the wake is let go: some failures are not the code's." 3)

(defn- real-runs [{:keys [runs]}] (remove #(looks? (:cmd %)) runs))

(defn- passed? [{:keys [exit no-tests]}] (and (= 0 exit) (not no-tests)))

(defn failed-run
  "The build or test that failed with nothing changed since, or nil. A `grep` with no match is an
   answer, not a failure, and a failure before any change is a finding to report, not to fix."
  [{:keys [edits] :as log}]
  (when-let [r (last (real-runs log))]
    (when (and (seq edits) (not (passed? r)) (not (:timed-out r))
               (>= (long (:turn r)) (long (:turn (peek (vec edits))))))
      r)))

(defn add
  "The log after a turn's results. Turns count across a thread's wakes: a card ends a wake, not the work."
  [log results]
  (let [log (merge {:files {} :order [] :n 0} log)
        n (inc (long (:n log)))
        was-failing (failed-run log)
        log (reduce #(add-one %1 n %2) (assoc log :n n) results)]
    (-> log
        (update :quiet #(if (some acted? results) 0 (inc (long (or % 0)))))
        (update :fixing #(if (and was-failing (failed-run log)) (inc (long (or % 0))) 0)))))

(defonce ^:private kept (atom {}))
(def ^:private kept-threads 64)

(defn recall "The log of a Principal's thread, if this process has one." [k] (get @kept k))

(defn remember
  "Keep the log for the next wake of the thread. In memory: after a restart the log on disk still
   says what happened, and a fresh read costs little. Returns log."
  [k log]
  (swap! kept (fn [m] (let [m (assoc m k (assoc log :at (System/currentTimeMillis)))]
                        (if (> (count m) kept-threads)
                          (dissoc m (key (apply min-key (comp :at val) m)))
                          m))))
  log)

(defn open?
  "Is this op a whole read of a file the prompt already holds, unchanged?"
  [log {:keys [op args]}]
  (and (= "read" (some-> op name)) (string? (:name args)) (not (:range args))
       (when-let [f (get-in log [:files (:name args)])]
         (and (nil? (:range f)) (:open f)))))

(defn- fit
  "The newest files verbatim while they fit, the older by name. Marks what stays open. The log's
   :open-chars is what this Principal's Model has room for."
  [{:keys [files order] :as log}]
  (loop [names (reverse order) left (:open-chars log open-chars) out files]
    (if-let [n (first names)]
      (let [c (count (:text (get files n)))
            open (<= c left)]
        (recur (next names) (if open (- left c) left) (assoc-in out [n :open] open)))
      (assoc log :files out))))

(defn settle
  "The log with what is open worked out, for open? and render alike."
  [log] (if log (fit log) log))

(defn hold-reading?
  "Has this wake read long enough that its reading tools should wait a turn?"
  [{:keys [quiet] :as log}]
  (>= (long (or quiet 0)) (long (:force-after log force-after))))

(defn fixing?
  "A build or test failed and nothing has changed: the next turn fixes it."
  [{:keys [fixing] :as log}]
  (boolean (and (failed-run log) (< (long (or fixing 0)) fix-turns))))

(defn held-tools
  "The tools this turn is limited to, or nil. Fixing keeps `read`: an edit needs the failing lines."
  [log]
  (cond (fixing? log) #{"edit" "read"}
        (hold-reading? log) write-tools))

(defn repeated-run
  "An op that runs the same failed command again with nothing changed: the failing lines, or nil."
  [log {:keys [op args]}]
  (when-let [{:keys [key cmd exit failing]} (failed-run log)]
    (let [action (some-> (:action args) name)
          same (str (or (get-in args [:args :cmd]) (:cmd args) action))]
      (when (and (= "act" (some-> op name)) (= same key))
        (str "`" cmd "` already failed with exit " exit " and nothing has changed. Change the code first."
             (when failing (str "\n" failing)))))))

(defn untested
  "The code files changed with no build or test passing since the last change, or nil."
  [{:keys [edits] :as log} docs-re]
  (let [code (remove #(re-find docs-re (str (:name %))) edits)
        at (:turn (last code))]
    (when (and at (not-any? #(and (> (long (:turn %)) (long at)) (passed? %)) (real-runs log)))
      (vec (distinct (map :name code))))))

(defn report
  "What a wake out of turns leaves its asker, from the record alone: changed, verified, unfinished."
  [{:keys [edits failed] :as log} docs-re]
  (let [last-change (long (or (:turn (peek (vec edits))) 0))
        runs (real-runs log)
        passed (filter #(and (passed? %) (>= (long (:turn %)) last-change)) runs)
        broken (let [r (last runs)] (when (and r (not (passed? r))) r))
        untested (untested log docs-re)
        lines (fn [title xs] (str "\n" title ":\n" (str/join "\n" (map #(str "- " %) xs))))]
    (str (lines "Changed" (or (seq (distinct (map :name edits))) ["nothing"]))
         (lines "Verified" (or (seq (for [{:keys [cmd turn]} (take-last 3 passed)] (str "`" cmd "` exit 0, turn " turn)))
                               ["nothing ran and passed after the last change"]))
         (lines "Unfinished"
                (concat (when broken
                          [(str "`" (:cmd broken) "` " (cond (:timed-out broken) "timed out"
                                                 (:no-tests broken) "ran no tests"
                                                 :else (str "FAILED, exit " (:exit broken)))
                                (some->> (:failing broken) (str "\n")))])
                        (when (seq untested) [(str (str/join ", " (map #(str "`" % "`") untested)) " changed, and nothing passed after that")])
                        (take-last 2 failed)
                        ["the work ran out of turns before an answer, so the goal is not shown to hold"])))))

(defn- phase [{:keys [edits runs quiet] :as log}]
  (let [quiet (long (or quiet 0))
        last-edit (or (:turn (peek (vec edits))) 0)
        last-run (or (:turn (last (real-runs log))) 0)]
    (cond
      (and (seq edits) (> last-edit last-run))
      "VERIFY: you changed something and have not run it. Build or test it now."
      (fixing? log)
      "FIX: the last run failed, and its failing lines are below. This turn change the code with edit, or read the lines you need. Running it again now is refused."
      (failed-run log)
      "STUCK: the same failure has outlasted three turns. Say what fails and why, plainly."
      (>= quiet (:force-after log force-after))
      "WRITE NOW: reading is closed this turn. Make the first change with edit, or say what stops you."
      (and (empty? edits) (>= quiet (:act-after log act-after)))
      "WRITE: you have read enough. State the plan in a line, then make the first change with edit. A new file is edit with `new` alone."
      (seq edits) "FINISH: the change has run, and a passing build is not the goal. Show the goal itself holds, then report, ending with `Proved: <what you ran or read, and what it showed>` or `Not proved: <why>`."
      :else "READ: find what the change touches, then change it.")))

(defn- run-block
  "A run for the note: the newest with what it said, an older one as its line. What failed says it
   once: the failing lines and not the tails they came from, which were a third of a build's note."
  [{:keys [turn cmd exit task out err failing no-tests box]} newest?]
  (str "- turn " turn ": `" cmd "` " (cond no-tests "exit 0, but it RAN NO TESTS: the command did not reach your test"
                                           (= 0 exit) "exit 0"
                                           (= -1 exit) "TIMED OUT: run it again with a longer timeout-s, or a narrower command"
                                           :else (str "FAILED, exit " exit))
       (when newest?
         (str (when (seq box) (str "\n" box))
              (when (seq failing) (str "\nfailing lines:\n" failing))
              (when (and (empty? failing) (seq err)) (str "\nstderr:\n" err))
              (when (and (empty? failing) (seq out)) (str "\nstdout:\n" out))
              (when task (str "\nwhole output: read {object " task ", field task/stdout}"))))))

(defn render
  "The note for the prompt, or nil with nothing to say. goal is the task, in the asker's words: a
   wake a card started has only the decision for its Message."
  ([log i limit] (render log i limit nil))
  ([log i limit goal]
  (when log
    (let [{:keys [files order edits runs looks failed] :as log} (settle log)
          block (fn [title xs] (when (seq xs) (str "\n" title ":\n" (str/join "\n" xs))))
          open (for [n order :let [f (get files n)] :when (:open f)]
                 (str "--- " n (when (:range f) (str " lines " (pr-str (:range f)))) "\n" (:text f)))
          closed (for [n order :let [f (get files n)] :when (not (:open f))]
                   (str "- " n " (" (or (:lines f) "?") " lines, turn " (:turn f) "): read again with a range"))]
      (str "# Work so far, kept by the kernel: turn " i "/" limit
           (when (seq goal) (str "\nThe task: " goal))
           "\nNext: " (phase log)
           (when (seq runs) "\nIn `run`, paths start at the project's root and do not carry the mount's name.")
           (block "Changed" (let [recent (- (count edits) 2)]
                              (for [[n {:keys [turn name diff]}] (map-indexed vector edits)]
                                (str "- turn " turn ": " name (when (and diff (>= n recent)) (str "\n" (subs diff 0 (min 400 (count diff)))))))))
           (block "Ran" (let [shown (vec (take-last 4 runs))]
                          (map-indexed (fn [n r] (run-block r (= n (dec (count shown))))) shown)))
           (block "Refused" (take-last 2 failed))
           (block "Looked at" (take-last 4 (distinct looks)))
           (block "Read, not held here" closed)
           (block "Open files, current" open))))))
