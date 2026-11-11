(ns knowledge.world.loop.ops-test
  "The ops parser alone (spec 9.8 steps 5 and 6): closed fences are ops, an unclosed ```ops fence
   is a nit the parser strips and counts, markdown code fences in a reply are left
   alone; and the loop's quiet-reply rule for Trigger wakes."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.loop.core :as lcore]
            [knowledge.world.loop.ops :as lops]))

(deftest a-closed-fence-is-ops-and-the-rest-is-the-reply
  (let [{:keys [ops reply malformed-fences]}
        (lops/parse {:text "Reading first.\n```ops\n{\"op\": \"read\", \"args\": {\"name\": \"harness/docs/ethos.md\"}}\n```\nThen I answer."})]
    (is (= 1 (count ops)))
    (is (= :read (:op (first ops))))
    (is (= "Reading first.\n\nThen I answer." reply))
    (is (= 0 malformed-fences))))

(deftest an-unclosed-ops-fence-is-stripped-and-counted
  (let [{:keys [ops reply malformed-fences]}
        (lops/parse {:text "Done. The counts are 8 tests, 227 assertions.\n```ops\nThat line was a slip, not an op."})]
    (is (empty? ops) "no block, no ops")
    (is (= 1 malformed-fences))
    (is (= "Done. The counts are 8 tests, 227 assertions.\nThat line was a slip, not an op." reply)
        "the marker went, the text stayed")))

(deftest markdown-code-fences-in-a-reply-are-not-ops-and-are-kept
  (let [text "Here is the loop:\n```clojure\n(defn cycle! [world p wake] ...)\n```\nEight iterations."
        {:keys [ops reply malformed-fences]} (lops/parse {:text text})]
    (is (empty? ops))
    (is (= 0 malformed-fences))
    (is (= text reply) "a language fence is prose to the parser")))

(deftest quiet-replies-to-a-trigger
  (testing "blank or a bare 'nothing to do' is quiet"
    (doseq [t ["" "   " "nothing to do" "Nothing to do." "No action needed." "all quiet" "ok" "Nothing to report."]]
      (is (lcore/quiet-reply? t) (pr-str t))))
  (testing "a short clean-check status that closes on nothing to act on is quiet too (what the real model wrote to a heartbeat)"
    (is (lcore/quiet-reply? "No pending Tasks are visible in my working set; memory/notes is empty (well under memory/budget 4000), and no Principals I created report anything in loop/stalled. Nothing to act on.")))
  (testing "anything with content is not"
    (doseq [t ["Nothing to do about the Tasks, but memory/notes is over budget: trimming it."
               "Two Tasks pending approval: 1eef20d7 and 4ff265e5."
               "nothing to do\n\nexcept that grid stalled at seq 40603"
               "scout stalled: evaluation failed after 1 s (model/timeout-s). I will raise its timeout. Nothing else to report, but this needs action."]]
      (is (not (lcore/quiet-reply? t)) (pr-str t)))))
