(ns knowledge.world.reply-record-test
  "The kernel's record of a wake's runs rides on the reply (and a contradiction is
   recorded), run results carry their tails, a workspace shell for a harness subtree runs at the
   repository root, unanswered questions are marked and answered, the asker sits on a new
   Principal's Route, and the parser reads two more of the model's framings."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.loop.core :as lcore]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- ser-thread [world p message] (vec (ser/thread-lines (act/db world) p message)))

(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))
(defn- field [c m k] (v (fields c m) k))
(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- run-line [cmd] (str "{\"op\": \"act\", \"args\": {\"action\": \"task.run\", \"args\": {\"adapter\": \"shell.local\", \"cmd\": " (pr-str cmd) "}}}"))

(defn- deliveries [c sub pred timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop [acc []]
      (if (or (pred acc) (> (System/currentTimeMillis) deadline))
        acc
        (let [push (ws/await-push c sub 2000)
              ids (keep #(when (= :delivered (:event/kind %)) (get-in % [:event/body :message])) (:events push))]
          (recur (into acc ids)))))))

(defn- setup
  "witness (a person) may read itself and transmit on root.route, and listens on its inbox."
  [root witness world]
  (let [route (resolve-as root "root.route" :read)
        w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{(:id witness)} #{:read})
    (fx/grant! root (:id witness) #{route} #{:transmit})
    (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
    {:route route :w w}))

(deftest the-contradiction-rule
  (let [runs [{:cmd "make test" :exit 2 :duration-ms 3000 :tail ["FAILED"]}]]
    (is (nil? (lcore/contradiction "make test exited 2; see the tail." runs)))
    (is (string? (lcore/contradiction "All tests passed, 0 failures." runs)) "success claimed over an exit 2")
    (is (string? (lcore/contradiction "The run exited 0." runs)) "a code no run had")
    (is (nil? (lcore/contradiction "anything" [])) "no runs, nothing to contradict")
    (is (nil? (lcore/contradiction "it passed" [{:cmd "ls" :exit 0}])) "success over exit 0 is consistent"))
  (testing "the block names the command, the exit and the tail"
    (let [b (lcore/runs-block [{:cmd "make test-unit" :exit 0 :duration-ms 21500 :tail ["133 tests, 1695 assertions, 0 failures."]}])]
      (is (str/includes? b "Runs this wake (kernel record)"))
      (is (str/includes? b "`make test-unit` → exit 0 (21 s)"))
      (is (str/includes? b "133 tests, 1695 assertions, 0 failures."))))
  (is (nil? (lcore/runs-block []))))

(deftest a-reply-carries-the-kernel-record-of-its-runs
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)]
      (testing "an ungated run: the reply ends with the run's exit and the tail of its output"
        (fx/policy! world root {:object root-id :field :policy/approval :type [:list :string] :value [] :reader :both})
        (let [cmd "for i in a b c d; do echo row-$i; done; exit 3"
              sent (ws/request!! w :send {:to #{root-id} :route route :text (str "run this:\n" (ops-block (run-line cmd)))})
              got (deliveries w "inbox" #(>= (count %) 1) 30000)
              reply (first got)]
          (is (:ok sent) (pr-str sent))
          (is (some? reply) "root replied")
          (let [text (str (field root reply :message/text))]
            (is (str/includes? text "Runs this wake (kernel record)") text)
            (is (str/includes? text "→ exit 3") text)
            (is (str/includes? text "row-d") "the tail of stdout, not its head")
            (is (not (str/includes? text "row-a")) "only the last three lines"))))
      (testing "a gated run: the continuation after approval carries the run the approval executed"
        (fx/policy! world root {:object root-id :field :policy/approval :type [:list :string] :value ["task.run"] :reader :both})
        (let [before (count (deliveries w "inbox" (constantly true) 1))
              _ (ws/request!! w :send {:to #{root-id} :route route :text (str "and this:\n" (ops-block (run-line "echo kernel-says-hi")))})
              got (deliveries w "inbox" #(>= (count %) 2) 30000)
              card (first (filter #(= :approval (some-> (field root % :message/kind) name keyword)) got))
              task (field root card :message/task)
              _ (is (uuid? task) (pr-str got))
              r (act! w "task.approve" {:task (str task) :note "go"})
              _ (is (:ok r) (pr-str [r (fields root task)]))
              more (deliveries w "inbox" #(some (fn [m] (and (nil? (some-> (field root m :message/kind) name keyword))
                                                              (str/includes? (str (field root m :message/text)) "kernel-says-hi")))
                                                 %) 30000)
              continuation (first (filter #(str/includes? (str (field root % :message/text)) "Runs this wake") more))]
          (is (some? continuation) (pr-str (map #(field root % :message/text) more)))
          (is (str/includes? (str (field root continuation :message/text)) "echo kernel-says-hi"))
          (is (str/includes? (str (field root continuation :message/text)) "→ exit 0"))
          (let [tf (fields root task)]
            (is (uuid? (v tf :task/ran)) "the approval Task points at the run")
            (is (= 0 (v tf :task/exit)))))))))

(deftest a-result-wake-answers-the-asker
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          _ (fx/policy! world root {:object root-id :field :policy/approval :type [:list :string] :value ["task.run"] :reader :both})
          sent (ws/request!! w :send {:to #{root-id} :route route :text (str "please run:\n" (ops-block (run-line "echo answer-me")))})
          question (get-in sent [:result :message])
          got (deliveries w "inbox" #(>= (count %) 2) 30000)
          card (first (filter #(= :approval (some-> (field root % :message/kind) name keyword)) got))
          task (field root card :message/task)]
      (is (uuid? task) (pr-str got))
      (testing "before the decision, the thread shows root the open question"
        (let [lines (ser-thread world root-id card)]
          (is (some #(str/includes? % "awaiting your answer") lines) (pr-str lines))))
      (let [r (act! w "task.approve" {:task (str task)})
            _ (is (:ok r) (pr-str r))
            more (deliveries w "inbox" #(some (fn [m] (= question (field root m :message/in-reply-to))) %) 30000)
            answer (first (filter #(= question (field root % :message/in-reply-to)) more))]
        (testing "the continuation after the result is addressed to the asker, in-reply-to the question"
          (is (some? answer) (pr-str (map #(vector (field root % :message/kind) (field root % :message/in-reply-to)) more)))
          (is (= root-id (field root answer :message/from)))
          (is (contains? (set (field root answer :message/to)) (:id witness)))
          (is (str/includes? (str (field root answer :message/text)) "answer-me") "with the kernel's record of the run"))
        (testing "afterwards the thread shows the question answered"
          (let [lines (ser-thread world root-id (last more))]
            (is (some #(str/includes? % "<- you answered") lines) (pr-str lines))))))))

(deftest a-harness-subtree-shell-runs-at-the-repository-root
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          created (act! root "principal.create" {:name "wdev" :charter "a developer" :grant {:scope {:prefix "wdev"} :ops ["read" "write" "transmit"]}})
          _ (is (:ok created) (pr-str created))
          target (get-in created [:result :result :principal])
          m (act! root "workspace.mount" {:principal (str target) :path "harness/world" :name "wdev"})
          res (get-in m [:result :result])]
      (is (:ok m) (pr-str m))
      (testing "the shell's cwd is the repository root and its scope the subtree"
        (is (string? (:shell-cwd res)) (pr-str res))
        (is (.exists (java.io.File. (str (:shell-cwd res) "/docs/protocol.md"))) "the repository root")
        (is (str/ends-with? (str (:shell-scope res)) "/world")))
      (testing "a command sees the repository root"
        (let [r (act! root "task.run" {:adapter "wdev.shell" :cmd "ls docs/protocol.md world/deps.edn"})
              rr (get-in r [:result :result])]
          (is (:ok r) (pr-str r))
          (is (= 0 (:exit rr)) (pr-str rr))
          (is (str/includes? (str (:stdout rr)) "world/deps.edn"))))
      (testing "names already bound are refused as conflict"
        (is (= :conflict (get-in (act! root "workspace.mount" {:principal (str target) :path "harness/world" :name "world"}) [:err :code])))
        (is (= :conflict (get-in (act! root "workspace.mount" {:principal (str target) :path "harness/world" :name "wdev"}) [:err :code])))))))

(deftest the-asker-sits-on-the-new-principals-route
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          ;; the default policy gates principal.create: the asker must get the seat through the approval path too
          line (str "{\"op\": \"act\", \"args\": {\"action\": \"principal.create\", \"args\": {\"name\": \"helper\", \"charter\": \"you help\", "
                    "\"grant\": {\"scope\": {\"prefix\": \"helper\"}, \"ops\": [\"read\", \"transmit\"]}}}}")
          _ (ws/request!! w :send {:to #{root-id} :route route :text (str "make me a helper:\n" (ops-block line))})
          got (deliveries w "inbox" #(>= (count %) 2) 30000)
          card (first (filter #(= :approval (some-> (field root % :message/kind) name keyword)) got))
          task (field root card :message/task)
          _ (is (uuid? task) (pr-str got))
          r (act! w "task.approve" {:task (str task)})
          _ (is (:ok r) (pr-str r))
          more (deliveries w "inbox" #(some (fn [m] (nil? (some-> (field root m :message/kind) name keyword))) %) 30000)]
      (is (seq more) "root replied after the approval")
      (let [helper (resolve-as w "helper" :read)]
        (is (uuid? helper) "the person who asked can resolve the new Principal")
        (let [hr (resolve-as w "root.helper" :transmit)]
          (is (uuid? hr) "and transmit on its Route")
          (is (contains? (set (field root hr :route/members)) (:id witness)) "as a member"))))))

(deftest strange-results-never-cost-the-wake
  (testing "fieldable drops nils and turns seqs into vectors; recordable replaces what still does not fit"
    (is (= {:a [1 2] :b {:c "x"}} (lops/fieldable {:a (list 1 nil 2) :b {:c "x" :d nil}})))
    (is (lops/storable? (lops/fieldable {:object nil :class :principal :events (map identity [{:seq 1}])})))
    (let [r (lops/recordable [{:op "read" :ok true :result {:object nil}} {:op "odd" :ok true :result (Object.)}])]
      (is (= 2 (count r)))
      (is (lops/storable? (first r)))
      (is (= :unrecordable (get-in (second r) [:err :code])))))
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          lines [(str "{\"op\": \"resolve\", \"args\": {\"name\": \"no-such-name-anywhere\", \"op\": \"read\"}}")
                 (str "{\"op\": \"history\", \"args\": {\"stream\": \"" root-id "\", \"limit\": 5}}")
                 (str "{\"op\": \"act\", \"args\": {\"action\": \"search.find\", \"args\": {\"text\": \"charter\", \"limit\": 3}}}")]
          _ (ws/request!! w :send {:to #{root-id} :route route :text (str "look around:\n" (apply ops-block lines))})
          got (deliveries w "inbox" #(>= (count %) 1) 30000)]
      (is (seq got) "root replied: the wake survived a resolve that found nothing, a history page and a search")
      (is (= "" (field root root-id :loop/stalled)) "no stall"))))
