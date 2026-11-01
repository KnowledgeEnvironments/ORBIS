(ns knowledge.world.evaluate.core
  "Evaluations (spec 5.4, 9.3, 9.6, 14.5). `evaluate!` runs as one Principal under one Grant:
   1. queued: the initiator resolves the Model for op :evaluate (the residency rule of 8.3 is
      the Grant on the Model; a Model with residency :external is refused with err residency
      until a residency Grant rule exists, see CONTRACT-NEEDS.md); the Evaluation Object is
      created; the Serialization is rendered and named; `evaluated` queued.
   2. running: the bytes go to the Model's adapter on another thread; `evaluated` running.
   2b. streaming: content deltas from the Model are coalesced (at most one event per 250 ms or
      200 chars) into `evaluated` streaming events {:delta :seq-in-stream}, and reasoning deltas
      likewise into {:thinking-delta :thinking-chars} (thinking-chars cumulative), interleaved on
      the same stream (spec 8.4 partial output; decisions 2026-10-30). A client that ignores them
      loses nothing: done carries the text and evaluation/thinking the whole reasoning.
   3. done: `evaluated` done with the outcome {:text :usage} and the cost {:tokens-in :tokens-out}.
   4. failed: `evaluated` failed with the error and the elapsed ms in the outcome. The Serialization remains.
   The reasoning mode the request went out with is evaluation/reasoning (:on or :off, decisions
   2026-10-30 'reasoning is asked for'): the caller resolves it (the loop, from the Message, the
   Principal and the Model); absent, the adapter's rule applies (a Model on :auto does not reason).
   4b. a stalled Model: the call carries a deadline (started + model/timeout-s)
      and an idle timeout between deltas (model/idle-timeout-s, 120 s); either fails the
      Evaluation with `timeout after N s, M chars received` or `no output for N s ...`; the loop's
      own wait uses the same deadline and `fail!` ends the Evaluation on the log if the adapter
      never returns. No Evaluation stays `running` past its deadline.
   5. cancelled (decisions 2026-10-30): `act {object action cancel}` on the Evaluation, or on a
      Principal for its current wake, by a Principal that resolves the target for :evaluate or
      :write; the Model call is aborted and `evaluated` cancelled {by partial-chars} is appended;
      no done follows. The running set is process state (`running`), not a fact: the log holds
      the request and the outcome. Steering cuts a call the same way, with reason steer, and the
      wake goes on (decisions 2026-10-31). Either way the text and reasoning so far stay on the
      Evaluation, as evaluation/partial and evaluation/thinking.
   On done, two more Fields land on the Evaluation (decisions 2026-10-30): evaluation/thinking
   (the Model's reasoning, when it returned any; bytes above 4 KiB go to a blob) and
   evaluation/context {serialization-bytes prompt-tokens completion-tokens objects dropped}
   (objects = Objects rendered, dropped = items summarised or left out by the serializer policy).
   `evaluated` events go to the Evaluation's, the Model's and the initiator's streams (7.1).
   Budget Triggers are not checked yet. Returns {:ok {:evaluation id :done future}}."
  (:require [knowledge.world.models :as models]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.model-api :as model]
            [knowledge.world.cost :as cost]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.image :as image]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.serialize.core :as ser]
            [taoensso.timbre :as timbre]))

(defn- evaluated! [world evaluation p model grant body]
  (act/append! world (log/pending :evaluated evaluation p (assoc body :evaluation evaluation :model model)
                                  :streams #{model p} :delegation grant)))

(defn- field-set! [world evaluation p grant k type v]
  (act/append! world (log/pending :field-set evaluation p {:name k :type type :value v :reader :both}
                                  :delegation grant :via evaluation)))

(defn- record-text!
  "A text Field on the Evaluation; above 4 KiB it goes to a blob. Nothing for empty text."
  [world evaluation principal grant k ^String text]
  (when (seq text)
    (let [bs (.getBytes text "UTF-8")]
      (if (> (alength bs) 4096)
        (field-set! world evaluation principal grant k :bytes bs)
        (field-set! world evaluation principal grant k :string text)))))

(defn- record-done!
  "evaluation/context and, when present, evaluation/thinking."
  [world {:keys [evaluation principal grant serialization dropped objects compressed standing-tokens budget-tokens over-budget] :as job} {:keys [usage thinking]}]
  (let [objects (long (or objects (count (:serialization/working-set serialization))))] ; rendered, not asked
    (field-set! world evaluation principal grant :evaluation/context :map
                {:serialization-bytes (get-in serialization [:serialization/size :bytes] 0)
                 :standing-tokens (long (or standing-tokens 0))
                 :budget-tokens (long (or budget-tokens 0))
                 :over-budget (boolean over-budget)
                 :compressed (boolean compressed)
                 :prompt-tokens (or (:prompt-tokens usage) 0)
                 :completion-tokens (or (:completion-tokens usage) 0)
                 :objects objects
                 :dropped (long (or dropped 0))
                 :reasoning (name (or (:reasoning job) :off))})
    ;; evaluation/cost at the Model's prices and the running books
    (try (cost/charge! world {:evaluation evaluation :principal principal :model (:model job) :grant grant :usage usage})
         (catch Throwable t (timbre/warn t "cost books failed" {:evaluation evaluation})))
    (when (string? thinking)
      (record-text! world evaluation principal grant :evaluation/thinking thinking))))

(defonce ^:private running
  ;; evaluation-id -> {:principal :model :started :cancel (atom) :by (atom) :reason (atom) :ended (promise)}:
  ;; the Evaluations whose Model call is in flight in this process, so a cancel can reach them
  (atom {}))

(defn running-evaluation
  "The latest in-flight Evaluation a Principal initiated, or nil."
  [p]
  (->> @running (filter (fn [[_ r]] (and (= p (:principal r)) (nil? (:scratch r))))) (sort-by (comp :started val) >) ffirst))

(defn cancel-scratch!
  "Abort every Model call of an off-the-record session. A scratch's Evaluations are not the real
   world's: running-evaluation never returns them, so a person's STOP on a Principal leaves them."
  [scratch]
  (doseq [[_ r] @running :when (= scratch (:scratch r))]
    (reset! (:cancel r) true)))

(def stream-flush-ms 250)
(def stream-flush-chars 200)

(defn streamer
  "A coalescing delta sink: returns {:on-delta f :flush! g}. Deltas accumulate; emit! is called with
   the joined text and a 1-based seq-in-stream when the buffer reaches stream-flush-chars or when
   stream-flush-ms passed since the last emit; flush! emits what is left. Single-threaded use:
   the adapter calls on-delta from the evaluating thread."
  [emit!]
  (let [buf (StringBuilder.) n (volatile! 0) last (volatile! (System/currentTimeMillis))
        emit (fn [] (when (pos? (.length buf))
                      (let [text (str buf)] (.setLength buf 0) (vreset! last (System/currentTimeMillis))
                        (emit! text (vswap! n inc)))))]
    {:on-delta (fn [^String d]
                 (.append buf d)
                 (when (or (>= (.length buf) stream-flush-chars)
                           (>= (- (System/currentTimeMillis) @last) stream-flush-ms))
                   (emit)))
     :flush! emit}))

(defn- finish!
  "Take the Evaluation out of the running set; true for the caller that did, so exactly one of the
   job thread and a fail! from outside appends the terminal event."
  [evaluation]
  (let [[before _] (swap-vals! running dissoc evaluation)]
    (contains? before evaluation)))

(defn- run-job!
  "The running, streaming, done or failed half, on the calling thread. Returns the outcome. The
   Model call carries a deadline, started + model/timeout-s, and the adapter's idle timeout: a stalled stream fails with the reason instead of staying `running`."
  [world {:keys [evaluation principal model grant adapter] :as job}]
  (let [started (System/currentTimeMillis)
        deadline (+ started (* 1000 (long (:timeout-s adapter 600))))
        elapsed #(- (System/currentTimeMillis) started)
        cancel (atom nil) by (atom nil) reason (atom nil) ended (promise)
        content-chars (volatile! 0)
        partial-text (StringBuilder.) partial-thinking (StringBuilder.) ; kept when the call is cut short
        emit! (fn [body] (try (evaluated! world evaluation principal model grant (assoc body :state :streaming))
                              (catch Throwable t (timbre/warn t "streaming event failed" {:evaluation evaluation}))))
        {content-delta :on-delta flush! :flush!} (streamer (fn [text k] (emit! {:delta text :seq-in-stream k})))
        chars (volatile! 0)
        {on-thinking :on-delta flush-thinking! :flush!} (streamer (fn [text _] (emit! {:thinking-delta text :thinking-chars (vswap! chars + (count text))})))
        ;; reasoning precedes content on the wire; the first content delta flushes what thinking is buffered
        on-delta (fn [d] (vswap! content-chars + (count d)) (.append partial-text ^String d) (flush-thinking!) (content-delta d))
        keep-thinking (fn [d] (.append partial-thinking ^String d) (on-thinking d))]
    (swap! running assoc evaluation {:principal principal :model model :grant grant :started started :deadline deadline
                                     :cancel cancel :by by :reason reason :ended ended :scratch (:scratch world)})
    (try
      (evaluated! world evaluation principal model grant {:state :running})
      (let [outcome (model/evaluate! adapter (:serialization job) {:on-delta on-delta :on-thinking keep-thinking
                                                                   :cancel cancel :reasoning (:reasoning job)
                                                                   :deadline deadline})]
        (flush-thinking!)
        (flush!)
        (when (finish! evaluation) ; else fail! from outside already ended it on the log
          (case (:state outcome)
            :done (do (evaluated! world evaluation principal model grant
                                  {:state :done :outcome (select-keys outcome [:text :usage :finish]) :cost (:cost outcome)})
                      (record-done! world job outcome))
            :cancelled (let [seq (evaluated! world evaluation principal model grant
                                             (cond-> {:state :cancelled :partial-chars (long @content-chars)}
                                               @by (assoc :by @by)
                                               (= :steer @reason) (assoc :reason :steer)))]
                         ;; what the call had said by then stays with it (decisions 2026-10-31)
                         (record-text! world evaluation principal grant :evaluation/partial (str partial-text))
                         (record-text! world evaluation principal grant :evaluation/thinking (str partial-thinking))
                         (deliver ended seq))
            (do (when (= :reasoning-cap (:reason outcome))
                  (record-text! world evaluation principal grant :evaluation/thinking (str partial-thinking)))
                (evaluated! world evaluation principal model grant
                            {:state :failed :outcome (cond-> {:error (str (:error outcome)) :elapsed-ms (elapsed)}
                                                       (:usage outcome) (assoc :usage (:usage outcome)))}))))
        (cond-> (assoc outcome :elapsed-ms (elapsed))
          @by (assoc :by @by)
          (= :reasoning-cap (:reason outcome)) (assoc :thinking (str partial-thinking))
          (and (= :cancelled (:state outcome)) (= :steer @reason)) (assoc :reason :steer)))
      (catch Throwable t
        (timbre/error t "evaluation failed" {:evaluation evaluation})
        (when (finish! evaluation)
          (try (evaluated! world evaluation principal model grant {:state :failed :outcome {:error (str (ex-message t)) :elapsed-ms (elapsed)}})
               (catch Throwable _ nil)))
        {:state :failed :error (str (ex-message t)) :elapsed-ms (elapsed)})
      (finally
        (finish! evaluation)
        (deliver ended nil)))))

(defn fail!
  "End a running Evaluation from outside with a reason: the loop calls it when its
   own wait passes. The Model call is aborted through the cancel atom (as {:timeout reason}); if
   the job thread has not ended the Evaluation on the log within 3 s, this call appends the failed
   event itself. Returns the failed outcome, or nil when nothing was running."
  [world evaluation reason]
  (when-let [{:keys [principal model grant cancel ended started]} (get @running evaluation)]
    (reset! cancel {:timeout reason})
    (deref ended 3000 nil)
    (when (finish! evaluation)
      (try (evaluated! world evaluation principal model grant
                       {:state :failed :outcome {:error (str reason) :elapsed-ms (- (System/currentTimeMillis) started)}})
           (catch Throwable t (timbre/warn t "failed event after fail!" {:evaluation evaluation}))))
    {:state :failed :error (str reason) :elapsed-ms (- (System/currentTimeMillis) started)}))

(defn orphaned!
  "End an Evaluation a stopped process left unfinished: nothing is running it any more, so it fails
   with reason restart and the log stops saying it is running."
  [world evaluation]
  (let [f (objects/fields (act/db world) evaluation)
        v #(get-in f [% :field/value])]
    (evaluated! world evaluation (v :evaluation/initiator) (v :evaluation/model) (v :evaluation/delegation)
                {:state :failed :outcome {:error "restart" :reason :restart :elapsed-ms 0}})))

(defn cancel!
  "The cancel action (decisions 2026-10-30): {:object <evaluation or principal id>}. On a
   Principal it means its current wake, the latest Evaluation it initiated that is in flight.
   The caller must resolve the named Object for :evaluate (or :write); a Message's sender resolves
   the Evaluation answering it (resolution step 7). Returns {:ok {:result {:evaluation :state
   :elapsed-ms} :object :seq}} with the seq of the cancelled event (state \"cancelling\" and no
   seq when the Model had not let go within 3 s), err conflict when the Evaluation is not running,
   err unresolved otherwise."
  [world p {:keys [object]}]
  (let [db (act/db world)
        s (objects/state db object)
        cls (:object/class s)
        target (case cls :evaluation object :principal (running-evaluation object) nil)]
    (cond
      (nil? s) act/unresolved
      (not (#{:evaluation :principal} cls)) (act/err :invalid "cancel applies to an Evaluation or a Principal")
      (not (or (resolve/resolve! world db p (act/id-name object) :evaluate)
               (resolve/resolve! world db p (act/id-name object) :write))) act/unresolved
      (nil? target) (act/err :conflict "no Evaluation is running for that Principal")
      :else
      (if-let [{:keys [cancel by ended started] claimed :reason} (get @running target)]
        (do (reset! claimed :cancel) ; a cancel beats a steer, whichever came first
            (reset! by p)
            (reset! cancel true) ; true = cancelled by a Principal; {:timeout reason} = failed (fail!)
            (let [seq (deref ended 3000 nil)]
              {:ok (cond-> {:result {:evaluation target :state (if seq "cancelled" "cancelling")
                                     :elapsed-ms (- (System/currentTimeMillis) started)}
                            :object target}
                     seq (assoc :seq seq))}))
        (let [state (get-in (objects/state db target) [:object/fields :evaluation/state :field/value])]
          (act/err :conflict (str "Evaluation is " (name (or state :unknown)) ", not running")))))))

(defn interrupt!
  "Cut a running Model call for `reason` on behalf of by (steering). True when this call claimed
   it, false when a cancel already had, nil when the Evaluation is not in the running set (yet)."
  [evaluation by reason]
  (when-let [{:keys [cancel] who :by claimed :reason} (get @running evaluation)]
    (when (compare-and-set! claimed nil reason)
      (reset! who by)
      (reset! cancel true)
      true)
    (= reason @claimed)))

(defn- withheld-tools
  "The tools this wake has no use for: `text_back` unless a text from outside woke it, and a
   subagent's `progress` and `ask` unless this Principal is one."
  [db principal message held]
  (let [field (fn [o k] (get-in (objects/fields db o) [k :field/value]))]
    (cond-> (vec held)
      (not (and message (field message :message/gateway))) (conj "text_back")
      (not (field principal :principal/ephemeral)) (into ["progress" "ask"]))))

(def ^:private lean-withheld
  "Tools a work wake past its first turn has no use for: a quarter of what is sent with every call."
  ["add_project" "projects" "bind" "grant" "history"])

(defn evaluate!
  "{:principal :working-set :model :route :policy :depth :message :wake :via :reasoning :known}. :reasoning
   is :on or :off as the caller resolved it (nil: the Model decides, see model/resolve-reasoning);
   :known lists Objects the serializer names in one line each without their Fields.
   :steers is the steering of this wake so far, rendered first (knowledge.world.loop.steer).
   :thinking-so-far and :retry-of retry a call cut at model/reasoning-max-chars: the reasoning
   already paid for goes back in, and the model answers from it.
   Returns at queued with the Evaluation id and a future of the outcome, or {:err ...}."
  [world {:keys [lean? hold-tools model-id changes wake-note max-tokens thinking-so-far last-turn? retry-of principal working-set model route policy depth message via sync? wake reasoning known rendered compressed steers ids] :as args}]
  (let [db (act/db world)
        model-state (objects/state db model)
        residency (get-in model-state [:object/fields :model/residency :field/value])]
    (cond
      (not= :model (:object/class model-state)) act/unresolved
      :else
      (if-let [{:keys [grant]} (resolve/resolve! world db principal (act/id-name model) :evaluate)]
        (if (= :external residency)
          (act/err :residency "no residency Grant rule admits an external Model yet")
          (let [evaluation (random-uuid)
                policy (or policy :normal)
                reasoning (model/resolve-reasoning (model/for-model model-state) reasoning)
                ;; a loop's Evaluation names the Message it answers and the delivered
                ;; event (the wake) it runs for, so a client groups a wake's Evaluations exactly
                fields (cond-> {:evaluation/initiator principal :evaluation/model model :evaluation/delegation grant
                                :evaluation/policy policy :evaluation/state :queued :evaluation/produced #{}
                                :evaluation/cost {} :evaluation/reasoning reasoning}
                         message (assoc :evaluation/message message)
                         wake (assoc :evaluation/wake wake)
                         retry-of (assoc :evaluation/retry-of retry-of))
                _ (act/append! world (log/pending :created evaluation principal {:class :evaluation :fields fields}
                                                  :delegation grant :via via))
                ;; a loop's turn acts, through the Model's own tools when it is a real one; a compression
                ;; (rendered) only writes. The last turn is offered no tool, so that it answers.
                adapter (models/for-call world model (model/for-model model-state) model-id)
                tools? (and message (nil? rendered) (= :real (:mode adapter)) (not (false? (get-in model-state [:object/fields :model/tools :field/value]))))
                {ser-ok :ok ser-err :err} (ser/serialize! world {:changes changes :tools? tools? :principal principal :working-set working-set :model model :ids ids
                                                                 :depth depth :route route :message message :known known :steers steers
                                                                 :thinking-so-far thinking-so-far
                                                                 :last-turn? last-turn? :wake-note wake-note :lean? lean?
                                                                 :rendered rendered :via evaluation :grant grant})]
            (if ser-err
              {:err ser-err}
              (let [ser-state (objects/state (act/db world) (:serialization ser-ok))
                    serialization (-> (into {} (map (fn [[k f]] [k (:field/value f)])) (:object/fields ser-state))
                                      ;; the record keeps a reference; only the call carries the bytes
                                      (assoc :serialization/bytes (image/expand world (ser/bytes-of world ser-state))))
                    _ (when (seq (:images ser-ok))
                        (field-set! world evaluation principal grant :evaluation/images [:list :map] (vec (:images ser-ok))))
                    _ (evaluated! world evaluation principal model grant {:state :queued :serialization (:serialization ser-ok)})
                    job {:evaluation evaluation :principal principal :model model :grant grant
                         :adapter (cond-> adapter
                                   tools? (assoc :tools? (if last-turn? :none true)
                                                 :withheld (withheld-tools (act/db world) principal message (when lean? lean-withheld))
                                                 :only hold-tools)
                                   max-tokens (assoc :max-tokens max-tokens))
                         :serialization serialization :reasoning reasoning
                         :dropped (:dropped ser-ok) :objects (:objects ser-ok) :compressed compressed
                         :standing-tokens (:standing-tokens ser-ok) :budget-tokens (:budget-tokens ser-ok)
                         :over-budget (:over-budget ser-ok)}
                    done (if sync? (doto (promise) (deliver (run-job! world job))) (future (run-job! world job)))]
                {:ok {:evaluation evaluation :serialization (:serialization ser-ok) :done done :ids (:ids ser-ok)}}))))
        act/unresolved))))
