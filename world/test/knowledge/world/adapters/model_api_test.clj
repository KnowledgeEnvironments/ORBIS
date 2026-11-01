(ns knowledge.world.adapters.model-api-test
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.adapters.model-api :as model]))

(deftest stub-records-a-serialization-and-an-outcome
  (let [a (model/create {:mode :stub})
        model (random-uuid) o (random-uuid)
        ser (model/serialization model [o] "What do you know?")
        out (model/evaluate! a ser)]
    (is (= :done (:state out)))
    (is (string? (:text out)))
    (is (= 1 (count (model/recorded a))))
    (is (= ser (:serialization (first (model/recorded a)))))
    (is (= 64 (count (:serialization/hash ser))))
    (is (= :stub (:mode (model/create {:mode :real})) ) "real mode without a url falls back to stub")))

(deftest consume-stream-folds-sse-lines-and-streams-content
  ;; the shape modelhost (and any OpenAI-compatible endpoint) sends: role, reasoning deltas, content
  ;; deltas, a final chunk with finish_reason and usage, then [DONE]
  (let [chunk (fn [delta & [finish usage]]
                (str "data: " (jsonista.core/write-value-as-string
                                (cond-> {:id "c" :object "chat.completion.chunk" :model "m"
                                         :choices [{:index 0 :delta delta :finish_reason finish}]}
                                  usage (assoc :usage usage)))))
        lines [(chunk {:role "assistant"}) ""
               (chunk {:reasoning_content "think "}) (chunk {:reasoning_content "hard"}) ""
               (chunk {:content "Hello"}) ": comment line" (chunk {:content ", "}) (chunk {:content "world"})
               (chunk {} "stop" {:prompt_tokens 18 :completion_tokens 7}) "data: [DONE]"]
        seen (atom [])
        out (model/consume-stream lines #(swap! seen conj %))]
    (is (= ["Hello" ", " "world"] @seen) "content deltas reach on-delta in order; reasoning does not")
    (is (= :done (:state out)))
    (is (= "Hello, world" (:text out)))
    (is (= "think hard" (:thinking out)))
    (is (= "stop" (:finish out)))
    (is (= {:prompt-tokens 18 :completion-tokens 7 :model "m"} (:usage out)))
    (is (= {:tokens-in 18.0 :tokens-out 7.0} (:cost out)))
    (testing "<think> blocks inside streamed content are split like a non-streamed reply"
      (let [o (model/consume-stream [(chunk {:content "<think>t</think>"}) (chunk {:content "a"}) (chunk {} "stop")] nil)]
        (is (= "a" (:text o)))
        (is (= "t" (:thinking o)))))
    (testing "tool_calls arrive in pieces by index"
      (let [o (model/consume-stream [(chunk {:tool_calls [{:index 0 :function {:name "read" :arguments "{\"ob"}}]})
                                     (chunk {:tool_calls [{:index 0 :function {:arguments "ject\": 1}"}}]})
                                     (chunk {} "tool_calls")] nil)]
        (is (= [{:name "read" :arguments "{\"object\": 1}"}] (:tool-calls o)))))))

(deftest the-stub-streams-its-reply
  (let [a (model/create {:mode :stub})
        ser (model/serialization (random-uuid) [] "What do you know?")
        seen (atom [])
        out (model/evaluate! a ser {:on-delta #(swap! seen conj %)})]
    (is (= (:text out) (apply str @seen)) "the deltas concatenate to the reply")
    (is (> (count @seen) 1))
    (is (= [] (model/stub-deltas "")))))

(deftest consume-stream-forwards-reasoning-deltas
  ;; decisions 2026-10-30: reasoning streams too, so the 25 to 50 s a model spends
  ;; thinking before its first content delta is visible as it happens
  (let [chunk (fn [delta & [finish]]
                (str "data: " (jsonista.core/write-value-as-string
                                {:choices [{:index 0 :delta delta :finish_reason finish}]})))
        content (atom []) thinking (atom [])
        out (model/consume-stream [(chunk {:reasoning_content "think "}) (chunk {:reasoning_content "hard"})
                                   (chunk {:content "Hello"}) (chunk {:content "!"}) (chunk {} "stop")]
                                  #(swap! content conj %) #(swap! thinking conj %))]
    (is (= ["think " "hard"] @thinking) "reasoning deltas reach on-thinking in order")
    (is (= ["Hello" "!"] @content))
    (is (= "think hard" (:thinking out)))
    (is (= "Hello!" (:text out)))
    (testing "<think> blocks arriving in the content are routed to on-thinking, tags consumed, across chunk boundaries"
      (reset! content []) (reset! thinking [])
      (let [o (model/consume-stream [(chunk {:content "<think>t"}) (chunk {:content "x</think>a"}) (chunk {:content "b"}) (chunk {} "stop")]
                                    #(swap! content conj %) #(swap! thinking conj %))]
        (is (= ["t" "x"] @thinking))
        (is (= ["a" "b"] @content))
        (is (= "ab" (:text o)))
        (is (= "tx" (:thinking o)))))
    (testing "route-delta is pure"
      (is (= ["" "a" false] (model/route-delta false "a")))
      (is (= ["t" "" true] (model/route-delta false "<think>t")))
      (is (= ["x" "a" false] (model/route-delta true "x</think>a")))
      (is (= ["" "<b" false] (model/route-delta false "<b")) "a lone < is content"))
    (testing "the two-arity form still works with no on-thinking"
      (is (= "a" (:text (model/consume-stream [(chunk {:reasoning_content "r"}) (chunk {:content "a"}) (chunk {} "stop")] nil)))))))

(deftest the-stub-streams-its-thinking-first
  (let [a (model/create {:mode :stub})
        ser (model/serialization (random-uuid) [] "think aloud, then answer")
        seen (atom [])
        out (model/evaluate! a ser {:on-delta #(swap! seen conj [:c %]) :on-thinking #(swap! seen conj [:t %])})]
    (is (string? (:thinking out)))
    (is (= (:thinking out) (apply str (map second (filter #(= :t (first %)) @seen)))))
    (is (= (:text out) (apply str (map second (filter #(= :c (first %)) @seen)))))
    (is (= :t (first (first @seen))) "thinking comes first")
    (is (< (.lastIndexOf ^java.util.List (mapv first @seen) :t) (.indexOf ^java.util.List (mapv first @seen) :c)) "all thinking before any content")))

(deftest reasoning-is-a-mode-with-request-kwargs
  ;; decisions 2026-10-30 'reasoning is asked for': the body a mode puts on the wire is data on the
  ;; Model (model/reasoning-kwargs over the defaults), and every call records the body it built
  (let [a (model/create {:mode :stub})
        msgs [{:role "user" :content "hi"}]]
    (is (= :auto (:reasoning a)) "a Model without a mode is :auto")
    (is (= {:chat_template_kwargs {:enable_thinking false}} (select-keys (model/request-body a msgs :off) [:chat_template_kwargs]))
        "off carries the kwargs that switch a thinking model's reasoning off")
    (is (nil? (:chat_template_kwargs (model/request-body a msgs :on))) "on sends the bare request")
    (is (= [:model :messages :stream :stream_options :max_tokens] (keys (model/request-body a msgs :on))))
    (testing "resolution at the adapter: the caller's definite mode wins, then the Model's, else off"
      (is (= :on (model/resolve-reasoning a :on)))
      (is (= :off (model/resolve-reasoning a nil)) "auto with nobody asking does not reason")
      (is (= :on (model/resolve-reasoning (model/create {:mode :stub :reasoning :on}) nil)))
      (is (= :off (model/resolve-reasoning (model/create {:mode :stub :reasoning :on}) :off)) "a Message's off beats the Model's on"))
    (testing "model/reasoning-kwargs replaces the shape per mode, for a server that wants another one"
      (let [b (model/create {:mode :stub :reasoning-kwargs {:off {:thinking {:type "disabled"}} "on" {:thinking {:type "enabled"}}}})]
        (is (= {:type "disabled"} (:thinking (model/request-body b msgs :off))))
        (is (= {:type "enabled"} (:thinking (model/request-body b msgs :on))))
        (is (nil? (:chat_template_kwargs (model/request-body b msgs :off))) "the default off shape is replaced, not kept")))
    (testing "for-model reads the Fields"
      (let [st {:object/fields {:model/reasoning {:field/value :off} :model/id {:field/value "m"}
                                :model/reasoning-kwargs {:field/value {:off {:x 1}}}}}
            e (model/for-model st)]
        (is (= :off (:reasoning e)))
        (is (= {:x 1} (get-in e [:reasoning-kwargs :off])))))
    (testing "the stub records the request body and the resolved mode like the real adapter would"
      (let [ser (model/serialization (random-uuid) [] "What do you know?")]
        (model/evaluate! a ser {:reasoning :on})
        (model/evaluate! a ser)
        (let [[on off] (model/recorded a)]
          (is (= :on (:reasoning on)))
          (is (nil? (get-in on [:request :chat_template_kwargs])))
          (is (= :off (:reasoning off)))
          (is (= false (get-in off [:request :chat_template_kwargs :enable_thinking])))
          (is (= :off (get-in off [:outcome :reasoning]))))))))
