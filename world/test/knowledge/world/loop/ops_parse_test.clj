(ns knowledge.world.loop.ops-parse-test
  "The loop's ops parser reads fenced ops blocks and, since GLM's literal
   <tool_call>ops ... </tool_call> block (seen live: the model wrote its ops that way and they were
   taken for prose), closed by the tag or by the end of the text."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.loop.ops :as ops]))

(def line "{\"op\": \"read\", \"args\": {\"object\": \"6acd6a8f-bab6-4e9f-8682-4a716dd3a363\"}}")

(deftest a-tool-call-tagged-ops-block-parses-like-a-fence
  (testing "closed by the tag"
    (let [{:keys [ops reply]} (ops/parse {:text (str "I will look.<tool_call>ops\n" line "\n" line "\n</tool_call>\nThen answer.")})]
      (is (= 2 (count ops)))
      (is (every? #(= :read (:op %)) ops) (pr-str ops))
      (is (= "I will look.\nThen answer." reply))))
  (testing "closed by the end of the text"
    (let [{:keys [ops reply]} (ops/parse {:text (str "draft<tool_call>ops\n" line)})]
      (is (= 1 (count ops)))
      (is (= :read (:op (first ops))))
      (is (= "draft" reply))))
  (testing "a fence and a tag together, fence first"
    (let [{:keys [ops]} (ops/parse {:text (str "```ops\n" line "\n```\n<tool_call>ops\n" line "\n</tool_call>")})]
      (is (= 2 (count ops)))))
  (testing "a plain tool_call without the ops word is left alone"
    (let [{:keys [ops reply]} (ops/parse {:text "<tool_call>something\nnot ops\n</tool_call>"})]
      (is (empty? ops))
      (is (= "<tool_call>something\nnot ops\n</tool_call>" reply)))))

(def act-json "{\"action\": \"task.run\", \"args\": {\"adapter\": \"shell.local\", \"cmd\": \"ls\"}}")

(deftest the-two-framings-seen-on-2026-10-30
  (testing "an opener glued to prose with no closer, JSON lines to the end (seq 19795)"
    (let [{:keys [ops reply malformed-fences]} (ops/parse {:text (str "I will read it before acting.```ops\n" line "\n" line)})]
      (is (= 2 (count ops)) (pr-str ops))
      (is (every? #(= :read (:op %)) ops) (pr-str ops))
      (is (= "I will read it before acting." reply))
      (is (= 1 malformed-fences) "counted, so the slip stays visible")))
  (testing "an opener glued to prose with nothing parseable after it stays text"
    (let [{:keys [ops reply]} (ops/parse {:text "see below.```ops\nnothing here"})]
      (is (empty? ops))
      (is (str/includes? reply "nothing here"))))
  (testing "<tool_call>act<tool_call>{...} with the action in the body (seq 16005)"
    (let [{:keys [ops reply]} (ops/parse {:text (str "Running the check.<tool_call>act<tool_call>" act-json "</tool_call>")})]
      (is (= 1 (count ops)) (pr-str ops))
      (is (= :act (:op (first ops))) (pr-str ops))
      (is (= "task.run" (name (get-in (first ops) [:args :action]))) (pr-str ops))
      (is (= "Running the check." reply))))
  (testing "<tool_call>task.run<tool_call>{...}: a dotted action name as the tag is the same request"
    (let [{:keys [ops]} (ops/parse {:text "<tool_call>task.run<tool_call>{\"adapter\": \"shell.local\", \"cmd\": \"ls\"}"})]
      (is (= 1 (count ops)) (pr-str ops))
      (is (= :act (:op (first ops))))
      (is (= "task.run" (name (get-in (first ops) [:args :action])))))))

(deftest more-of-the-models-framings
  (testing "op over args, the args an act (live 2026-10-31)"
    (let [{:keys [ops reply]} (ops/parse {:text (str "I'll read it.<tool_call>op read<tool_call>args {\"object\": \"" (random-uuid) "\", \"action\": \"run\", \"args\": {\"cmd\": \"cat x\"}}</arg_value></tool_call>")})]
      (is (= 1 (count ops)) (pr-str ops))
      (is (= :act (:op (first ops))) (pr-str ops))
      (is (= "I'll read it." reply))))
  (testing "op over args, a plain read"
    (let [{:keys [ops reply]} (ops/parse {:text "<tool_call>op read<tool_call>args {\"name\": \"person/instructions/soul\"}</arg_value></tool_call>"})]
      (is (= :read (:op (first ops))) (pr-str ops))
      (is (= "" reply))))
  (testing "the model's own key and value pairs"
    (let [{:keys [ops]} (ops/parse {:text "<tool_call>read<arg_key>name</arg_key><arg_value>harness/README.md</arg_value><arg_key>range</arg_key><arg_value>[1, 3]</arg_value></tool_call>"})]
      (is (= :read (:op (first ops))) (pr-str ops))
      (is (= "harness/README.md" (get-in (first ops) [:args :name]))))))

(deftest the-op-straight-after-the-tag
  (let [line "{\"op\":\"act\",\"args\":{\"action\":\"task.run\",\"args\":{\"adapter\":\"a.shell\",\"cmd\":\"ls\"}}}"
        {:keys [ops reply]} (ops/parse {:text (str "I'll search.<tool_call>" line)})]
    (is (= 1 (count ops)))
    (is (= :act (:op (first ops))))
    (is (= "I'll search." reply))))
