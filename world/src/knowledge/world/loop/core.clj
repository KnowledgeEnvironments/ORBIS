(ns knowledge.world.loop.core
  "The resident loop, v1 (spec 9.8, decisions 2026-09-30 'agent loops are resident'). Kernel code,
   one cycle per wake, no state outside the Principal's Fields (loop/current, loop/iteration,
   loop/stalled). The world runs it for every Principal driven by :process whose principal/routes
   lists the Route a Message was delivered on.

   Wake (9.8 step 1): a `delivered` event, seen through an in-process log subscription (14.4), is
   resolved on a dispatcher thread and queued to the Principal's own serial lane (:
   different Principals evaluate at once, up to loop/concurrency on the world Principal, one
   Principal strictly in order); the log writer thread never appends.
   Assemble (step 2): the working set is the Message, its sender, the Route's input tags and the
   Principal itself (its instruction Fields); the Route's template shapes it (9.5).
   Serialize and evaluate (steps 3, 4): for the Principal's first Model, policy :normal.
   Parse and apply (steps 5, 6): ops the model emitted (knowledge.world.loop.ops) run as the
   Principal through the same dispatch a session uses; each iteration is its own Evaluation whose
   Fields hold the ops and their results, and the next iteration renders them from the log. An
   outcome with no ops is the reply: a Message on the same Route to the sender, in-reply-to the
   waking Message, via the last Evaluation. Caps: the Principal's Fields loop/max-iterations (8)
   and loop/max-ops (16), read at each wake.
   Loops talk to loops (decisions 2026-10-30): a Message a loop sent (event/via set) wakes the
   recipient's loop when its sender is one the recipient accepts, the Field loop/accepts-from, or
   by default its creator and the Principals it created, and when the Message's hop count
   (message/hop, incremented on every loop-sent leg) is below max-hops, so two loops cannot
   ping-pong past a bounded exchange: a person asks root (hop 0), root asks a Principal it made
   (hop 1), that Principal answers root (hop 2), and root's answer back (hop 3) wakes nobody.
   Reasoning is asked for (decisions 2026-10-30): the mode each Evaluation goes out with is resolved
   by reasoning-for: the Message's explicit request (message/reasoning, or a /think or /fast
   directive at the start of a line) wins, then the Principal's loop/reasoning, then the Model's
   model/reasoning; :auto at every level means on only for the turn after ops failed, off otherwise.
   Cancel (decisions 2026-10-30): `act cancel` on the Evaluation or on the Principal aborts the
   Model call; the wake ends with loop/stalled \"cancelled by <name> after N s\", no reply is sent,
   and the loop takes the next Message at once.
   Queue (decisions 2026-10-31): a wake that has to wait is on loop/queue, in order, and can be
   dequeued or reordered before it starts; waiting wakes survive a restart.
   Steer (decisions 2026-10-31): a Message of kind steer joins the running wake instead of waiting,
   see knowledge.world.loop.steer.
   Failure: loop/stalled holds the reason; the world does not crash."
  (:require [clojure.walk :as walk]
            [knowledge.world.code :as code]
            [knowledge.world.models :as models]
            [clojure.java.io :as io]
            [clojure.string :as str]
            [datascript.core :as d]
            [knowledge.world.act.core :as act]
            [knowledge.world.projects :as projects]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.refs :as refs]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fold.objects :as objects]
            [jsonista.core :as json]
            [knowledge.world.log.core :as log]
            [knowledge.world.delegate :as delegate]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.loop.worklog :as worklog]
            [knowledge.world.loop.queue :as queue]
            [knowledge.world.loop.resume :as resume]
            [knowledge.world.loop.steer :as steer]
            [knowledge.world.route.core :as route]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.changes :as changes]
            [knowledge.world.conversation :as conversation]
            [knowledge.world.thread :as thread]
            [taoensso.timbre :as timbre])
  (:import [java.util.concurrent Executors ExecutorService Semaphore ThreadFactory TimeUnit]))

(def evaluation-grace-ms 15000)
(def max-hops 3)
(def default-concurrency 8)   ; loop/concurrency on the world Principal overrides
(def known-objects-limit 40)  ; the most recent Objects a Principal created or is bound to, named in one line each
(def ^:private uuid-re #"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")

(defn- evaluation-timeout-ms
  "The Model's model/timeout-s (decisions 2026-10-30; the adapter's default when unset) plus a grace."
  [db model]
  (+ evaluation-grace-ms (* 1000 (long (or (get-in (objects/state db model) [:object/fields :model/timeout-s :field/value]) 600)))))

(defn- field [state k] (get-in state [:object/fields k :field/value]))

(defn- set-loop-field! [world p k type v]
  (act/set-field! world p {:object p :field k :type type :value v :reader :both}))

(defn first-model
  "The Principal's first Model of kind :model (spec 4.2 models), or nil."
  [db principal-state]
  (some (fn [e] (when (and (= :model (field (objects/state db e) :model/kind)) (not (objects/retired? db e))) e))
        (sort-by str (field principal-state :principal/models))))

(defn inbox?
  "Is the Route one the Principal receives on, and is the Principal process-driven?"
  [principal-state route]
  (and (= :process (:principal/driven-by principal-state))
       (contains? (set (field principal-state :principal/routes)) route)))

(defn- record-evaluation!
  "The iteration's bookkeeping as Fields on its Evaluation (decisions 2026-10-30): evaluation/iteration,
   evaluation/ops (what the model asked) and evaluation/results (what each op answered). Appended
   under the delegation the Evaluation itself was created with, via the Evaluation, like its
   `evaluated` events; the log is the truth the next iteration is rendered from."
  [world p evaluation iteration parsed-ops results]
  (let [grant (field (objects/state (act/db world) evaluation) :evaluation/delegation)
        fs (fn [k type v] (act/append! world (log/pending :field-set evaluation p {:name k :type type :value v :reader :both}
                                                          :delegation grant :via evaluation)))]
    (fs :evaluation/iteration :number iteration)
    (fs :evaluation/ops [:list :map] (mapv (fn [{:keys [op args raw err]}]
                                             (cond-> {:op (if op (name op) (str raw))}
                                               args (assoc :args (lops/fieldable args))
                                               err (assoc :err (lops/fieldable err))))
                                           parsed-ops))
    (fs :evaluation/results [:list :map] (lops/recordable results))))

(defn- record-iteration!
  "The final iteration of a wake asks for nothing; it still says which iteration it was."
  [world p evaluation iteration]
  (let [grant (field (objects/state (act/db world) evaluation) :evaluation/delegation)]
    (act/append! world (log/pending :field-set evaluation p {:name :evaluation/iteration :type :number :value iteration :reader :both}
                                    :delegation grant :via evaluation))))

(defn message-reasoning
  "The Message's explicit request: its message/reasoning Field, else a line starting with /think
   (on) or /fast (off) in its text; nil when it asks nothing."
  [m-state]
  (or (#{:on :off} (some-> (field m-state :message/reasoning) name keyword))
      (let [text (str (field m-state :message/text))]
        (cond (re-find #"(?m)^[ \t]*/think\b" text) :on
              (re-find #"(?m)^[ \t]*/fast\b" text) :off))))

(defn reasoning-for
  "The resolved mode for one iteration: Message, then Principal (loop/reasoning), then Model
   (model/reasoning), each counting only when definite (:on or :off); :auto all the way down means
   :off. A failed op no longer turns reasoning on by itself (decisions 2026-10-30: on the real model
   that cost an 8-minute, 29k-char turn after one bad op); people ask with /think, Principals and
   Models with their Fields. prev-failed? is kept in the signature for the serializer's note."
  [db p-state m-state model prev-failed?]
  (let [definite (fn [v] (#{:on :off} (some-> v name keyword)))]
    (or (message-reasoning m-state)
        (definite (field p-state :loop/reasoning))
        (definite (field (objects/state db model) :model/reasoning))
        (do prev-failed? :off))))
(defn- name-of
  "A bound name of an Object for people to read, else its short id."
  [db id]
  (or (first (sort (map second (objects/names db id)))) (subs (str id) 0 8)))

(defn recent-messages
  "The newest Messages on the Route before `message`, newest first, up to n: the
   conversation this wake continues, rendered by the serializer as Messages, never as Evaluations."
  [db route message n]
  (if (or (nil? route) (not (pos? (long n))))
    []
    (->> (d/q '[:find ?id ?s :in $ ?route :where
                [?f :field/name :message/route] [?f :field/value ?route]
                [?o :object/fields ?f] [?o :object/id ?id] [?o :object/created-seq ?s]]
              db route)
         (remove (fn [[id _]] (= id message)))
         (sort-by second >)
         (take n)
         (mapv first))))

(defn known-objects
  "Objects p created (Principals, Adapters, Models, Routes, Groups, plain Objects) or is bound to,
   newest first, up to known-objects-limit: named to the model in one line each."
  [db p p-state]
  (let [created (d/q '[:find [?id ...] :in $ ?p [?c ...] :where
                       [?o :object/created-by ?p] [?o :object/class ?c] [?o :object/id ?id]]
                     db p [:principal :adapter :model :route :group :object])
        bound (mapcat #(let [v (field p-state %)] (if (coll? v) v (when v [v])))
                      [:principal/adapters :principal/models :principal/routes :principal/workspaces])]
    (->> (concat bound created)
         (filter uuid?)
         distinct
         ;; an action is named in the Actions block; forty lines of them bury what p really has
         (remove #(field (objects/state db %) :action/name))
         (sort-by (fn [o] (- (long (or (:object/created-seq (objects/entity db o)) 0)))))
         (take known-objects-limit)
         vec)))

(def mentioned-limit 4) ; ids a Message spells out that enter whole; the rest stay known-by-name

(defn- mentioned
  "Objects whose id the Message text spells out, the first mentioned-limit of them: they enter the
   working set whole; a Message that lists dozens of ids (an ops script) names them, no more."
  [db text]
  (->> (re-seq uuid-re (str text)) distinct (map parse-uuid) (filter #(objects/exists? db %)) (take mentioned-limit) vec))

(defn- created-by-ops
  "Objects an act created in this iteration's results: they enter the next iteration whole, so the
   model sees what it just made; what it read is in the results already and is not repeated."
  [results]
  (->> results
       (filter (fn [{:keys [op ok result]}] (and ok (= "act" (str op)) (uuid? (:object result)))))
       (map #(get-in % [:result :object]))
       vec))

(defn- stall! [world p reason]
  (set-loop-field! world p :loop/stalled :string (str reason))
  {:stalled reason})

(defn trigger-message?
  "Is the Message a Trigger's firing (message/kind :trigger)? ."
  [m-state]
  (= :trigger (some-> (field m-state :message/kind) name keyword)))

(def ^:private quiet-re #"(?is)^\W*(nothing( to (do|report|add))?|no( action| change| update)?( (needed|required|to report))?|all (quiet|clear|good)|ok|done|idle)\W*$")
(def ^:private quiet-close-re #"(?is)\b(nothing to (do|act on|report|add)|no action (is )?(needed|required)|all quiet)\W*$")

(defn quiet-reply?
  "A heartbeat answered with nothing to say: blank, a short line that only says so, or a status
   under 400 chars that closes on it (\"... Nothing to act on.\"), the way a model reports a clean
   check. Such a reply is dropped, not sent, so heartbeats never spam a Route (decisions 2026-10-30
   'triggers'); the Evaluation keeps the text."
  [text]
  (let [t (str/trim (str text))]
    (or (str/blank? t)
        (and (< (count t) 80) (boolean (re-find quiet-re t)))
        (and (< (count t) 400) (boolean (re-find quiet-close-re t))))))

(def ^:private ack-re #"(?is)^\W*(thanks?( you)?|thank you|acknowledged|ack|noted|got it|received|understood|roger|will do|great|perfect|ok(ay)?|done|cheers|confirmed|sounds good)\b")

(defn courtesy-reply?
  "An acknowledgement between loops: a quiet reply, or at most 200 chars that open
   with thanks/noted/acknowledged and ask nothing. At hop 3 or more such a reply is dropped, not
   sent, the way a quiet heartbeat is: the text stays on the Evaluation."
  [text]
  (let [t (str/trim (str text))]
    (or (quiet-reply? t)
        (and (<= (count t) 200) (not (str/includes? t "?")) (boolean (re-find ack-re t))))))

(defn- result-message?
  "A Message that reports an outcome rather than asks: an approval decided (kind approval-result) or
   a Task's completion (kind task-result)."
  [m-state]
  (contains? #{:approval-result :task-result} (some-> (field m-state :message/kind) name keyword)))

(defn- note-malformed!
  "evaluation/context gains malformed-fences when the reply carried an unclosed ```ops fence
   (the fence was stripped and the text kept)."
  [world p evaluation n]
  (when (pos? (long n))
    (let [s (objects/state (act/db world) evaluation)
          grant (get-in s [:object/fields :evaluation/delegation :field/value])
          ctx (or (get-in s [:object/fields :evaluation/context :field/value]) {})]
      (act/append! world (log/pending :field-set evaluation p {:name :evaluation/context :type :map
                                                              :value (assoc ctx :malformed-fences (long n)) :reader :both}
                                      :delegation grant :via evaluation)))))


;; ---------------------------------------------------------------- runs this wake

(defn- last-lines [^String s n]
  (->> (str/split-lines (str s)) (map str/trimr) (remove str/blank?) (take-last n) vec))

(defn- run-of-result
  "A wake result that ran a command: {:cmd :exit :duration-ms :tail} or nil."
  [{:keys [op args ok result]}]
  ;; execute! records a handler's {:ok {:result ... :object ...}} as {:result {:result ... :object ...}}
  (let [r (cond (and (map? result) (contains? result :exit)) result
                (and (map? result) (map? (:result result)) (contains? (:result result) :exit)) (:result result))]
    (when (and ok (= "act" (str op)) r (:task r))
      (let [cmd (or (get-in args [:args :cmd]) (get-in args [:cmd]) (get-in args [:args "cmd"]))]
        {:cmd (str (or cmd "?")) :exit (:exit r) :duration-ms (:duration-ms r)
         :tail (last-lines (or (:stdout-tail r) (:stdout r)) 3)}))))

(defn- run-of-approval
  "The run an approval Task executed, read from the Task Fields (task/ran, set by gates/approve!)."
  [db m-state]
  (when-let [t (field m-state :message/task)]
    (when-let [ran (get-in (objects/fields db t) [:task/ran :field/value])]
      (let [rf (objects/fields db ran)
            v (fn [k] (get-in rf [k :field/value]))
            out (v :task/stdout)]
        {:cmd (str (v :task/command)) :exit (v :task/exit) :duration-ms (v :task/duration-ms)
         :tail (if (string? out) (last-lines out 3) ["(stdout is a blob on the Task)"])}))))

(defn runs-block
  "The kernel's record of the commands a wake ran, appended to the reply so the numbers a person
   reads come from task/exit and stdout, not from the model's prose (report 2026-10-30, finding 2)."
  [runs]
  (when (seq runs)
    (str "\n\nRuns this wake (kernel record):\n"
         (str/join "\n" (for [{:keys [cmd exit duration-ms tail]} runs]
                           (str "- `" cmd "` → exit " exit
                                (when duration-ms (str " (" (quot (long duration-ms) 1000) " s)"))
                                (when (seq tail) (str "\n    " (str/join "\n    " tail)))))))))

(def ^:private claimed-exit-re #"(?i)\bexit(?:ed|\s+code|\s+status)?\s*[:=]?\s*(\d+)")
(def ^:private claims-success-re #"(?i)\b(0 failures|all tests? pass(?:ed|ing)?|tests? passed|build succeeded|succeeded|exit(?: code)? 0)\b")

(defn contradiction
  "What the reply claims about its runs against what the kernel recorded, or nil: a claimed exit
   code no run had, or a claim of success when a run exited non-zero (listener-dev reported
   \"14 run, 0 failed\" over an exit 2)."
  [text runs]
  (when (seq runs)
    (let [exits (set (map #(long (or (:exit %) 0)) runs))
          claimed (set (map #(Long/parseLong (second %)) (re-seq claimed-exit-re (str text))))
          bad-codes (remove exits claimed)
          success? (boolean (re-find claims-success-re (str text)))
          failed (filter #(not= 0 (long (or (:exit %) 0))) runs)]
      (cond
        (seq bad-codes) (str "the reply claims exit " (str/join ", " (sort bad-codes)) " but the runs exited " (str/join ", " (sort exits)))
        (and success? (seq failed)) (str "the reply claims success but `" (:cmd (first failed)) "` exited " (:exit (first failed)))))))

(def ^:private count-claim-re #"(\d+)\s+(?:tests?|assertions?|run)\b")

(defn- principals-named
  "Other Principals whose bound name the text mentions as a word: [[name id] ...]."
  [db p text]
  (vec (for [[id] (d/q '[:find ?id :where [?o :object/class :principal] [?o :object/id ?id]] db)
             :when (not= id p)
             :let [n (name-of db id)]
             :when (and (string? n) (not (re-matches #"[0-9a-f]{8}" n))
                        (re-find (re-pattern (str "(?<![\\w.-])" (java.util.regex.Pattern/quote n) "(?![\\w-])")) (str text)))]
         [n id])))

(defn- latest-task
  "The newest Task a Principal created, or nil."
  [db q]
  (some->> (d/q '[:find ?id ?s :in $ ?q :where [?o :object/class :task] [?o :object/created-by ?q] [?o :object/id ?id] [?o :object/created-seq ?s]] db q)
           seq (sort-by second) last first))

(defn other-run-contradiction
  "What the reply claims about ANOTHER Principal's runs against that Principal's latest Task on the
   log (the report's run 2: root said listener-dev \"earlier reported its counts\" with
   no Task behind it): a claimed exit, a success, or a test count near the Principal's name that its
   newest Task does not bear, or no Task at all. Nil when the reply makes no such claim."
  [db p text]
  (first
    (for [[n q] (principals-named db p text)
          :let [t (str text)
                i (str/index-of t n)
                window (subs t i (min (count t) (+ i (count n) 220)))
                exits (set (map #(Long/parseLong (second %)) (re-seq claimed-exit-re window)))
                counts (set (map second (re-seq count-claim-re window)))
                success? (boolean (re-find claims-success-re window))]
          :when (or (seq exits) (seq counts) success?)
          :let [task (latest-task db q)
                tf (when task (objects/fields db task))
                exit (when tf (get-in tf [:task/exit :field/value]))
                cmd (when tf (get-in tf [:task/command :field/value]))
                out (when tf (get-in tf [:task/stdout :field/value]))
                problem (cond
                          (nil? task) (str "the reply speaks of a run or a result of " n ", but " n " has run nothing on the log")
                          (and (seq exits) (not (contains? exits (long (or exit -1)))))
                          (str "the reply claims " n " exited " (str/join ", " (sort exits)) " but its latest run `" cmd "` exited " exit)
                          (and success? (not= 0 (long (or exit 0))))
                          (str "the reply claims success for " n " but its latest run `" cmd "` exited " exit)
                          (and (seq counts) (string? out) (not (some #(str/includes? out %) counts)))
                          (str "the reply quotes " (str/join ", " (sort counts)) " for " n " but its latest run's output holds no such count"))]
          :when problem]
      problem)))

(defn- own-names [db p] (vec (keep (fn [[_ n]] n) (:object/names (objects/state db p)))))

(defn lacking-of
  "The ops of this wake that failed unresolved on a name under the Principal's own prefix (world
   pass 14): what it lacks and only its creator can grant or mount. {:op :target :args :err} each."
  [db p results]
  (let [names (own-names db p)
        under? (fn [t] (some (fn [n] (or (= t n) (str/starts-with? t (str n "/")) (str/starts-with? t (str n ".")))) names))]
    (vec (for [{:keys [op args err]} results
               :when (= :unresolved (some-> (:code err) name keyword))
               :let [t (or (:name args) (:object args) (get-in args [:args :adapter]) (get-in args [:args :principal]) (:adapter args))]
               :when (and (string? t) (under? t))]
           {:op (str op) :target t :args (lops/fieldable args) :err (str (:message err))}))))

(defn- drop-wake!
  "A retired Principal's queued wake does not run (decisions 2026-10-31). The note is on the
   Principal, the world as actor, as the running books are: loop/dropped counts them."
  [world p {:keys [message]}]
  (let [n (long (or (get-in (objects/fields (act/db world) p) [:loop/dropped :field/value]) 0))]
    (log/append!! (:log world) (log/pending :field-set p (:world-id world)
                                            {:name :loop/dropped :type :number :value (inc n) :reader :both}))
    (timbre/info "wake dropped: the Principal is retired" {:principal p :message message})
    {:stalled "retired"}))

(defn- prepare-wake
  "Everything one wake needs, read once at its start. The working set is the Message, its sender,
   p, the Message's tags, the recent conversation on the Route and whatever the Message names by id;
   what else p owns is named in one line each (known). When someone else started the thread, its
   root enters whole, so an answer arriving at hop 2 is read next to the question it serves."
  [world p {:keys [message route wake]}]
  (let [db (act/db world)
        p-state (objects/state db p)
        m-state (objects/state db message)
        sender (field m-state :message/from)
        pol (ser/policy db p)
        thread-id (thread/thread-of db message)
        asked-by (thread/asked-by db message)
        choice (models/for-wake db p-state thread-id)
        base (-> #{message} (conj sender p) (into (field m-state :message/tags))
                 (into (recent-messages db route message (dec (long (:serialize/messages-verbatim pol)))))
                 (into (mentioned db (field m-state :message/text)))
                 (into (take mentioned-limit (keep :object (field m-state :message/refs))))
                 (cond-> (and (not= thread-id message) (not= asked-by p)) (conj thread-id)))]
    {:db db :p-state p-state :m-state m-state :sender sender :message message :route route :wake wake
     :choice choice
     :model (:model choice)
     :hop (inc (long (or (field m-state :message/hop) 0)))
     ;; eight turns was sized to 8k of context; with a Model's real window the wake has room to work
     :max-iterations (long (or (field p-state :loop/max-iterations)
                               (if (models/window-of db p thread-id) models/wide-iterations lops/default-max-iterations)))
     ;; a limit a person set on the Principal is a hard one; otherwise progress carries the wake further
     :max-turns (long (or (field p-state :loop/max-turns)
                          (field p-state :loop/max-iterations)
                          (if (models/window-of db p thread-id) models/wide-turns lops/default-max-turns)))
     :max-ops (long (or (field p-state :loop/max-ops) lops/default-max-ops))
     :thread-id thread-id :asked-by asked-by :base base
     :known (vec (remove base (known-objects db p p-state)))}))

(defn- forward-wake!
  "A Principal with no Model: a bridge forwards the Message to the other world and relays the
   answer (decisions 2026-10-30, other worlds as Adapters); anything else stalls."
  [world p {:keys [db p-state message route]}]
  (if (some (fn [a] (= :world (field (objects/state db a) :adapter/binds))) (field p-state :principal/adapters))
    (let [r ((requiring-resolve 'knowledge.world.act.handlers/forward!) world p {:message message :route route})]
      (if (:err r)
        (stall! world p (str "bridge: " (get-in r [:err :code]) " " (get-in r [:err :message])))
        (do (set-loop-field! world p :loop/stalled :string "")
            {:forwarded (:remote-message r) :reply (:reply r)})))
    (stall! world p "no Model in principal/models")))

(defn- stopping?
  "Is the world stopping? A scratch session's child world has no lanes to stop."
  [world]
  (when-let [running (some-> (:loop-hooks world) deref :running)] (not @running)))

(defn- turn-limit
  "The turns this wake has now. Eight cannot read, change, build and test: in 12 real tasks every
   wake that ran out had been finding things to the end. So a wake keeps going while it makes
   progress, a few turns past the last that did, up to loop/max-turns; a wake going round stops."
  [{:keys [max-iterations max-turns]} {:keys [progress-at]}]
  (min (long max-turns) (max (long max-iterations) (+ (long (or progress-at 0)) lops/progress-grace))))

(defn- stop-reason
  "Why the wake must not start another iteration, or nil. A stopping world ends the wake as
   interrupted, so that it resumes at the next boot rather than stalling."
  [world p {:keys [db] :as ctx} {:keys [i] :as st}]
  (when (stopping? world) (throw (InterruptedException.)))
  (let [limit (turn-limit ctx st)]
    (cond
      (queue/wake-cancelled world p) (str "cancelled by " (name-of db (queue/wake-cancelled world p)))
      ;; its turns to act, one with no tools to say what they found, one more if that was empty
      (> i (+ 2 limit)) (str "iteration cap " limit " reached without a final reply (loop/max-iterations, loop/max-turns)"))))

(defn- named-dirs
  "The directories the waking Message and its thread's first Message name, by absolute path."
  [db {:keys [m-state thread-id]}]
  (let [asked (str (field m-state :message/text) "\n"
                   (some-> thread-id (->> (objects/state db)) (field :message/text)))]
    (->> (re-seq #"/[\w.@-]+(?:/[\w.@-]+)+" asked) (map #(str/replace % #"\.+$" "")) distinct
         (filter #(.isDirectory (io/file ^String %))))))

(defn- under? [root ^String w] (or (= w root) (str/starts-with? w (str root "/"))))

(defn- named-directory-note
  "A line for this wake when the question names a directory p has no mount at or under, and p can add
   one. Told in a standing line where its source was, root looked for the projects under that."
  [world p ctx]
  (let [db (act/db world)]
    (when (resolve/resolves? db p "action/project.add" :evaluate)
      (let [mounts (map :path (projects/projects-of db p))
            bare (remove (fn [d] (some #(or (under? d %) (under? % d)) mounts)) (named-dirs db ctx))]
        (when (seq bare)
          (str "Outside your mounts, so no read, outline or shell reaches it: " (str/join ", " (map #(str "`" % "`") bare))
               ". Call add_project {name, path} once for each project there, with its full path."))))))

(defn- work-note
  "A line once a wake has read for a long while and done nothing. With turns to spare, a model read
   for all of them: 31 turns on a one-file test, and no test."
  [{:keys [i done]}]
  (when (and (>= i 8) (not-any? #(or (= "file.edit" (some-> (get-in % [:args :action]) name)) (run-of-result %)) done))
    (str "Turn " i ": you have looked at " (count done) " things and changed nothing. "
         "You know enough. If the ask is a change, make it now with edit. Otherwise answer now.")))

(defn- await-outcome!
  "Wait for the Evaluation. The adapter's own deadline should fire first; if it does not, the
   Evaluation ends on the log here, so nothing stays `running` for ever."
  [world p {:keys [db model]} ok]
  (let [evaluation (:evaluation ok)
        _ (queue/running! world p evaluation)
        _ (steer/settle! world p evaluation)
        ms (evaluation-timeout-ms db model)
        waited (deref (:done ok) ms ::timeout)]
    (queue/running! world p nil)
    (if (= ::timeout waited)
      (or (evaluate/fail! world evaluation (str "timeout after " (quot ms 1000) " s waiting for the Model"))
          {:state :failed :error "timeout waiting for the Model"})
      waited)))

(defn- asked-goal
  "The task in the asker's words: a card's result is not the task, the thread's first Message is."
  [{:keys [db m-state thread-id]}]
  (let [m (if (and thread-id (result-message? m-state)) (objects/state db thread-id) m-state)
        t (str/trim (str (field m :message/text)))]
    (subs t 0 (min 400 (count t)))))

(defn- lean?
  "Past a work wake's first look, the kernel's note carries the wake: the prompt need not."
  [{:keys [work?]} {:keys [log]}]
  (boolean (and work? (pos? (long (or (:n log) 0))))))

(defn- call-model!
  "One iteration's model call, with any steering that has arrived. A steered wake answers to its
   newest steer, not to the Message that opened it: a /think on the question does not carry over
   (measured: 298 s against 74 s to the steered answer); the steer's own /think or /fast, then the
   Principal and Model, decide. Returns {:evaluation :outcome} or {:err}."
  [world p {:keys [db p-state m-state choice base known message route wake changes] :as ctx} compressed?
   {:keys [i evaluations made prev-failed? reasoning-spent thinking-so-far retry-of ids fallback] :as st}]
  (let [{:keys [model model-id]} (or fallback choice)
        steered (queue/take-steers! world p)
        steer-state (some->> (peek (:all steered)) :message (objects/state (act/db world)))
        reasoning (if reasoning-spent :off (reasoning-for db p-state (or steer-state m-state) model prev-failed?))
        working-set (-> base (into evaluations) (into made))
        {:keys [ok err]} (evaluate/evaluate! world {:principal p :working-set working-set :model model
                                                    :model-id model-id
                                                    :route route :policy :normal :message message :wake wake
                                                    :compressed (and (= 1 i) compressed?)
                                                    :changes (when (= 1 i) changes)
                                                    :reasoning reasoning :known (vec (remove working-set known))
                                                    :steers (:all steered) :ids ids
                                                    :thinking-so-far thinking-so-far :retry-of retry-of
                                                    :last-turn? (> i (turn-limit ctx st))
                                                    :hold-tools (when (and (:work? ctx) (:log st)) (worklog/held-tools (:log st)))
                                                    :lean? (lean? ctx st)
                                                    :wake-note (if (:work? ctx)
                                                                 (or (worklog/render (:log st) i (turn-limit ctx st) (asked-goal ctx))
                                                                     (named-directory-note world p ctx))
                                                                 ;; no running count on a real window: it changes the prompt every turn
                                                                 (or (when-not (models/window-of db p (:thread-id ctx)) (work-note st))
                                                                     (named-directory-note world p ctx)))})]
    (if err
      {:err err}
      (do (steer/applied! world (:evaluation ok) (:new steered))
          {:evaluation (:evaluation ok) :ids (:ids ok) :outcome (await-outcome! world p ctx ok)}))))

(defn- usage-note [{:keys [usage]}]
  (when usage (str " (tokens in " (:prompt-tokens usage) ", out " (:completion-tokens usage) ")")))

(defn- steered? [outcome] (and (= :cancelled (:state outcome)) (= :steer (:reason outcome))))

(defn- reasoning-cut?
  "The call hit model/reasoning-max-chars for the first time this wake. A long /think is a
   legitimate request: the reasoning already paid for goes back in with reasoning off, once."
  [outcome {:keys [reasoning-spent]}]
  (and (= :reasoning-cap (:reason outcome)) (not reasoning-spent)))

(defn- note-choice!
  "loop/model-note says which Model is standing in, and goes when none is, so a person is never left
   reading a stale reason."
  [world p {:keys [db p-state choice]}]
  (let [was (field p-state :loop/model-note)
        now (if-let [from (:fell-back-from choice)]
              (str (models/label-of db (:model choice)) " is answering: " (models/label-of db from) " is "
                   (name (or (field (objects/state db from) :model/status) :unknown)))
              "")]
    (when (not= (str was) now) (set-loop-field! world p :loop/model-note :string now))))

(defn- fallback-now
  "A failed call whose Model is down, once a wake, moves to the fallback: the wake goes on, and the
   person reads which Model answered and why. The failed Model is marked either way, so the next wake
   passes it over too. Nil when there is nothing to move to."
  [world p {:keys [p-state choice]} {:keys [fallback]} outcome]
  (when (and (nil? fallback) (= :failed (:state outcome)))
    (let [model (:model choice)
          status (models/note-failure! world model (:error outcome))
          f (when status (models/fallback-for (act/db world) p-state model))]
      (when f
        (set-loop-field! world p :loop/model-note :string
                         (str (models/label-of (act/db world) (:model f)) " is answering: "
                              (models/label-of (act/db world) model) " is " (name status)))
        f))))

(defn- end-undone!
  "The model call ended without an answer: a cancel (decisions 2026-10-30) ends the wake with no
   reply and frees the loop; a failure stalls with its reason."
  [world p {:keys [db]} outcome]
  (let [seconds (quot (long (or (:elapsed-ms outcome) 0)) 1000)]
    (if (= :cancelled (:state outcome))
      (do (queue/cancel-wake! world p (or (:by outcome) p))
          (stall! world p (str "cancelled by " (name-of db (or (:by outcome) p)) " after " seconds " s")))
      (stall! world p (str "evaluation failed after " seconds " s: " (:error outcome) (usage-note outcome))))))

(defn- reply-address
  "Where the reply goes. A wake that is a result (an approval decided, a run done) in a thread
   someone else opened with a question p has not answered goes to that asker, in reply to their
   question, not back to the result's sender."
  [world p {:keys [m-state message route sender thread-id asked-by]}]
  (let [db (act/db world)
        question (when (and (result-message? m-state) thread-id (not= thread-id message) asked-by (not= asked-by p))
                   (thread/open-question db thread-id p))
        ;; a subagent's result answers whoever asked for the work: the subagent is about to go
        task (when (and (delegate/child-of? db p sender)
                        (not= :request (some-> (field m-state :message/kind) name keyword)))
               (delegate/task-of db sender))
        task-state (when task (objects/state db task))
        asker (when task (field task-state :task/requested-by))
        asked (when task (field task-state :task/message))]
    (cond
      question {:to #{asked-by} :route (or (field (objects/state db question) :message/route) route) :in-reply-to question}
      (and asker asked) {:to #{asker} :route (or (field (objects/state db asked) :message/route) route) :in-reply-to asked}
      ;; nobody asked for it: the subagent is going, so p notes it to itself
      task {:to #{p} :route (or (first (remove #{route} (field (objects/state db p) :principal/routes))) route) :in-reply-to message}
      :else {:to #{sender} :route route :in-reply-to message})))

(defn- note-reply!
  "Facts about a sent reply, under the Evaluation's delegation as evaluation/results are: a narrow
   Principal holds no :write on its own Messages, and the record must land anyway."
  [world p evaluation message contra lacking]
  (let [grant (field (objects/state (act/db world) evaluation) :evaluation/delegation)
        note! (fn [name type value]
                (act/append! world (log/pending :field-set message p {:name name :type type :value value :reader :both}
                                                :delegation grant :via evaluation)))]
    (when contra (note! :reply/contradiction :string contra))
    (when lacking (note! :message/lacking [:list :map] lacking))))

(defn- send-reply!
  "The final text goes out with the kernel's record of this wake's runs on it, checked against what
   the text claims, including about other Principals' runs. While an op of this wake waits for
   approval it is a status, not the answer, so the thread keeps the question open. To the creator,
   after ops p could not perform under its own name, it is a provisioning request."
  [world p {:keys [db m-state hop message] :as ctx} {:keys [i done interim? failed ids answer-refs]} evaluation reply]
  (if (delegate/waiting? (act/db world) p message)
    (do (record-iteration! world p evaluation i)
        (set-loop-field! world p :loop/stalled :string "")
        {:evaluation evaluation :quiet true :iterations i})
  (let [{reply :text refs :refs unresolved :unresolved} (refs/parse (act/db world) p ids reply)
        refs (or (seq refs) (seq answer-refs))
        ;; what is plainly a file where p works links; a reference already there keeps its place
        cited (remove (fn [{:keys [start end]}] (some #(and (< start (:end %)) (< (:start %) end)) refs))
                      (code/cited-paths (act/db world) p reply))
        refs (not-empty (vec (sort-by :start (concat refs cited))))
        runs (vec (remove nil? (conj (mapv run-of-result done) (run-of-approval db m-state))))
        contra (or (contradiction reply runs) (other-run-contradiction (act/db world) p reply))
        address (reply-address world p ctx)
        db' (act/db world)
        creator (:object/created-by (objects/entity db' p))
        lacking (lacking-of db' p failed)
        request? (boolean (and (seq lacking) creator (contains? (:to address) creator)))
        kind (cond request? :request interim? :status)
        sent (route/send! world p (cond-> (assoc address :text (str reply (runs-block runs)) :via evaluation :hop hop)
                                    kind (assoc :kind kind)
                                    (seq refs) (assoc :refs refs)
                                    (seq unresolved) (assoc :unresolved-refs unresolved)))]
    (if (:err sent)
      (stall! world p (str "send: " (get-in sent [:err :code])))
      (let [message (get-in sent [:ok :message])]
        (when (or contra request?) (note-reply! world p evaluation message contra (when request? lacking)))
        (set-loop-field! world p :loop/stalled :string "")
        (when-not (or interim? request?) (delegate/result! world p reply (:message ctx)))
        {:evaluation evaluation :reply message :iterations i :runs runs :contradiction contra :lacking lacking})))))

(defn- end-quietly! [world p evaluation i]
  (record-iteration! world p evaluation i)
  (set-loop-field! world p :loop/stalled :string "")
  {:evaluation evaluation :quiet true :iterations i})

(defn- finish-turn!
  "An outcome with no ops is the end of the wake. Nothing to say to a Trigger, or an acknowledgement
   at hop 3 or more between loops, is dropped rather than sent; an empty text stalls."
  [world p {:keys [m-state hop] :as ctx} {:keys [i] :as st} evaluation outcome reply]
  (cond
    (and (trigger-message? m-state) (quiet-reply? reply)) (end-quietly! world p evaluation i)
    (and (>= hop 3) (courtesy-reply? reply)) (end-quietly! world p evaluation i)
    (str/blank? reply)
    (stall! world p (str "empty reply"
                         (when (= "length" (:finish outcome)) ": completion cut at max_tokens (finish_reason length)")
                         (usage-note outcome)
                         (when (:thinking outcome) "; the reasoning is on the Evaluation as evaluation/thinking")))
    :else (do (record-iteration! world p evaluation i)
              (send-reply! world p ctx st evaluation reply))))

(defn- pending? [r] (= :approval-pending (some-> (get-in r [:err :code]) name keyword)))

(defn- looked-at
  "What this wake did, in a line a person can use: the model gave nothing, the record still says
   where it got to."
  [done]
  (let [seen (->> done
                  (keep (fn [{:keys [op args]}]
                          (let [inner (:args args)]
                            (or (when (= "read" (some-> op name)) (:name args))
                                (:cmd inner) (:pattern inner) (:name inner)))))
                  distinct (take-last 6) vec)]
    (when (seq seen) (str " It last looked at: " (str/join "; " seen) "."))))

(defn- found-line
  "One thing a wake learned, from the record of the op: what the model could not say, the kernel can."
  [r]
  (let [{:keys [op args result]} r
        inner (:args args)
        res (or (:result result) result)]
    (cond
      (run-of-result r) (let [{:keys [cmd exit tail]} (run-of-result r)]
                          (str "`" cmd "` exited " exit (when-let [l (last (flatten [tail]))] (str ": " l))))
      (seq (:results res)) (str "`" (or (:pattern res) (:pattern inner)) "` matched in " (str/join ", " (distinct (take 4 (map :path (:results res))))))
      (seq (:projects res)) (str/join "\n- " (map (fn [{:keys [name state about git]}]
                                                    (str name ", " state (when about (str ": " about))
                                                         (when git (str " Last commit " (:last-commit git) ", " (:uncommitted git) " uncommitted."))))
                                                  (:projects res)))
      (= "read" (some-> op name)) (when-let [n (:name args)] (str "read " n)))))

(defn- found-so-far
  "What this wake's ops established, newest last, or nil."
  [done]
  (let [lines (->> done (keep found-line) distinct (take-last 6) vec)]
    (when (seq lines) (str "\nWhat it found:\n" (str/join "\n" (map #(str "- " %) lines))))))

(declare docs-re)

(defn- tell-stopped!
  "A wake that hit its cap ends without an answer, so the asker is told: a person otherwise waits on
   a silence, with the reason only in a Field they are not looking at. The model had two turns to say
   what it found and said nothing, so the kernel reports the record, as the answer: three runs of six
   ended here with the work done and nothing said."
  [world p ctx {:keys [evaluations done log] :as st}]
  (send-reply! world p ctx st (peek evaluations)
               (str "Stopped after " (turn-limit ctx st) " turns without an answer, so this is the kernel's record."
                    (when (:work? ctx) (worklog/report log docs-re))
                    (or (found-so-far done) (looked-at done)))))

(defn- keep-held!
  "What a card held back goes on the card's Task, for the decision to carry."
  [world p evaluation failed]
  (let [held (filter #(= :held (some-> (get-in % [:err :code]) name keyword)) failed)
        task (some #(when (pending? %) (get-in % [:err :task])) (reverse failed))]
    (when (and (seq held) (some? task))
      (let [line (fn [{:keys [op args]}]
                   (let [t (str (or (:action args) op) " " (json/write-value-as-string (or (:args args) args)))]
                     (subs t 0 (min 400 (count t)))))
            grant (field (objects/state (act/db world) evaluation) :evaluation/delegation)]
        (act/append! world (log/pending :field-set (if (string? task) (parse-uuid task) task) p
                                        {:name :task/held :type [:list :string] :value (mapv line held) :reader :both}
                                        :delegation grant :via evaluation))))))

(defn- wait-for-approval!
  "A gated op of this wake waits for a person, so the wake ends here, with no further model call: the
   status is the kernel's, naming what waits, and the decision wakes p with the result. A model asked
   to go on at this point has nothing to report, and writes the answer it expected anyway (seen live:
   a load average of 1.42 before the command had run), or asks again."
  [world p {:keys [hop] :as ctx} {:keys [i failed] :as st} evaluation]
  (record-iteration! world p evaluation i)
  (keep-held! world p evaluation failed)
  (let [waiting (distinct (keep #(or (get-in % [:err :summary]) (get-in % [:err :message])) (filter pending? failed)))
        sent (route/send! world p (assoc (reply-address world p ctx)
                                         :text (str "Waiting for approval: " (str/join "; " waiting))
                                         :via evaluation :hop hop :kind :status))]
    (if (:err sent)
      (stall! world p (str "send: " (get-in sent [:err :code])))
      (do (set-loop-field! world p :loop/stalled :string "")
          {:evaluation evaluation :reply (get-in sent [:ok :message]) :iterations i :waiting (vec waiting)}))))

(defn- answers-of
  "The sends among ops that can only be this wake's answer: plain text to exactly the asker. The
   kernel delivers the final reply itself, so such a send is not a Message of its own: said with
   nothing else it is the reply, and the wake ends; said beside other ops it was written before their
   results, so it is held back and the model answers from what comes back (seen live: seven sends
   of one answer)."
  [world p ctx ops]
  (let [{:keys [to]} (reply-address world p ctx)]
    (filterv (fn [{:keys [op args err]}]
               (and (= :send op) (nil? err) (not (str/blank? (:text args))) (not (:steer args))
                    (= (set to) (set (:to args)))))
             ops)))

(def ^:private answer-too-soon
  {:code :answer-held
   :message "not sent: written before the results of the ops beside it. Answer from those results as plain text, with no ops."})

(defn- not-an-answer
  "Why this text cannot be what the asker reads, or nil: an op the parser could not make out, or a
   placeholder. Each of these reached a person as the answer in live runs: `*Preparing reply...*`,
   `... (truncated)`, `<tool_call>...`. It costs a turn, not a Message."
  [reply]
  (let [t (str/trim (str reply))
        words (-> t (str/replace #"(?i)\(truncated\)" "") (str/replace #"[\W_]+" " ") str/trim)]
    (cond
      (str/blank? t) nil                ; an empty reply has its own stall, with the Model's reason
      (or (str/includes? t "<tool_call>") (re-find #"^\{\s*\"op\"" t))
      "an op that could not be read. An op goes in a fenced ops block, one JSON object a line."
      ;; a tool's name as the whole reply reached a person as the answer: `act`
      (re-find #"(?i)^(act|read|grep|outline|run|test|edit|send|projects|add_project|project\.add)$" t)
      "a tool's name, not an answer. Call the tool, or answer."
      ;; an intention with nothing behind it: the turn ended, so the person waits on nothing
      (and (< (count t) 160) (not (str/ends-with? t "?")) (not (re-find #"(?i)\byour?\b" t))
           (re-find #"(?i)^(i\W*ll|i will|i am going to|let me|next i)\b" words))
      "a plan, with no op behind it. Run the op now, or answer."
      (or (str/blank? words)
          (and (< (count t) 80)
               (re-find #"(?i)^(preparing|working on|one moment|please wait|stand by|understood\s+i\W*ll wait|i\W*ll wait)" words)))
      "a placeholder, not an answer. Do what was asked, or say plainly why you cannot.")))

(def ^:private claims-work-re
  #"(?i)runs this wake|\bexit(?:ed)?(?:\s+code)?\s*[:=]?\s*\d+|\bthe edit is done\b|\bi(?:\s+have|'ve)?\s+(?:edited|changed|added|wrote|updated|created|committed|ran|built|fixed|installed|deleted|removed)\b")

(def ^:private ends-in-a-plan-re
  "A reply whose last sentence is what it means to do next, or an instruction to itself: the turn is
   over, so nothing follows it. Seen live: `Let me configure first, then build.`, `Answer them on the Route.`"
  #"(?i)(?:\A|[.!:\n]\s*)(?:let me|i\W?ll|i will|now i\W?ll|next,? i\W?ll|answer\b|reply\b)[^.?!\n]*[.!]?\s*\z")

(def ^:private offers-next-re
  "A reply that stops at a step and offers the rest. Seen live, on `Run the build or tests`: the
   configure step passed, no compile or test run has happened yet. The asker wanted the end state."
  #"(?i)\b(?:next step|next,? (?:i|we) (?:can|could|should)|would you like|want me to|shall i|should i|if you(?:'d| would)? (?:like|want)|let me know if|i can (?:now|then|next)|has(?:n't| not) (?:happened|run)(?: yet)?|no [a-z ]{1,40}(?:has|have) (?:happened|run|been)|ha(?:s|ve) not (?:yet )?(?:been )?(?:built|compiled|run)|not (?:yet )?(?:been )?(?:built|compiled|run)|still need(?:s)? to)\b")

(defn- stopped-short
  "The wake did some work and the reply ends by offering the rest. Asked once."
  [{:keys [done challenged?]} reply]
  (when (and (seq done) (not challenged?) (re-find offers-next-re (str reply)))
    "you stopped at a step and offered the next. Do the rest of what was asked now with an op, then report the result."))

(defn- unbacked-claim
  "The reply says work was done, and this wake did none: no run and nothing but reads. Asked once, so
   a claim about an earlier wake survives it. Seen live: a diff and a kernel-style run record, written
   by the model, for an edit never made."
  [world {:keys [m-state]} {:keys [done challenged?]} reply]
  (when-not challenged?
    (let [t (str/trim (str reply))
          plan (re-find ends-in-a-plan-re t)]
      (cond
        (and plan (not (re-find #"(?i)\byour?\b" (str plan))))
        "your reply ends on what you mean to do, and a reply ends the wake. Do it now with an op, or give the answer."
        (and (re-find claims-work-re t)
             (not (run-of-approval (act/db world) m-state))
             (every? #(#{"read" "resolve" "history"} (str (:op %))) done))
        "you claim work, and this wake ran nothing but reads. Do the work now with an op, or say you have not done it."))))

(def ^:private honest-re
  "A sentence that says a file could not be had is a true report, not a citation of it."
  #"(?i)could\s*n.?t|could not|cannot|can.t|can not|fail|not found|no such|does\s*n.?t exist|does not exist|missing|unable|denied|unread|not read|no file|absent|not there|not exist")

(defn- file-word? [^String s] (boolean (re-find #"[^/\s]\.[A-Za-z0-9]{1,8}$" s)))

(defn- base-name [^String s] (last (str/split s #"/")))

(defn- words-of [cmd] (remove #(str/starts-with? % "-") (str/split (str/trim (str cmd)) #"\s+")))

(defn- read-file-name
  "The file a read op asked for by name, or nil: a Field name is not a file."
  [{:keys [op args]}]
  (let [n (:name args)]
    (when (and (= "read" (some-> op name)) (string? n) (file-word? n)) n)))

(defn- wanted-and-got
  "{:failed names :got base names}: files this wake tried to read and could not, and the files it did
   read, by a read op or a command that exited 0."
  [{:keys [done failed]}]
  (let [runs (keep run-of-result done)
        ran-files (fn [pred] (for [{:keys [cmd exit]} runs :when (pred exit) w (words-of cmd) :when (file-word? w)] w))
        got (concat (keep read-file-name done) (ran-files #(= 0 %))
                    (keep #(get-in % [:args :args :name]) done))]
    {:failed (distinct (concat (keep read-file-name failed) (ran-files #(and (number? %) (not= 0 %)))))
     :got (set (map base-name got))}))

(defn- cited-as-read
  "Files the reply leans on though no read of them succeeded this wake, or nil. Seen live: a status that
   quoted a one-line `grid/STATUS.md`, after two reads and a `cat` of it had all failed."
  [st reply]
  (let [{:keys [failed got]} (wanted-and-got st)
        lines (str/split-lines (str reply))
        leaned-on? (fn [n] (let [b (base-name n)]
                             (some #(and (str/includes? % b) (not (re-find honest-re %))) lines)))]
    (seq (filter leaned-on? (remove #(got (base-name %)) failed)))))

(defn- cold-citation
  "A `path:line` in a reply from a wake that did nothing at all, or nil. Seen live, 2 runs in 4: a model
   made no call and wrote what the file said, how many files matched and a table of the record."
  [{:keys [done failed cold? log]} reply]
  ;; a wake a card started carries the thread's work log: it has read what it cites
  (when (and (not cold?) (empty? done) (empty? failed) (zero? (long (or (:n log) 0))))
    (when-let [cite (re-find #"[\w.-]+/[\w./-]*\.\w{1,8}:\d+" (str reply))]
      (str "not sent: your reply cites `" cite "`, and this wake has read nothing. Read it with the read tool, then answer from what comes back."))))

(defn- unread-challenge
  "Asked once: said again, the note below goes with it."
  [{:keys [cited?] :as st} reply]
  (when-not cited?
    (when-let [names (cited-as-read st reply)]
      (str "not sent: your reply cites " (str/join ", " (map #(str "`" % "`") names))
           ", and no read of it succeeded in this wake. Say that it could not be read, or read something that exists, and answer from that."))))

(def ^:private says-absent-re
  "A reply saying something is not there."
  #"(?i)does\s*n.?t exist|does not exist|do not exist|not mounted|isn.t mounted|no code|no such|there is no|there are no|there.s no|is not (?:here|there|present|available)|isn.t (?:here|there|present|available)|cannot find|can.t find|could not find|couldn.t find|not found|nothing (?:here|there)|is empty")

(defn- blind-run
  "The first command of this wake that failed with its errors thrown away, or nil. An exit 1 from
   `ls x 2>/dev/null` says nothing about x: seen live, root joined a mount's name onto a path, hid
   what `ls` said, and told a person the repository was not there."
  [world {:keys [m-state]} {:keys [done]}]
  (first (for [{:keys [cmd exit]} (remove nil? (conj (mapv run-of-result done) (run-of-approval (act/db world) m-state)))
               :when (and (number? exit) (not (zero? exit)) (re-find #"2>\s*/dev/null|&>\s*/dev/null" (str cmd)))]
           cmd)))

(defn- blind-challenge
  "Asked once: said again, the note below goes with it."
  [world ctx {:keys [blind?] :as st} reply]
  (when (and (not blind?) (re-find says-absent-re (str reply)))
    (when-let [cmd (blind-run world ctx st)]
      (str "not sent: you say something is not there, and your only check, `" cmd "`, failed with its errors hidden, "
           "which shows nothing. Check with read, outline or grep, or run it without the redirect, then answer."))))

(def ^:private docs-re #"(?i)\.(?:md|markdown|txt|rst|adoc|tex)$")

(defn- edited-name [{:keys [args]}]
  (when (= "file.edit" (some-> (:action args) name))
    (or (get-in args [:args :name]) (get-in args [:args "name"]))))

(defn- approved-result
  "What the card that started this wake did, as one of the wake's own results, or nil. The op ran in
   task.approve, outside any wake, so the work log never held it: no edit, so no VERIFY or FIX, stale
   files shown as current, and the turns of reading went on counting up until the wake stopped at 8."
  [db m-state]
  (when-let [t (and (= :approval-result (some-> (field m-state :message/kind) name keyword)) (field m-state :message/task))]
    (let [f (objects/fields db t) v #(get-in f [% :field/value])
          {:keys [op args]} (walk/keywordize-keys (v :task/op))
          {:keys [ok text]} (walk/keywordize-keys (v :task/result))
          ran (v :task/ran)
          action (some-> (:action args) name)]
      (cond
        (or (nil? op) (nil? (v :task/result))) nil
        (not ok) {:op op :args args :ok false :err {:code :failed :message (str text)}}
        (uuid? ran) {:op op :args args :ok true :result (lops/run-record db ran)}
        (= "file.edit" action) {:op op :args args :ok true :result {:name (get-in args [:args :name])}}
        :else {:op op :args args :ok true :result {:note (str text)}}))))

(defn- with-current
  "Results with each changed file as it now stands, for the work log. It is read here so the log stays pure."
  [db p results]
  (mapv (fn [{:keys [ok args] :as r}]
          (if-let [n (and ok (= "file.edit" (some-> (:action args) name)) (get-in args [:args :name]))]
            (assoc r :current (code/current-text db p n))
            r))
        results))

(defn- room-of
  "What the work log may hold and how long reading may go on, for this Principal's Model."
  [world p thread-id]
  (let [db (act/db world)
        fields (objects/fields db p)
        wide? (models/window-of db p thread-id)
        turns (fn [k wide narrow]
                (let [v (get-in fields [k :field/value])]
                  (cond (number? v) (long v) wide? wide :else narrow)))]
    {:open-chars (:serialize/open-chars (ser/policy db p thread-id))
     :act-after (turns :loop/act-after 20 worklog/act-after)
     :force-after (turns :loop/force-after 30 worklog/force-after)}))

(defn- thread-log
  "The thread's work log, with what the waking card did."
  [world p {:keys [m-state thread-id]}]
  (let [k [p thread-id]
        log (worklog/recall k)
        r (approved-result (act/db world) m-state)]
    (some-> (if r (worklog/remember k (worklog/add log (with-current (act/db world) p [r]))) log)
            (merge (room-of world p thread-id)))))

(defn- approved-edit
  "The file an approved card just changed, when that decision is what woke p, or nil: a card ends the
   wake that asked, so the change arrives here. Seen live: an approved edit, reported done, never built."
  [db m-state]
  (when-let [t (field m-state :message/task)]
    (let [f (objects/fields db t)
          op (get-in f [:task/op :field/value])
          args (or (:args op) (get op "args"))
          inner (or (:args args) (get args "args"))
          ok (get-in f [:task/result :field/value])]
      (when (and (= "file.edit" (some-> (or (:action args) (get args "action")) name)) (or (:ok ok) (get ok "ok")))
        (or (:name inner) (get inner "name"))))))

(defn- untested-edits
  "The code files this wake changed with nothing passing after the last change, or nil. Seen live: a
   one-line fix calling a function that does not exist, reported done, never built; 12 of 12."
  [{:keys [done approved-edit log]}]
  (if (seq (:edits log))
    ;; the work log runs across a thread's wakes, and it does not take `ls` for a passing build
    (worklog/untested log docs-re)
    (let [indexed (map-indexed vector done)
          edits (concat (when (and approved-edit (not (re-find docs-re (str approved-edit)))) [[-1 approved-edit]])
                        (for [[i r] indexed :let [n (edited-name r)] :when (and n (not (re-find docs-re (str n))))] [i n]))
          last-edit (first (last edits))]
      (when (and last-edit
                 (not-any? (fn [[i r]] (and (> i last-edit) (= 0 (:exit (run-of-result r))))) indexed))
        (vec (distinct (map second edits)))))))

(defn- verify-challenge
  "Asked once: said again, the note below goes with it."
  [{:keys [verify-asked?] :as st}]
  (when-not verify-asked?
    (when-let [names (untested-edits st)]
      (str "not sent: you changed " (str/join ", " (map #(str "`" % "`") names)) " and nothing has passed since. "
           "Use `test`, or `run` its build, fix what fails, then report. If it cannot be built here, say so plainly."))))

(def ^:private wrote-file-re
  #"(?i)\b(?:wrote|written|created|generated|saved)\b[^.\n]{0,80}?`?([\w./-]+\.[A-Za-z0-9]{1,8})\b")

(defn- claimed-writes
  "Files the reply says this wake wrote, when it changed nothing and ran nothing, or nil. Seen live: a
   generator never run, an existing file read and reported as new."
  [{:keys [done]} reply]
  (when-not (some #(or (edited-name %) (run-of-result %)) done)
    (seq (distinct (map second (re-seq wrote-file-re (str reply)))))))

(defn- write-challenge
  "Asked once: said again, the note below goes with it."
  [{:keys [write-asked?] :as st} reply]
  (when-not write-asked?
    (when-let [names (claimed-writes st reply)]
      (str "not sent: you say you wrote " (str/join ", " (map #(str "`" % "`") names))
           ", and this wake wrote nothing and ran nothing. Write it now, or say it already existed."))))

(def ^:private proof-re "The line a report of changed work ends on." #"(?im)^[\W_]*(?:not\s+)?proved:\s*\S")

(defn- wants-proof?
  "This wake's thread changed something and the reply does not say how the goal was shown to hold."
  [{:keys [work?]} {:keys [log]} reply]
  (and work? (seq (:edits log)) (not (re-find proof-re (str reply)))))

(defn- goal-challenge
  "Asked once: a build that passes is not `stops printing`. Seen live, twice, on one task: both tries
   built, and neither run looked at whether the line still printed."
  [ctx {:keys [goal-asked?] :as st} reply]
  (when (and (not goal-asked?) (wants-proof? ctx st reply))
    (str "not sent: check the goal, not only the build. The task said: \"" (asked-goal ctx) "\". "
         "If nothing yet shows that holds, show it now with read or run. Then end your reply with "
         "`Proved: <what you ran or read, and what it showed>`, or `Not proved: <why>`.")))

(defn- with-unread-note
  "The text a person gets: what still rests on a failed read says so, in the kernel's words."
  [world ctx st reply]
  (let [names (cited-as-read st reply)
        cmd (when (re-find says-absent-re (str reply)) (blind-run world ctx st))
        untested (untested-edits st)
        claimed (claimed-writes st reply)]
    (str reply
         (when (wants-proof? ctx st reply) "\n\nNot proved: the reply does not say how the goal was checked.")
         (when names (str "\n\n(Not verified: no read of " (str/join ", " (map #(str "`" % "`") names)) " succeeded in this wake.)"))
         (when cmd (str "\n\n(Not verified: the check behind this, `" cmd "`, failed with its errors hidden.)"))
         (when untested (str "\n\n(Not verified: " (str/join ", " (map #(str "`" % "`") untested)) " changed, and nothing passed after that.)"))
         (when claimed (str "\n\n(Not verified: this wake wrote nothing and ran nothing, so " (str/join ", " (map #(str "`" % "`") claimed)) " is not its work.)")))))

(defn- unmounted-dir
  "A directory the question named that this shell command reaches outside p's mounts, or nil. A person
   naming a path is asking for it to be mounted: in 9 runs of one-sentence setup root reached for a
   shell, was stopped at a card, and asked a person for help, with add_project a tool away. A path that
   is not there at all, with a directory named, is that directory misremembered: seen live, a long
   path came back with its middle gone."
  [world p ctx cmd]
  (let [db (act/db world)]
    (when (and (string? cmd) (resolve/resolves? db p "action/project.add" :evaluate))
      (let [dirs (named-dirs db ctx)
            mounts (map :path (projects/projects-of db p))
            words (map #(str/replace % #"[\"'/]+$" "") (str/split cmd #"[\s\"'=]+"))
            lost? (fn [^String w] (and (str/starts-with? w "/") (str/includes? (subs w 1) "/")
                                       (not (.exists (io/file w)))))]
        (first (for [d dirs w words
                     :when (and (or (under? d w) (lost? w)) (not-any? #(under? % w) mounts))]
                 d))))))

(defn- refuse-unmounted
  "The op with its refusal, when a shell command reaches a named directory that is not mounted."
  [world p ctx {:keys [args] :as o}]
  (let [cmd (or (get-in args [:args :cmd]) (get-in args [:args "cmd"]))]
    (if-let [d (unmounted-dir world p ctx cmd)]
      (assoc o :err {:code :not-mounted
                     :message (str "not run: `" d "` is outside your mounts, so nothing can read it yet. "
                                   "Call add_project {name, path} once per project there; its result has the README.")})
      o)))

(def ^:private widening
  "What reaches further than a Principal already can."
  #{"project.add" "workspace.mount" "adapter.unconfine" "grant" "principal.create" "delegate"})

(defn- refuse-widening
  "The op with its refusal, in a wake another agent started: reach comes from a person or from root.
   Root on a wide leash is who an agent escalates to, and decides for itself; a Principal that holds a
   wide Grant it was given is not."
  [world {:keys [sender]} p {:keys [op args] :as o}]
  (let [db (act/db world)
        what (str (or (some-> (:action args) name) (some-> op name)))]
    (if (and (widening what) sender (= :process (:principal/driven-by (objects/entity db sender)))
             (not (true? (get-in (objects/fields db p) [:principal/wide-leash :field/value]))))
      (assoc o :err {:code :not-from-a-person
                     :message (str "not run: " (name-of db sender) " asked for this, and an agent cannot widen its reach "
                                   "through you. Answer it that a person must grant this from their own session.")})
      o)))

(defn- read-hint
  "A name that resolves to nothing, said so that the next op can be right. Seen live: root asked for
   paths under a directory it had been told about, four ways, and then asked a person for help."
  [db p {:keys [op args err] :as r}]
  (if (and (= "read" (some-> op name)) (string? (:name args))
           (= :unresolved (some-> (:code err) name keyword)))
    (assoc-in r [:err :message]
              (str (if (:field args)
                     (str "does not resolve: `" (:name args) "` has no Field `" (:field args) "`. Read it without `field`.")
                     (code/path-hint db p (:name args)))
                   " `outline` lists what is there."
                   (when (resolve/resolves? db p "action/project.add" :evaluate)
                     " A directory a person named is add_project {name, path}.")))
    r))

(defn- writes?
  "Did this op change something a later run should see again?"
  [{:keys [op args]}]
  (or (= "set-field" (some-> op name))
      (= "file.edit" (some-> (:action args) name))))

(defn- progress
  "{:seen :fresh?}: a turn makes progress when an op succeeds that this wake has not already run. A
   change makes the runs fresh again, since edit, test, edit, test is the work, not a loop."
  [seen results]
  (let [ok (filter :ok results)
        key-of (fn [{:keys [op args]}] [(some-> op name) (pr-str args)])
        run? (fn [[op args]] (and (= "act" op) (str/includes? args "task.run")))
        base (if (some writes? ok) (set (remove run? seen)) seen)]
    {:fresh? (boolean (some #(not (contains? base (key-of %))) ok))
     :seen (into base (map key-of) ok)}))

(defn- open-file-result
  "A whole read of a file the prompt already holds: answered with where it is, at no cost."
  [{:keys [op args]}]
  {:op op :args args :ok true
   :result {:note (str (:name args) " is unchanged and open in your prompt, under Open files. Do not read it again.")}})

(defn- run-ops!
  "Run the ops the model emitted and carry the results into the next iteration. What succeeded is
   kept so an identical op later in the wake is answered from here, not run again."
  [world p {:keys [hop max-ops message route thread-id work?] :as ctx} {:keys [i done] :as st} evaluation ops]
  (let [log (worklog/settle (:log st))
        open (when work? (filter #(worklog/open? log %) ops))
        ;; the same failed command, unchanged, is answered with what failed and does not run
        repeats (when work? (filter #(worklog/repeated-run log %) ops))
        to-run (vec (remove (set (concat open repeats)) ops))
        results (-> (mapv #(read-hint (act/db world) p %)
                          (lops/execute! world p evaluation hop to-run {:max-ops max-ops :message message :route route
                                                                        :thread thread-id :done done :shown (:shown st)}))
                    (into (map open-file-result) open)
                    (into (map (fn [{:keys [op args] :as o}]
                                 {:op op :args args :ok false
                                  :err {:code :fix-first :message (worklog/repeated-run log o)}}))
                          repeats))
        failed? (fn [r] (or (:err r) (false? (:ok r))))
        {:keys [seen fresh?]} (progress (:seen st #{}) results)
        log (when work? (worklog/remember [p thread-id]
                                    (worklog/add (merge log (room-of world p thread-id))
                                                 (with-current (act/db world) p results))))
        ;; in a work wake a turn of reading stops counting once it has gone on long enough
        fresh? (and fresh? (not (and work? (worklog/hold-reading? log))))]
    (record-evaluation! world p evaluation i ops results)
    (when (and fresh? (> (turn-limit ctx (assoc st :progress-at i)) (turn-limit ctx st)))
      (set-loop-field! world p :loop/turn-limit :number (turn-limit ctx (assoc st :progress-at i))))
    (-> st
        (dissoc :thinking-so-far :retry-of)
        (assoc :seen seen :shown results)
        (cond-> log (assoc :log log))
        (cond-> fresh? (assoc :progress-at i))
        (update :i inc)
        (update :evaluations conj evaluation)
        (assoc :prev-failed? (boolean (some failed? results)))
        (update :made into (created-by-ops results))
        (update :done into (filter :ok results))
        (update :interim? #(or % (boolean (some pending? results))))
        (update :failed into (remove :ok results)))))

(defn- turns-so-far
  "A resumed wake's own turns, oldest first, so it goes on instead of starting again: a restart is
   routine now that a world moves itself onto new releases, and the conversation should outlive it."
  [db p {:keys [m-state message]}]
  (when (and (pos? (long (or (field m-state :message/resumed) 0))) (:serialize/conversation (ser/policy db p (thread/thread-of db message))))
    (->> (objects/objects-with-field db :evaluation/message)
         (filter (fn [e] (let [f (objects/fields db e)]
                           (and (= message (get-in f [:evaluation/message :field/value]))
                                (= p (get-in f [:evaluation/initiator :field/value]))
                                (= :done (some-> (get-in f [:evaluation/state :field/value]) name keyword))
                                (seq (get-in f [:evaluation/ops :field/value]))))))
         (sort-by #(:object/created-seq (objects/entity db %)))
         vec)))

(defn- run-iterations!
  "Evaluate, run the ops the model emitted, evaluate again with their results in the working set,
   until an outcome carries no ops (spec 9.8 steps 3 to 6). A steered call was cut for new
   direction: what ran stands, and the same turn starts again without spending an iteration."
  [world p ctx compressed?]
  (note-choice! world p ctx)
  (set-loop-field! world p :loop/turn-limit :number (turn-limit ctx {}))
  ;; a card ends a wake and its decision starts the next, so the work is the thread's, not the wake's
  ;; With a real window a wake is a plain conversation: every call and result stays in the prompt, in
  ;; order. The work log's note, its reading limits and its forced moves exist to survive 8k; on a
  ;; window they took the model's own history away. At turn 89 it could see four turns.
  (let [ctx (assoc ctx :work? (and (not (models/window-of (:db ctx) p (:thread-id ctx)))
                                   (worklog/work? (str (field (:m-state ctx) :message/text) " "
                                                  (some->> (:thread-id ctx) (objects/state (:db ctx))
                                                           (#(field % :message/text)))))))]
  (loop [{:keys [i] :as st} {:i 1 :evaluations (or (turns-so-far (:db ctx) p ctx) []) :prev-failed? false :made [] :done []
                             :interim? false :failed []
                             :log (when (:work? ctx) (thread-log world p ctx))
                             :approved-edit (approved-edit (act/db world) (:m-state ctx))}]
    (if-let [stop (stop-reason world p ctx st)]
      (do (when (> i (+ 2 (turn-limit ctx st))) (tell-stopped! world p ctx st))
          (stall! world p stop))
      (let [_ (when (> i 1) (set-loop-field! world p :loop/iteration :number i))
            {:keys [err evaluation outcome ids]} (call-model! world p ctx compressed? st)
            st (cond-> st ids (assoc :ids ids))
            moved (when-not err (fallback-now world p ctx st outcome))]
        (cond
          err (stall! world p (str "evaluate: " (:code err) " " (:message err)))
          (steered? outcome) (recur st)
          (reasoning-cut? outcome st) (recur (assoc st :reasoning-spent true :retry-of evaluation
                                                    :thinking-so-far (:thinking outcome)))
          moved (recur (assoc st :fallback moved))
          (not= :done (:state outcome)) (end-undone! world p ctx outcome)
          :else
          (let [{:keys [ops reply malformed-fences]} (lops/parse outcome {:db (act/db world) :p p :ids (:ids st)})]
            (note-malformed! world p evaluation (or malformed-fences 0))
            (let [answers (answers-of world p ctx ops)
                  held (set answers)
                  answering? (and (seq answers) (= (count answers) (count ops)))
                  {sent-text :text sent-refs :refs} (when answering? (:args (first answers)))
                  text (if answering? sent-text reply)
                  last-turn? (> i (turn-limit ctx st))
                  usable? (and (not (str/blank? text)) (nil? (not-an-answer text)))]
              (cond
                ;; turns are over, so ops written now cannot run: what the model has is the answer. With
                ;; nothing to say, the kernel says where it got to.
                (and last-turn? usable?)
                (finish-turn! world p ctx (assoc st :answer-refs sent-refs) evaluation outcome (with-unread-note world ctx st text))
                last-turn?
                (recur (run-ops! world p ctx st evaluation
                                 [{:raw (subs (str text) 0 (min 200 (count (str text))))
                                   :err {:code :no-turns
                                         :message "You wrote no answer. Write it now as plain text, not a tool call: what you found, with file:line, and what is unfinished."}}]))
                (and (seq ops) (not answering?))
                (let [st' (run-ops! world p ctx st evaluation
                                    (mapv #(if (held %) (assoc % :err answer-too-soon) (refuse-widening world ctx p (refuse-unmounted world p ctx %))) ops))]
                  (if (:interim? st')
                    (wait-for-approval! world p ctx st' evaluation)
                    (recur st')))
                ;; said as a send or as text, the same test: seen live, a plan sent to the asker walked past it
                (or (not-an-answer text) (unbacked-claim world ctx st text) (stopped-short st text))
                (recur (run-ops! world p ctx (assoc st :challenged? true) evaluation
                                 [{:raw (subs text 0 (min 200 (count text)))
                                   :err {:code :not-an-answer
                                         :message (str "not sent: " (or (not-an-answer text) (unbacked-claim world ctx st text)
                                                                        (stopped-short st text)))}}]))
                (cold-citation st text)
                (recur (run-ops! world p ctx (assoc st :cold? true) evaluation
                                 [{:raw (subs text 0 (min 200 (count text)))
                                   :err {:code :not-an-answer :message (cold-citation st text)}}]))
                (unread-challenge st text)
                (recur (run-ops! world p ctx (assoc st :cited? true) evaluation
                                 [{:raw (subs text 0 (min 200 (count text)))
                                   :err {:code :not-an-answer :message (unread-challenge st text)}}]))
                (blind-challenge world ctx st text)
                (recur (run-ops! world p ctx (assoc st :blind? true) evaluation
                                 [{:raw (subs text 0 (min 200 (count text)))
                                   :err {:code :not-an-answer :message (blind-challenge world ctx st text)}}]))
                (verify-challenge st)
                (recur (run-ops! world p ctx (assoc st :verify-asked? true) evaluation
                                 [{:raw (subs text 0 (min 200 (count text)))
                                   :err {:code :not-an-answer :message (verify-challenge st)}}]))
                (write-challenge st text)
                (recur (run-ops! world p ctx (assoc st :write-asked? true) evaluation
                                 [{:raw (subs text 0 (min 200 (count text)))
                                   :err {:code :not-an-answer :message (write-challenge st text)}}]))
                (goal-challenge ctx st text)
                (recur (run-ops! world p ctx (assoc st :goal-asked? true) evaluation
                                 [{:raw (subs text 0 (min 200 (count text)))
                                   :err {:code :not-an-answer :message (goal-challenge ctx st text)}}]))
                :else (finish-turn! world p ctx (assoc st :answer-refs sent-refs) evaluation outcome (with-unread-note world ctx st text)))))))))))

(defn cycle!
  "One cycle for principal p woken by a Message on a Route. Returns {:evaluation :reply :iterations},
   {:evaluation :quiet :iterations}, {:forwarded :reply} or {:stalled reason}."
  [world p {:keys [message] :as w}]
  (if (objects/retired? (act/db world) p)
    (drop-wake! world p w)
    (let [{:keys [model] :as ctx} (prepare-wake world p w)
          interrupted? (volatile! false)]
     (if (and (pos? (long (or (field (:m-state ctx) :message/resumed) 0)))
              (thread/replied-since? (:db ctx) (:thread-id ctx) message p))
      ;; it came back from a restart, and the thread has had its answer since: seen live, root
      ;; reported a release done, then read all its files again to do the work it had finished
      {:stalled "answered before this resume"}
      (do
      (set-loop-field! world p :loop/current [:ref :message] message)
      (set-loop-field! world p :loop/iteration :number 1)
      (try
        (let [compressed? (and model (conversation/maybe-compress! world p message))]
          (if model
            (run-iterations! world p (assoc ctx :changes (changes/see! world p #(name-of (act/db world) %))) compressed?)
            (forward-wake! world p ctx)))
        ;; the world is stopping: loop/iteration stays above zero and the wake resumes at the next boot
        (catch InterruptedException _
          (vreset! interrupted? true)
          {:stalled "interrupted"})
        (catch Throwable t
          (timbre/error t "loop cycle failed" {:principal p})
          (try (set-loop-field! world p :loop/stalled :string (str "cycle: " (ex-message t))) (catch Throwable _ nil))
          {:stalled (ex-message t)})
        (finally
          (when-not @interrupted?
            (try (set-loop-field! world p :loop/iteration :number 0) (catch Throwable _ nil))))))))))

(defn accepts-from
  "Principals whose loop-sent Messages wake p: the Field loop/accepts-from when set, else p's
   creator and the Principals p created (decisions 2026-10-30 'loops talk to loops')."
  [db p]
  (let [explicit (get-in (objects/fields db p) [:loop/accepts-from :field/value])]
    (if (seq explicit)
      (set explicit)
      (into (if-let [c (:object/created-by (objects/entity db p))] #{c} #{})
            (d/q '[:find [?id ...] :in $ ?p :where [?o :object/created-by ?p] [?o :object/class :principal] [?o :object/id ?id]]
                 db p)))))

(defn- own-approval-result?
  "Is the Message the decision on an approval Task the recipient asked for (message/kind
   approval-result, task/requested-by the recipient)? Decisions 2026-10-30 'approval gates': the
   requester's loop wakes to continue from what was approved or denied, whoever decided."
  [db recipient message]
  (let [f (objects/fields db message)]
    (and (= :approval-result (some-> (get-in f [:message/kind :field/value]) name keyword))
         (when-let [t (get-in f [:message/task :field/value])]
           (let [tf (objects/fields db t)
                 op (get-in tf [:task/op :field/value])
                 args (or (:args op) (get op "args"))
                 ok (get-in tf [:task/result :field/value])]
             (and (= recipient (get-in tf [:task/requested-by :field/value]))
                  ;; an approved delegation has nothing to say yet: the subagent's result is the next
                  ;; wake, and a model woken now answers from nothing (seen live: an invented quote)
                  (not (and (= "delegate" (some-> (or (:action args) (get args "action")) name))
                            (or (:ok ok) (get ok "ok"))))))))))

(defn- own-trigger?
  "Is the Message a Trigger's firing addressed to the Trigger's owner's recipient? A
   Trigger's Message carries via = the Trigger and message/kind :trigger; it wakes its recipient
   although via is set, at hop 0, and the recipient's reply to itself wakes nobody."
  [db recipient message]
  (let [f (objects/fields db message)]
    (and (= :trigger (some-> (get-in f [:message/kind :field/value]) name keyword))
         (when-let [t (get-in f [:message/trigger :field/value])]
           (let [tf (objects/fields db t)]
             (= recipient (or (get-in tf [:trigger/to :field/value]) (get-in tf [:trigger/owner :field/value]))))))))

(defn loop-wake?
  "Does a Message a loop sent wake recipient: a process Principal, the sender one it accepts, and the
   hop count below max-hops, on any Route (decisions 2026-10-30 'loops talk to loops', amended); or
   the decision on an approval Task the recipient asked for; or one of the recipient's own Triggers
   firing (both wake although via is set)."
  [db recipient p-state message]
  (let [f (objects/fields db message)
        from (get-in f [:message/from :field/value])
        hop (long (or (get-in f [:message/hop :field/value]) 0))]
    (and (= :process (:principal/driven-by p-state))
         (or (own-approval-result? db recipient message)
             (own-trigger? db recipient message)
             ;; a subagent's parent always hears from it, past any hop count
             (delegate/child-of? db recipient from)
             (and (< hop max-hops)
                  (contains? (accepts-from db recipient) from))))))

(defn- wakes
  "The wake records a committed batch carries: delivered Messages to process Principals on their
   inboxes. A delivery an Evaluation produced (event/via set) wakes only when loop-wake? admits it;
   a Principal writing to itself does not wake itself (it is not in its own accepts-from)."
  [world events]
  (let [db (act/db world)]
    (for [{:keys [event/kind event/body event/via event/id]} events
          :when (= kind :delivered)
          :let [{:keys [message route recipient]} body
                p-state (objects/state db recipient)]
          ;; a directive is the kernel's, never a wake (decisions 2026-10-31); steering joins the running
          ;; wake (knowledge.world.loop.steer) and wakes nothing here
          :when (not (#{:directive :steer} (some-> (field (objects/state db message) :message/kind) name keyword)))
          :when (and (not (:object/retired p-state))
                      (if (nil? via) (inbox? p-state route) (loop-wake? db recipient p-state message)))]
      {:principal recipient :message message :route route :wake id})))

(defn concurrency
  "How many Principals evaluate at once: loop/concurrency on the world Principal, else the default."
  [world]
  (let [db (act/db world)
        v (field (objects/state db (:world-id world)) :loop/concurrency)]
    (if (and (number? v) (pos? v)) (long v) default-concurrency)))

(defn- virtual-factory [^String prefix] (.factory (.name (Thread/ofVirtual) prefix (long 0))))

(defn- lane
  "The serial lane of a Principal: one single-thread executor on a virtual thread,
   made on first use, so one Principal's wakes run in order while different Principals run at once."
  [lanes p]
  (or (get @lanes p)
      (get (swap! lanes update p #(or % (Executors/newSingleThreadExecutor (virtual-factory (str "ke-loop-" (subs (str p) 0 8) "-"))))) p)))

(defn start!
  "Subscribe to the log; a dispatcher thread resolves wakes and hands each to its Principal's lane;
   a semaphore of `concurrency` permits bounds how many cycles run at once. Returns the loop component."
  [world]
  (let [dispatcher (Executors/newSingleThreadExecutor
                     (reify ThreadFactory
                       (newThread [_ r] (doto (Thread. ^Runnable r "ke-resident-loop") (.setDaemon true)))))
        lanes (atom {})
        permits (Semaphore. (int (concurrency world)) true)
        running (atom true)
        pump! (fn pump! [p]
                (.execute ^ExecutorService (lane lanes p)
                          (fn []
                            (when @running
                              (.acquire permits)
                              (try
                                (when @running
                                  ;; nil: the wake this turn was for was dequeued
                                  (when-let [w (queue/take-next! world (act/db world) p)]
                                    (try (cycle! world p w)
                                         (finally
                                           ;; steering that came as the wake ended is the next wake
                                           (let [db (act/db world)]
                                             (doseq [s (reverse (queue/end! world db p))]
                                               (queue/enqueue! world db (assoc s :principal p) :head? true)
                                               (pump! p)))))))
                                (finally (.release permits)))))))
        key (log/subscribe! (:log world)
                            (fn [batch]
                              (when (and @running (some #(= :delivered (:event/kind %)) batch))
                                ;; off the writer thread: the dispatcher resolves the wakes, the lanes run the cycles
                                (.execute ^ExecutorService dispatcher
                                          (fn []
                                            (doseq [w (try (doall (wakes world batch))
                                                           (catch Throwable t (timbre/error t "loop wake failed") nil))
                                                    :when @running]
                                              (queue/enqueue! world (act/db world) w)
                                              (pump! (:principal w))))))))]
    (reset! (:loop-hooks world) {:pump! pump! :running running})
    ;; a restart keeps its place in line: the wakes loop/queue names run again, in order, behind
    ;; whatever the stopped process was in the middle of
    (doseq [[p e] (queue/restore! world (act/db world))]
      (timbre/info "queued wake restored" {:principal p :message (:message e)})
      (pump! p))
    (doseq [[p _] (resume/resume! world (act/db world))]
      (pump! p))
    (timbre/info "resident loop started" {:concurrency (.availablePermits permits)})
    {:executor dispatcher :lanes lanes :permits permits :running running :key key}))

(defn stop! [{:keys [^ExecutorService executor lanes running key]} world]
  (when executor
    (reset! running false)
    (log/unsubscribe! (:log world) key)
    (.shutdownNow executor)
    (doseq [^ExecutorService e (vals @lanes)] (.shutdownNow e))
    (.awaitTermination executor 2 TimeUnit/SECONDS)
    ;; a wake interrupted here still writes its last Fields: the log closes after them
    (doseq [^ExecutorService e (vals @lanes)] (.awaitTermination e 2 TimeUnit/SECONDS))))
