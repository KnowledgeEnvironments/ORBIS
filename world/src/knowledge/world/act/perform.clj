(ns knowledge.world.act.perform
  "Actions on Objects other than create (ADR 0008, decisions 2026-10-30): `act {object action args}`.
   Two actions exist, both on Adapters (spec 5.10), both through resolution (ground rule 4):

   - `run` on a host Adapter (adapter/binds :host): the Principal must resolve the Adapter for
     :evaluate. The command runs through knowledge.world.adapters.shell in the Adapter's locator
     (or args cwd under it) and is recorded as a Task the Principal creates (spec 5.3): Fields
     task/command task/cwd task/adapter task/started-at, then task/exit task/stdout task/stderr
     task/stdout-total task/stderr-total task/duration-ms task/timed-out task/ended-at, moved
     open -> active -> done (any exit code) or cancelled (timeout). Output above the blob threshold
     is stored as bytes (a blob); the op result carries the Task id, the exit code and the first
     2 KiB of each stream so a loop sees it in the next iteration.
   - `index` on an fs Adapter (adapter/binds :filesystem): the Principal must resolve the Adapter
     for :read; harness/index and harness/head (knowledge.world.adapters.harness) are set on the
     Principal itself (:write on itself).
   - `cancel` on an Evaluation, or on a Principal for its current wake (decisions 2026-10-30):
     knowledge.world.evaluate.core/cancel!, resolution for :evaluate or :write on the target."
  (:refer-clojure :exclude [run!])
  (:require [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.adapters.harness :as harness]
            [clojure.string :as str]
            [knowledge.world.adapters.shell :as shell]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.loop.steer :as steer]
            [knowledge.world.resolve.core :as resolve])
  (:import [java.util Date]))

(def inline-cap 2048)

(defn- field [state k] (get-in state [:object/fields k :field/value]))

(defn- output-field
  "A string Field up to the blob threshold, else bytes (the log moves it to a blob)."
  [^String s]
  (let [bs (.getBytes s "UTF-8")]
    (if (<= (alength bs) fs/text-threshold) [:string s] [:bytes bs])))

(defn- clip [^String s n] (if (> (count s) n) (str (subs s 0 n) "\n[... " (- (count s) n) " more chars on the Task]") s))

(def tail-cap 1024)

(defn- tail
  "The last n chars of s when the head clip hid them (test counts live at the end
   of a run's output; the first 2 KiB alone showed none of them), else nil."
  [^String s head n]
  (when (> (count s) head) (subs s (max 0 (- (count s) n)))))

(defn- ok! [{:keys [ok err]}] (if err (throw (ex-info (str (:code err) " " (:message err)) {:err err})) ok))

(defn run-dir
  "The directory a run's cwd names. What a model is told a path looks like also works: the project's
   absolute path, and a path with the mount's name on the front."
  [locator cwd]
  (let [cwd (if (and (string? cwd) (string? locator) (str/starts-with? cwd locator))
              (str/replace (subs cwd (count locator)) #"^/+" "")
              cwd)
        dir (fs/safe-file locator (if (seq cwd) cwd "."))
        rest-of (when (string? cwd) (second (str/split cwd #"/" 2)))
        under (when (and dir (not (.exists ^java.io.File dir)) (string? cwd))
                (fs/safe-file locator (or rest-of ".")))]
    (if (and under (.isDirectory ^java.io.File under)) under dir)))

(defn run!
  "The run action. Returns {:ok {:result {...} :object task :seq}} or {:err}. grant is the Grant
   that resolved the Adapter for :evaluate: the Task's Fields and moves are the kernel's record of
   the run, appended under it as the Principal (like loop/* bookkeeping and route/members), so a
   Principal that may run a command need not hold :write on the Task it creates."
  [world p adapter-state {:keys [cmd cwd timeout-s]} via grant]
  (let [adapter (:object/id adapter-state)
        locator (field adapter-state :adapter/locator)
        dir (run-dir locator cwd)]
    (cond
      (not (string? cmd)) (act/err :invalid "run needs args {cmd}")
      (nil? dir) (act/err :invalid "cwd escapes the Adapter's locator")
      ;; a missing cwd reads as `sandbox-exec: no such file` once the box starts: seen live, the agent
      ;; reported a harness with no sandbox
      (not (.isDirectory ^java.io.File dir))
      (act/err :invalid (str "cwd `" cwd "` is no directory here: a run's paths start at the project's root and do not carry the mount's name"))
      (:err (unsafe/box-for world adapter p)) {:err (:err (unsafe/box-for world adapter p))}
      :else
      (try
        (let [object (random-uuid)
              ;; the Task is the kernel's record of the run, created under the Grant that admitted the run
              ;; (decisions 2026-10-30): a Principal that may run a command need not resolve schema.Task
              _ (act/append! world (log/pending :created object p
                                                {:class :task :fields {:task/owner p :task/state :open :task/blocked-on #{}
                                                                       :task/evidence #{} :task/title cmd}}
                                                :delegation grant :via via))
              put! (fn [k type v] (act/append! world (log/pending :field-set object p {:name k :type type :value v :reader :both}
                                                                  :delegation grant :via via)))
              state (atom :open)
              move! (fn [to] (let [seq (act/append! world (log/pending :task-moved object p {:task object :from @state :to to}
                                                                       :streams #{p} :delegation grant :via via))]
                               (reset! state to)
                               {:seq seq}))]
          (put! :task/command :string cmd)
          (put! :task/cwd :string (.getPath ^java.io.File dir))
          (put! :task/adapter [:ref :adapter] adapter)
          (put! :task/started-at :instant (Date.))
          (move! :active)
          (let [box (:box (unsafe/box-for world adapter p))
                {:keys [exit stdout stderr stdout-total stderr-total timed-out duration-ms]}
                (shell/run! {:cmd cmd :cwd (.getPath ^java.io.File dir) :timeout-s timeout-s :box box})
                [ot ov] (output-field stdout)
                [et ev] (output-field stderr)
                refused (shell/box-note stderr stdout box)]
            (put! :task/exit :number exit)
            (put! :task/stdout ot ov)
            (put! :task/stderr et ev)
            (put! :task/stdout-total :number stdout-total)
            (put! :task/stderr-total :number stderr-total)
            (put! :task/duration-ms :number duration-ms)
            (put! :task/timed-out :boolean timed-out)
            ;; over the blob threshold the output is bytes, and the next turn cannot quote bytes
            (when-let [failing (and (not= 0 exit) (shell/failing-lines stderr stdout))]
              (put! :task/failing :string failing))
            (when-let [t (tail stdout inline-cap tail-cap)] (put! :task/stdout-tail :string t))
            (when-let [t (tail stderr (quot inline-cap 2) (quot tail-cap 2))] (put! :task/stderr-tail :string t))
            (when refused (put! :task/box :string refused))
            (put! :task/ended-at :instant (Date.))
            (let [{:keys [seq]} (move! (if timed-out :cancelled :done))]
              {:ok {:result (cond-> {:task object :exit exit :state (if timed-out "cancelled" "done") :timed-out timed-out
                                     :duration-ms duration-ms :stdout (clip stdout inline-cap) :stderr (clip stderr (quot inline-cap 2))
                                     :stdout-total stdout-total :stderr-total stderr-total}
                              (not= 0 exit) (assoc :outcome (str "FAILED, exit " exit))
                              refused (assoc :box refused)
                              (and (not= 0 exit) (shell/failing-lines stderr stdout))
                              (assoc :failing (shell/failing-lines stderr stdout))
                              (tail stdout inline-cap tail-cap) (assoc :stdout-tail (tail stdout inline-cap tail-cap))
                              (tail stderr (quot inline-cap 2) (quot tail-cap 2)) (assoc :stderr-tail (tail stderr (quot inline-cap 2) (quot tail-cap 2))))
                    :object object :seq seq}})))
        (catch clojure.lang.ExceptionInfo e
          (if-let [err (:err (ex-data e))] {:err err} (act/err :invalid (ex-message e))))))))

(defn index!
  "The index action: harness/index and harness/head on p. Returns {:ok {:result {...} :seq}}."
  [world p adapter-state via]
  (let [root (field adapter-state :adapter/locator)
        [text head] (binding [harness/*data-dir* (:data-dir world)]
                      [(harness/index-text root) (harness/head root)])
        r1 (act/set-field! world p {:object p :field :harness/index :type :string :value text :reader :both :via via})
        r2 (act/set-field! world p {:object p :field :harness/head :type :string :value head :reader :both :via via})]
    (cond (:err r1) r1
          (:err r2) r2
          :else {:ok {:result {:bytes (count (.getBytes ^String text "UTF-8")) :head head} :object p :seq (get-in r2 [:ok :seq])}})))

(defn perform!
  "Dispatch an action on an Object. {:object :action :args :via}."
  [world p {:keys [object action args via]}]
  (let [db (act/db world)
        s (objects/state db object)
        binds (field s :adapter/binds)]
    (cond
      (nil? s) act/unresolved
      (:object/retired s) (act/err :conflict (str "that " (name (:object/class s)) " is retired"))
      (and (= :cancel action) (#{:evaluation :principal} (:object/class s)))
      (let [r (evaluate/cancel! world p {:object object :via via})]
        ;; a cancel beats a steer: between two Model calls there is nothing to abort, so the wake
        ;; ends at its next turn instead (decisions 2026-10-31)
        (or (when (= :conflict (get-in r [:err :code])) (steer/cancel-wake! world p object)) r))
      (and (= :run action) (= :host binds))
      (if-let [{:keys [grant]} (resolve/resolve! world db p (act/id-name object) :evaluate)] (run! world p s args via grant) act/unresolved)
      (and (= :index action) (= :filesystem binds))
      (if (resolve/resolve! world db p (act/id-name object) :read) (index! world p s via) act/unresolved)
      ;; a named action sent at an Object: say the shape, since "not built" reads as a missing feature
      (objects/lookup db (objects/world-id db) (str "action/" (name action)))
      (act/err :invalid-args (str (name action) " is a named action, with no object: "
                                  "{\"op\":\"act\",\"args\":{\"action\":\"" (name action) "\",\"args\":{...}}}"))
      :else (act/err :not-implemented (str "action " (name action) " on a " (name (or (:object/class s) :object))
                                           (when binds (str " bound to " (name binds))) " is not built yet")))))
