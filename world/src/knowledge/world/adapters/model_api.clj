(ns knowledge.world.adapters.model-api
  "Model adapter (spec 5.6, 5.10, 11.5). An adapter is built from a Model's Fields by
   `for-model`: :real when the Model has model/base-url (OpenAI chat completions, bearer key
   read from the environment variable the Model's model/key-env names, never from the log),
   else :stub. The :stub Model is deterministic and honest (ground rule 7): it reads its
   Serialization and answers with the names of the Fields it was given and the hash it saw.
   Nothing here appends to the log; knowledge.world.evaluate owns the Evaluation events."
  (:require [knowledge.world.adapters.responses :as responses]
            [clojure.string :as str]
            [jsonista.core :as j]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.serialize.core :as ser])
  (:import [java.net URI]
           [java.net.http HttpClient HttpRequest HttpRequest$BodyPublishers HttpResponse$BodyHandlers]
           [java.time Duration]
           [java.util.stream Stream]))

(def default-timeout-s 600)
(def default-max-tokens 16384) ; reasoning counts against it: 4096 left GLM with no room to answer

(def default-reasoning-kwargs
  "What the request body carries per reasoning mode (decisions 2026-10-30 'reasoning is asked for').
   The :off form is what vLLM, SGLang and the exl3 server behind modelhost honour for GLM and Qwen
   thinking models; a Model whose server wants another shape sets model/reasoning-kwargs."
  {:off {:chat_template_kwargs {:enable_thinking false}}
   :on {}})

(def reasoning-modes #{:off :on :auto})

(defn- kw [v] (when v (if (keyword? v) v (keyword (str v)))))

(def default-idle-timeout-s 120) ; model/idle-timeout-s: the longest silence between two deltas
(def default-reasoning-max-chars 20000) ; model/reasoning-max-chars: more reasoning than this cuts the call; the loop retries once with reasoning off

(defn create
  "{:mode :stub|:real :url :key :model :timeout-s :idle-timeout-s :max-tokens :reasoning :reasoning-kwargs :delta-ms}.
   Real mode needs :url. :reasoning is the Model's mode (:off :on :auto, default :auto);
   :reasoning-kwargs replaces default-reasoning-kwargs per mode. delta-ms paces the stub's
   deltas (model/stub-delta-ms), so a test can cancel a stream mid-way. timeout-s is the whole
   call's deadline and idle-timeout-s the longest silence between deltas: a stalled
   stream fails with the reason instead of running forever."
  [{:keys [mode url key model timeout-s idle-timeout-s max-tokens reasoning reasoning-kwargs reasoning-max-chars delta-ms responses?] :or {mode :stub}}]
  {:mode (if (and (= mode :real) url) :real :stub) :url url :key key :model (or model "default")
   :timeout-s (long (or timeout-s default-timeout-s)) :max-tokens (long (or max-tokens default-max-tokens))
   :idle-ms (long (* 1000 (double (or idle-timeout-s default-idle-timeout-s))))
   :reasoning-max (long (or reasoning-max-chars default-reasoning-max-chars))
   :reasoning (or (reasoning-modes (kw reasoning)) :auto)
   ;; a configured mode replaces the default shape for that mode; an unmentioned mode keeps the default
   :reasoning-kwargs (merge default-reasoning-kwargs
                            (into {} (for [[k v] (when (map? reasoning-kwargs) reasoning-kwargs) :when (map? v)] [(kw k) v])))
   :delta-ms (long (or delta-ms 0))
   :responses? (boolean responses?)
   :recorded (atom [])})

(defn for-model
  "The adapter for a Model's state map (spec 11.5 item 2). model/timeout-s, model/max-tokens,
   model/reasoning and model/reasoning-kwargs are Fields on the Model (decisions 2026-10-30),
   read at evaluate time."
  [model-state]
  (let [f (fn [k] (get-in model-state [:object/fields k :field/value]))
        url (f :model/base-url)]
    (create {:mode (if url :real :stub) :url url :model (f :model/id)
             :key (some-> (f :model/key-env) System/getenv)
             ;; a plan-backed Model's token comes from models/for-call, never from here or a Field
             :responses? (= :chatgpt-plan (f :model/api))
             :timeout-s (f :model/timeout-s) :idle-timeout-s (f :model/idle-timeout-s) :max-tokens (f :model/max-tokens)
             :reasoning (f :model/reasoning) :reasoning-kwargs (f :model/reasoning-kwargs)
             :reasoning-max-chars (f :model/reasoning-max-chars)
             :delta-ms (f :model/stub-delta-ms)})))

(defn resolve-reasoning
  "The mode a request goes out with: what the caller resolved (:on or :off, from the Message, the
   Principal and the Model, see knowledge.world.loop.core/reasoning-for), else the Model's own
   mode when it is definite, else :off: a Model on :auto with nobody asking does not reason."
  [{:keys [reasoning]} requested]
  (or (#{:on :off} (kw requested)) (#{:on :off} reasoning) :off))

(defn- tool [nm description required properties]
  {:type "function"
   :function {:name nm :description description
              :parameters {:type "object" :properties properties :required required}}})

(def ^:private text {:type "string"})

(def op-tools
  "The wire ops a loop may use, as the Model's own tools (decisions 2026-10-31). A call comes back
   structured, so nothing has to be read out of prose: this model wrote ops seven ways as text, each
   of which reached a person as the answer at some point. Fenced blocks still parse."
  [(tool "read" (str "Read one thing, in one of two ways. A file or anything named: {name, range?}, as "
                     "{name: \"harness/README.md\", range: [10, 40]}, range being lines, so a `path:line` is one read away. "
                     "An Object: {object, field?, range?}. Leave out what you do not use.")
         []
         {:object text :name text :field text :range {:type "array" :items {:type "integer"}}})
   (tool "grep" (str "Find lines in your mounted files. `pattern` is literal text and a phrase matches however it is "
                     "spelled; set `regex` true for a regular expression.")
         ["pattern"]
         {:pattern text :path text :glob text :regex {:type "boolean"}})
   (tool "outline" "The headings and functions of a file, the folders of a directory, or with no name every mount you can reach." [] {:name text})
   (tool "projects" "Every project you can see: what it is, its last commit, its Principal's last work." [] {})
   (tool "add_project" "Make a directory someone named a project of yours: its own Principal, mount, shell, Route and Grant. Call it once per project, before trying to read the directory: nothing outside your mounts can be read until then." ["name" "path"]
         {:name text :path text :charter text})
   (tool "delegate" "Hand one Task to a temporary subagent. `task` must say everything it needs. It is retired when done, and its result wakes you." ["task"]
         {:task text :deadline-s {:type "integer"}})
   (tool "progress" "Record how your Task is going. It wakes nobody." ["text"] {:text text})
   (tool "ask" "A subagent's question to its parent, when it cannot go on." ["text"] {:text text})
   (tool "text_back" "Text the person whose text from outside you are answering. It goes to a person's approval first." ["text"] {:text text})
   (tool "edit" "Replace exact text in a file, once, and get the diff. `at` top or end inserts. `name` and `new` alone create a new file." ["name" "new"]
         {:name text :old text :new text :all {:type "boolean"} :at text})
   (tool "test" "Run this project's tests from nothing to results, with its own test command, in one step. Use it when asked to run the build or the tests." [] {})
   (tool "run" "Run a command in your shell Adapter. To read a file use `read` with a range, not sed or cat." ["cmd"] {:cmd text :adapter text :cwd text :timeout-s {:type "integer"}})
   (tool "act" "A named action from your Actions list; read {name: \"action/<name>\"} gives its args. Prefer a tool of its own where there is one." ["action"]
         {:object text :action text :args {:type "object"}})
   (tool "send" "A Message to another Principal. Never your reply to the asker: that is your final text." ["to" "route" "text"]
         {:to {:type "array" :items text} :route text :text text :in-reply-to text :images {:type "array" :items text}})
   (tool "set-field" "Write a Field on an Object." ["object" "field" "type"]
         {:object text :field text :type text :value {} :from {:type "object"}})
   (tool "resolve" "What a name denotes for you." ["name"] {:name text})
   (tool "history" (str "Events from the record. One Object: {object, limit: 20}. A conversation: {thread}. "
                        "The newest you can see: {last: 20}. Page on with {after: <seq>, limit: 50}.")
         []
         {:object text :thread text :last {:type "integer"} :after {:type "integer"} :limit {:type "integer"}})
   (tool "bind" "Give an Object a name." ["name" "object"] {:name text :object text})
   (tool "grant" "A Grant to another Principal." ["grantee" "scope" "ops"]
         {:grantee text :scope {} :ops {:type "array" :items text} :delegable {:type "boolean"}})])

(defn offered
  "The tools for this call: all of them less what the wake has no use for. A tool offered is a tool
   used: `text_back` on a person's plain hello raised a card for a gateway that was not there."
  [{:keys [withheld only]}]
  (vec (remove #(let [n (get-in % [:function :name])]
                  (or (contains? (set withheld) n) (and (seq only) (not (contains? (set only) n)))))
               op-tools)))

(defn request-body
  "The chat completions body for a messages array and a resolved reasoning mode (:on or :off):
   the model, the stream flags, max_tokens and the mode's kwargs merged in."
  [{:keys [model max-tokens reasoning-kwargs tools? only] :as adapter} messages mode]
  (merge (cond-> {:model model :messages messages :stream true
                  :stream_options {:include_usage true} :max_tokens max-tokens}
           ;; no tools at all on a wake's last turn: seen live, this endpoint calls one under tool_choice none
           (and tools? (not= :none tools?)) (assoc :tools (offered adapter) :tool_choice (if (seq only) "required" "auto")))
         (get reasoning-kwargs mode {})))

(defn- cancelled! [] (throw (ex-info "cancelled" {::cancelled true})))

(defn- check-cancel
  "The cancel atom aborts the call when it holds anything: true means cancelled by a Principal,
   {:timeout reason} means the deadline or the idle timeout passed."
  [cancel] (when (and cancel @cancel) (cancelled!)))

(defn- abort-outcome
  "The outcome for an aborted call: cancelled, or failed with the timeout's reason."
  [cancel]
  (let [v (when cancel @cancel)]
    (if (and (map? v) (:timeout v))
      (cond-> {:state :failed :error (:timeout v)} (:reason v) (assoc :reason (:reason v)))
      {:state :cancelled})))

(defn- timeout-reason [kind secs chars]
  (case kind
    :deadline (format "timeout after %d s, %d chars received (model/timeout-s)" (long secs) (long chars))
    :idle (format "no output for %d s, %d chars received (model/idle-timeout-s)" (long secs) (long chars))
    :reasoning (format "reasoning exceeded %d chars (model/reasoning-max-chars)" (long chars))))

(defn reasoning-guard
  "Wrap an on-thinking callback so that more than `max` reasoning chars flips `cancel` to a
   timeout-style reason and the call ends fast (two Evaluations once streamed 50 KB
   of reasoning for 600 s and never answered). Counts cumulatively; nil when there is no callback."
  [on-thinking cancel max]
  (when on-thinking
    (let [n (atom 0)]
      (fn [^String d]
        (when (and (> (swap! n + (count d)) (long max)) cancel (not @cancel))
          (reset! cancel {:timeout (timeout-reason :reasoning 0 max) :reason :reasoning-cap}))
        (on-thinking d)))))

(defn watchdog
  "Start a daemon thread that flips `cancel` to {:timeout reason} when the deadline (absolute ms)
   passes or when `last` (an atom of ms, updated on every delta) is older than idle-ms; `chars`
   (an atom) is reported in the reason. Returns a fn that stops it."
  [cancel started deadline idle-ms last chars]
  (let [running (atom true)
        t (doto (Thread. (fn []
                           (loop []
                             (when @running
                               (let [now (System/currentTimeMillis)]
                                 (cond
                                   (and cancel @cancel) nil
                                   (>= now deadline)
                                   (reset! cancel {:timeout (timeout-reason :deadline (quot (- now started) 1000) @chars)})
                                   (and (pos? (long idle-ms)) (> (- now @last) idle-ms))
                                   (reset! cancel {:timeout (timeout-reason :idle (quot (- now @last) 1000) @chars)})
                                   :else (do (Thread/sleep 250) (recur)))))))
                         "ke-model-watchdog")
            (.setDaemon true))]
    (.start t)
    (fn [] (reset! running false))))

(def ^:private think-block #"(?s)<think>(.*?)</think>\s*")

(defn split-thinking
  "[reply thinking]: the endpoint's reasoning_content (or reasoning), else <think>...</think>
   blocks in the content, removed from the reply; nil when there is none."
  [content reasoning]
  (let [content (str content)
        in-text (map second (re-seq think-block content))
        reply (str/trim (str/replace content think-block ""))
        thinking (str/join "\n\n" (remove str/blank? (concat (when (string? reasoning) [reasoning]) in-text)))]
    [reply (when-not (str/blank? thinking) thinking)]))

(defn serialization
  "A Serialization-like map (spec 4.5) for a working set rendered as one prompt string."
  [model-id working-set ^String text]
  (let [bs (.getBytes text "UTF-8")]
    {:serialization/working-set (set working-set) :serialization/model model-id
     :serialization/depth (zipmap working-set (repeat 0)) :serialization/format :openai-chat
     :serialization/bytes text :serialization/size {:bytes (alength bs)}
     :serialization/hash (blob/sha256-hex bs) :serialization/as-of {}}))

(defn- messages-of
  "The chat messages array in the bytes, or one user message of the raw text."
  [{:keys [serialization/format serialization/bytes]}]
  (or (when (= format :openai-chat)
        (try (let [v (j/read-value bytes j/keyword-keys-object-mapper)] (when (sequential? v) v)) (catch Exception _ nil)))
      [{:role "user" :content (str bytes)}]))

(defn- content-text
  "A message's content as text: a string, or the text of its parts."
  [content]
  (if (sequential? content)
    (str/join "\n" (keep :text content))
    (str content)))

(defn- images-in [messages]
  (count (filter #(= "image_url" (:type %)) (mapcat #(when (sequential? (:content %)) (:content %)) messages))))

(defn- parse-sse-chunk
  "One `data: {...}` line -> the chunk map, or nil for blanks, comments and [DONE]."
  [^String line]
  (let [line (str/trim line)]
    (when (and (str/starts-with? line "data:") (not= "data: [DONE]" line) (not= "data:[DONE]" line))
      (try (j/read-value (str/trim (subs line 5)) j/keyword-keys-object-mapper) (catch Exception _ nil)))))

(defn- merge-tool-call
  "Accumulate a streamed tool_call delta (by index) into calls, a vector of {:name :arguments}."
  [calls {:keys [index function]}]
  (let [i (or index 0)
        calls (if (< i (count calls)) calls (into calls (repeat (- (inc i) (count calls)) {:name nil :arguments ""})))]
    (cond-> calls
      (:name function) (assoc-in [i :name] (:name function))
      (:arguments function) (update-in [i :arguments] str (:arguments function)))))

(defn route-delta
  "Split one content delta into [thinking-part content-part] given whether the stream was inside
   an unclosed <think> block before it; returns [thinking content in-think-after]. Tags are consumed."
  [in-think ^String delta]
  (reduce (fn [[t c in] tok]
            (case tok
              "<think>" [t c true]
              "</think>" [t c false]
              (if in [(str t tok) c in] [t (str c tok) in])))
          ["" "" in-think]
          (re-seq #"<think>|</think>|[^<]+|<" delta)))

(defn consume-stream
  "Fold an OpenAI chat completion event stream (a seq of SSE lines) into one outcome, calling
   on-delta with each content delta and on-thinking with each reasoning delta (reasoning_content,
   reasoning, or the inside of <think> blocks arriving in the content) as they arrive. Returns the
   same shape post-chat returned before streaming existed: {:state :done :text :usage :cost
   :finish? :thinking? :tool-calls?}. Pure over the lines; the caller owns the socket."
  ([lines on-delta] (consume-stream lines on-delta nil))
  ([lines on-delta on-thinking]
   (let [acc (reduce (fn [acc line]
                       (if-let [chunk (parse-sse-chunk line)]
                         (let [choice (get-in chunk [:choices 0])
                               delta (:delta choice)
                               content (:content delta)
                               reasoning (or (:reasoning_content delta) (:reasoning delta))
                               [t c in] (if (string? content) (route-delta (:in-think acc) content) ["" "" (:in-think acc)])]
                           (when (and (seq t) on-thinking) (on-thinking t))
                           (when (and (seq c) on-delta) (on-delta c))
                           (when (and (string? reasoning) (seq reasoning) on-thinking) (on-thinking reasoning))
                           (cond-> (assoc acc :in-think in)
                             (string? content) (update :content str content)
                             (string? reasoning) (update :reasoning str reasoning)
                             (:tool_calls delta) (update :tool-calls #(reduce merge-tool-call (or % []) (:tool_calls delta)))
                             (:finish_reason choice) (assoc :finish (str (:finish_reason choice)))
                             (:usage chunk) (assoc :usage (:usage chunk))
                             (:model chunk) (assoc :model (:model chunk))))
                         acc))
                     {:content "" :reasoning "" :in-think false}
                     lines)
         in (get-in acc [:usage :prompt_tokens] 0) out (get-in acc [:usage :completion_tokens] 0)
         [reply thinking] (split-thinking (:content acc) (when (seq (:reasoning acc)) (:reasoning acc)))
         tool-calls (vec (filter :name (:tool-calls acc)))]
     (cond-> {:state :done :text reply
              :usage {:prompt-tokens in :completion-tokens out :model (:model acc)}
              :cost {:tokens-in (double in) :tokens-out (double out)}}
       (:finish acc) (assoc :finish (:finish acc))
       thinking (assoc :thinking thinking)
       (seq tool-calls) (assoc :tool-calls tool-calls)))))

(defn- parse-json-completion
  "A non-streamed chat completion body -> outcome (an endpoint that ignores stream: true)."
  [parsed]
  (let [in (get-in parsed [:usage :prompt_tokens] 0) out (get-in parsed [:usage :completion_tokens] 0)
        msg (get-in parsed [:choices 0 :message])
        finish (get-in parsed [:choices 0 :finish_reason])
        tool-calls (vec (for [{:keys [function]} (:tool_calls msg) :when (:name function)]
                          {:name (:name function) :arguments (:arguments function)}))
        [reply thinking] (split-thinking (:content msg) (or (:reasoning_content msg) (:reasoning msg)))]
    (cond-> {:state :done :text reply
             :usage {:prompt-tokens in :completion-tokens out :model (:model parsed)}
             :cost {:tokens-in (double in) :tokens-out (double out)}}
      finish (assoc :finish (str finish))
      thinking (assoc :thinking thinking)
      (seq tool-calls) (assoc :tool-calls tool-calls))))

(defn- post-chat
  "POST /chat/completions with the body request-body built (stream: true, decisions 2026-10-30
   streaming; the reasoning kwargs of the resolved mode). The response body
   is read line by line; content deltas reach on-delta and reasoning deltas on-thinking as they
   arrive; the outcome is the fold of
   the whole stream, usage from the final chunk (stream_options include_usage). An endpoint that
   answers JSON instead of an event stream is parsed as before."
  [{:keys [url key timeout-s idle-ms reasoning-max responses?] :as adapter} body on-delta on-thinking cancel deadline]
  (let [body (j/write-value-as-string body)
        started (System/currentTimeMillis)
        deadline (long (or deadline (+ started (* 1000 (long timeout-s)))))
        last (atom started) chars (atom 0)
        cancel (or cancel (atom nil))
        touch (fn [f] (when f (fn [^String d] (reset! last (System/currentTimeMillis)) (swap! chars + (count d)) (f d))))
        on-delta (touch on-delta)
        on-thinking (touch (reasoning-guard on-thinking cancel (or reasoning-max default-reasoning-max-chars)))
        stop-watchdog! (watchdog cancel started deadline idle-ms last chars)
        req (-> (HttpRequest/newBuilder (URI. (str url (if responses? "/responses" "/chat/completions"))))
                (.timeout (Duration/ofSeconds (long timeout-s)))
                (.header "content-type" "application/json")
                (.header "accept" "text/event-stream, application/json")
                (cond-> key (.header "authorization" (str "Bearer " key)))
                (.POST (HttpRequest$BodyPublishers/ofString body))
                .build)
        ;; cancel (an atom, decisions 2026-10-30): flipping it aborts the request before headers
        ;; (future cancel) or closes the body stream mid-way, which cancels the subscription
        fut (.sendAsync (HttpClient/newHttpClient) req (HttpResponse$BodyHandlers/ofLines))
        stream-ref (atom nil)
        abort! (fn [] (.cancel fut true) (when-let [^Stream s @stream-ref] (try (.close s) (catch Throwable _ nil))))
        _ (add-watch cancel ::abort (fn [_ _ _ v] (when v (abort!))))]
    (try
      (check-cancel cancel)
      (let [resp (.get fut)
            content-type (str (.orElse (.firstValue (.headers resp) "content-type") ""))]
        (with-open [^Stream lines (.body resp)]
          (reset! stream-ref lines)
          (check-cancel cancel)
          (let [line-seq (map (fn [l] (check-cancel cancel) (reset! last (System/currentTimeMillis)) l) (iterator-seq (.iterator lines)))]
            (if (not= 200 (.statusCode resp))
              (let [text (str/join "\n" line-seq)]
                {:state :failed :error (if responses?
                                         (responses/error-of-body (.statusCode resp) text)
                                         (str "HTTP " (.statusCode resp) " " (subs text 0 (min 300 (count text)))))})
              ;; a stream is a stream by its bytes too: a live endpoint can label it otherwise
              (if (or (str/includes? content-type "text/event-stream")
                      (some->> (first line-seq) (re-find #"^(event|data):")))
                (if responses?
                  (responses/consume-events line-seq on-delta on-thinking)
                  (consume-stream line-seq on-delta on-thinking))
                (let [text (str/join "\n" line-seq)
                      parsed (try (j/read-value text j/keyword-keys-object-mapper) (catch Exception _ nil))]
                  (if (map? parsed)
                    (parse-json-completion parsed)
                    {:state :failed :error (str "unparseable completion: " (subs text 0 (min 300 (count text))))})))))))
      (finally (stop-watchdog!) (when cancel (remove-watch cancel ::abort))))))

(def ^:private ops-block #"(?s)```ops[ \t]*\n.*?```")
(def ^:private relay-block #"(?s)```ops-relay[ \t]*\n(.*?)```")   ; the stub's "once everyone answered" block
(def ^:private thread-header #"## thread ([0-9a-f]{2,36})")

(defn- stub-outcome
  "The stub reads its context: the Field names after the serializer's field markers. Asked to act
   (the user turn carries a fenced `ops` block) it repeats that block once, and answers in prose
   as soon as its context shows an evaluation/results Field: the honest minimum of a model that
   acts (spec 9.8 steps 5 and 6) without inventing anything."
  [{:keys [serialization/hash] :as s}]
  (let [messages (messages-of s)
        text (str/join "\n" (map (comp content-text :content) messages))
        fields (map second (re-seq (re-pattern (str "(?m)^" ser/field-marker " (\\S+)")) text))
        user-text (content-text (:content (last (filter #(= "user" (:role %)) messages))))
        seen (images-in messages)
        ;; the user turn may carry several ops blocks: block k is emitted on iteration k (the
        ;; Evaluations already in view count the iterations), so a test can shape a long wake
        blocks (vec (re-seq ops-block user-text))
        asked (nth blocks (count (distinct (map second (re-seq #"(?m)^(?:## |- )evaluation ([0-9a-f]{2,36})" text)))) nil)
        acted? (some #(= "evaluation/results" %) fields)
        ;; a thread root may carry a block tagged ops-relay, what to do once every
        ;; send of this Principal in the thread has been answered; the stub emits it in the wake
        ;; where the thread block shows nothing awaiting, with <thread> replaced by the thread id,
        ;; the way a model that reads its thread would relay
        relay (when (and (nil? asked) (not acted?) (not (re-find #"awaiting reply" text)))
                (when-let [[_ body] (re-find relay-block text)]
                  (let [thread-id (second (re-find thread-header text))]
                    (str "```ops\n" (cond-> body thread-id (str/replace "<thread>" thread-id)) "\n```"))))
        ;; asked to think aloud, the stub answers like an endpoint that returns <think> blocks in content
        thinking (when (and (str/includes? user-text "think aloud") (not (str/includes? text ser/reasoning-so-far-heading))) (str "stub thinking: " (count fields) " Fields in view."))
        ;; the stub counts tokens as chars/4, so cost books have something honest to add up
        prompt-tokens (long (quot (count text) 4))
        base {:state :done :usage {:prompt-tokens prompt-tokens :completion-tokens 0} :cost {:tokens-in (double prompt-tokens) :tokens-out 0.0}}
        text (cond
               asked (str "stub model: acting as asked.\n" asked)
               relay (str "stub model: everyone answered; relaying as the thread asked.\n" relay)
               :else
               (str "stub model: I read " (count fields) " Fields"
                    (when (seq fields) (str ": " (str/join ", " (distinct fields))))
                    (when acted? " (I acted; the results are in evaluation/results)")
                    (when (pos? seen) (str " I see " seen " image" (when (> seen 1) "s") "."))
                    ". Serialization " hash "."))
        [reply thinking] (split-thinking (str (when thinking (str "<think>" thinking "</think>\n")) text) nil)]
    (let [completion-tokens (long (quot (count (str reply)) 4))]
      (cond-> (-> base (assoc :text reply)
                  (assoc-in [:usage :completion-tokens] completion-tokens)
                  (assoc-in [:cost :tokens-out] (double completion-tokens)))
        thinking (assoc :thinking thinking)))))

(defn stub-deltas
  "The stub streams like an endpoint (ground rule 7): its reply text in pieces of at most n chars."
  ([text] (stub-deltas text 24))
  ([^String text n] (if (str/blank? text) [] (map #(apply str %) (partition-all n text)))))

(defn evaluate!
  "Run one Serialization through the Model. opts {:on-delta (fn [text]) :on-thinking (fn [text])
   :reasoning :on|:off :cancel atom}: the callbacks receive content and reasoning deltas as the
   Model produces them (the stub produces them too: its thinking first, then its reply, paced by
   delta-ms); :reasoning is the mode the caller resolved (resolve-reasoning fills it in from the
   Model when absent); flipping :cancel to true aborts the Model call and the outcome is
   {:state :cancelled}. Every call records the request body it built, so a test can see the kwargs
   a mode put on the wire, for the stub too.
   Otherwise returns {:state :text :usage :cost :reasoning} or {:state :failed :error :reasoning}."
  ([adapter ser] (evaluate! adapter ser {}))
  ([{:keys [mode recorded delta-ms timeout-s idle-ms reasoning-max] :as adapter} ser {:keys [on-delta on-thinking reasoning cancel deadline]}]
   (let [resolved (resolve-reasoning adapter reasoning)
         body (if (:responses? adapter)
                (responses/request-body adapter (offered adapter) (messages-of ser) resolved)
                (request-body adapter (messages-of ser) resolved))
         cancel (or cancel (atom nil))
         on-thinking (reasoning-guard on-thinking cancel (or reasoning-max default-reasoning-max-chars))
         started (System/currentTimeMillis)
         deadline (long (or deadline (+ started (* 1000 (long timeout-s)))))
         ;; the stub keeps the same clocks as a real call: a paced delta past the deadline or the
         ;; idle timeout fails with the reason, so the timeouts are tested without a Model
         last (atom started) chars (atom 0)
         pace (fn [] (when (pos? (long (or delta-ms 0))) (Thread/sleep (long delta-ms)))
                (let [now (System/currentTimeMillis)]
                  (when-not (and cancel @cancel)
                    (cond (>= now deadline) (reset! cancel {:timeout (timeout-reason :deadline (quot (- now started) 1000) @chars)})
                          (and (pos? (long idle-ms)) (> (- now @last) idle-ms)) (reset! cancel {:timeout (timeout-reason :idle (quot (- now @last) 1000) @chars)}))))
                (check-cancel cancel))
         emit (fn [f d] (reset! last (System/currentTimeMillis)) (swap! chars + (count d)) (f d))
         outcome (try (case mode
                        :stub (let [o (stub-outcome ser)]
                                (check-cancel cancel)
                                (when (and on-thinking (:thinking o)) (doseq [d (stub-deltas (:thinking o))] (pace) (emit on-thinking d)))
                                (when on-delta (doseq [d (stub-deltas (:text o))] (pace) (emit on-delta d)))
                                (check-cancel cancel)
                                o)
                        :real (let [o (post-chat adapter body on-delta on-thinking cancel deadline)]
                                ;; a stream the watchdog closed may still end cleanly: the timeout wins
                                (if (and cancel @cancel) (abort-outcome cancel) o)))
                      (catch Throwable e
                        (if (or (::cancelled (ex-data e)) (and cancel @cancel))
                          (abort-outcome cancel)
                          {:state :failed :error (str (.getName (class e)) ": " (ex-message e))})))
         outcome (assoc outcome :reasoning resolved)]
     (swap! recorded conj {:serialization ser :outcome outcome :request body :reasoning resolved})
     outcome)))

(defn recorded [adapter] @(:recorded adapter))
