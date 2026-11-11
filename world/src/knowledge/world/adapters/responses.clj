(ns knowledge.world.adapters.responses
  "OpenAI's Responses API, the one route ChatGPT plan usage allows
   (developers.openai.com/siwc/token-sharing-open-source/preview-limitations): store false, stream
   true, the whole history in `input`, system text in `instructions`, function tools in a namespace,
   and none of max_output_tokens, temperature, metadata and the rest. Pure over messages and lines:
   the socket stays in model_api."
  (:require [clojure.string :as str]
            [jsonista.core :as j]))

(defn- instructions-of [messages]
  (str/join "\n\n" (keep (fn [{:keys [role content]}] (when (= "system" role) content)) messages)))

(defn- items-of
  "One chat message as Responses input items."
  [{:keys [role content tool_calls tool_call_id]}]
  (case role
    "system" []
    "tool" [{:type "function_call_output" :call_id tool_call_id :output (str content)}]
    (concat
     (when (seq content) [{:role role :content content}])
     (for [{:keys [id function]} tool_calls]
       {:type "function_call" :call_id id :name (:name function) :arguments (:arguments function)}))))

(defn tools
  "The chat tools as one namespace, which is how this route takes function tools."
  [chat-tools]
  [{:type "namespace" :name "orbis" :description "Acting in this world."
    :tools (mapv (fn [{:keys [function]}]
                   ;; strict defaults on here, and strict makes every property required: the model then
                   ;; fills what it does not mean with "" (seen live: object "" on a file read, four times)
                   {:type "function" :name (:name function) :description (:description function)
                    :strict false :parameters (:parameters function)})
                 chat-tools)}])

(defn request-body
  "The Responses body for a chat messages array and a resolved reasoning mode."
  [{:keys [model tools? only]} chat-tools messages mode]
  (cond-> {:model model :store false :stream true
           :input (vec (mapcat items-of messages))}
    (seq (instructions-of messages)) (assoc :instructions (instructions-of messages))
    tools? (assoc :tools (tools chat-tools))
    (and tools? (seq only)) (assoc :tool_choice "required")
    ;; these Models reason whatever is asked: the mode sets the effort. What comes back is only a summary,
    ;; and only when it is asked for, so it is asked for every time and SHOW has something to show
    (= :on mode) (assoc :reasoning {:effort "medium" :summary "auto"})
    (not= :on mode) (assoc :reasoning {:summary "auto"})))

(defn- error-line
  "An error object or a `{\"detail\": ...}` body as one line, its code first: the code decides recovery."
  [{:keys [code message detail]}]
  (str/join ": " (remove nil? [code (or message detail)])))

(defn error-of-body
  "A non-200 body as one line. Admission errors have no standard shape, so the text stays diagnostic."
  [status ^String body]
  (let [parsed (try (j/read-value body j/keyword-keys-object-mapper) (catch Exception _ nil))
        said (when (map? parsed) (error-line (or (when (map? (:error parsed)) (:error parsed)) parsed)))]
    (str "HTTP " status " " (if (seq said) said (subs body 0 (min 300 (count body)))))))

(defn- data-of [^String line]
  (let [line (str/trim line)]
    (when (str/starts-with? line "data:")
      (try (j/read-value (str/trim (subs line 5)) j/keyword-keys-object-mapper) (catch Exception _ nil)))))

(defn consume-events
  "Fold a Responses event stream into the outcome model_api returns for a chat stream. Success is
   response.completed and nothing else: a limit can arrive as response.failed after text has begun."
  [lines on-delta on-thinking]
  (let [acc (reduce
             (fn [acc line]
               (if-let [{:keys [type delta item response] :as e} (data-of line)]
                 (case type
                   "response.output_text.delta" (do (when (and on-delta (seq delta)) (on-delta delta))
                                                    (update acc :text str delta))
                   ("response.reasoning_text.delta" "response.reasoning_summary_text.delta")
                   (do (when (and on-thinking (seq delta)) (on-thinking delta))
                       (update acc :thinking str delta))
                   "response.output_item.done"
                   (if (= "function_call" (:type item))
                     (update acc :tool-calls conj {:name (:name item) :arguments (:arguments item)})
                     acc)
                   "response.completed" (assoc acc :completed response)
                   "response.incomplete" (assoc acc :incomplete (or (get-in response [:incomplete_details :reason]) "incomplete"))
                   "response.failed" (assoc acc :error (or (error-line (:error response)) "response failed"))
                   "error" (assoc acc :error (or (error-line (or (:error e) e)) "stream error"))
                   acc)
                 acc))
             {:text "" :thinking "" :tool-calls []}
             lines)
        usage (get-in acc [:completed :usage])
        in (long (or (:input_tokens usage) 0)) out (long (or (:output_tokens usage) 0))]
    (cond
      (:error acc) {:state :failed :error (:error acc)}
      (:incomplete acc) {:state :failed :error (str "response incomplete: " (:incomplete acc))}
      (nil? (:completed acc)) {:state :failed :error "the stream ended without response.completed"}
      :else (cond-> {:state :done :text (:text acc) :finish "stop"
                     :usage {:prompt-tokens in :completion-tokens out :model (get-in acc [:completed :model])}
                     :cost {:tokens-in (double in) :tokens-out (double out)}}
              (seq (:thinking acc)) (assoc :thinking (:thinking acc))
              (seq (:tool-calls acc)) (assoc :tool-calls (:tool-calls acc))))))
