(ns knowledge.world.act.gates-test
  "Approval gates (plan §19.1 W4, decisions 2026-10-30) on the stub Model: a loop's gated op becomes
   a pending Task delivered to the approver as a card, the model sees err approval-pending and
   replies; task.approve runs the op as the requester with the Task as via and wakes the requester
   with the result; task.deny tells it the reason; a non-approver is unresolved; an expired Task
   is expired and the requester told; an ungated op and a session's own op are untouched."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.handlers]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))
(defn- field [c id k] (v (fields c id) k))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))
(def run-line "{\"op\": \"act\", \"args\": {\"action\": \"task.run\", \"args\": {\"adapter\": \"shell.local\", \"cmd\": \"echo gated-hello\"}}}")
(defn- set-field-line [object field value]
  (str "{\"op\": \"set-field\", \"args\": {\"object\": \"" object "\", \"field\": \"" field "\", \"type\": \"string\", \"value\": \"" value "\"}}"))

(defn- deliveries
  "Message ids delivered on sub until pred holds over them or timeout-ms passes."
  [c sub pred timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop [acc []]
      (if (or (pred acc) (> (System/currentTimeMillis) deadline))
        acc
        (let [push (ws/await-push c sub 2000)
              ids (keep #(when (= :delivered (:event/kind %)) (get-in % [:event/body :message])) (:events push))]
          (recur (into acc ids)))))))

(defn- kind-of [c m] (some-> (field c m :message/kind) name keyword))

(defn- setup
  "witness (a person) may read itself and transmit on root.route, and listens on its inbox."
  [root witness world]
  (let [route (resolve-as root "root.route" :read)
        w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{(:id witness)} #{:read})
    (fx/grant! root (:id witness) #{route} #{:transmit})
    (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
    {:route route :w w}))

(deftest a-card-for-an-edit-names-the-file-first
  (let [sum (fn [args] (gates/summary nil :act {:action "file.edit" :args args}))
        long-new (str/join "\n" (repeat 40 "    log::info(\"store: sub {} skipped seq {}..{} ({}): filtered by resolution\", hole.sub);"))]
    (testing "a long `new` no longer pushes the path past the clip"
      (let [s (sum {:new long-new :old "x\ny" :name "proj/listener/src/util/Log.hpp"})]
        (is (str/starts-with? s "edit `proj/listener/src/util/Log.hpp`: 2 lines to 40"))
        (is (< (count s) 200))))
    (testing "the shapes"
      (is (= "create `proj/new.md`: 1 line" (subs (sum {:new "hello\n" :name "proj/new.md"}) 0 (count "create `proj/new.md`: 1 line"))))
      (is (str/includes? (sum {:new "x" :at "top" :name "p/a"}) "insert 1 line at top"))
      (is (str/includes? (sum {:new "two" :old "second line" :all true :name "p/a"}) "replace every `second line`")))))

(deftest a-long-approved-run-does-not-hold-the-card
  (fx/with-world [{:keys [world root witness]}]
    (let [{:keys [route w]} (setup root witness world)
          line (str/replace run-line "echo gated-hello" "sleep 1; echo gated-late")
          _ (ws/request!! w :send {:to #{(:principal root)} :route route :text (str "please run this:\n" (ops-block line))})
          got (deliveries w "inbox" #(some (fn [m] (= :approval (kind-of root m))) %) 20000)
          task (field root (first (filter #(= :approval (kind-of root %)) got)) :message/task)]
      (with-redefs [knowledge.world.act.handlers/approve-wait-ms 50]
        (let [r (act! w "task.approve" {:task task})]
          (testing "the answer comes at once and says it is running"
            (is (:ok r) (pr-str r))
            (is (= "running" (get-in r [:result :result :state] (get-in r [:result :state]))) (pr-str r))
            (is (= :approved (field root task :task/state)))))
        (testing "and the result lands when it ends"
          (is (loop [n 200]
                (cond (= :done (field root task :task/state)) true
                      (zero? n) false
                      :else (do (Thread/sleep 50) (recur (dec n))))))
          (is (str/includes? (str (field root task :task/result)) "gated-late")))))))

(deftest a-gated-op-becomes-a-pending-task-the-approver-decides
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          sent (ws/request!! w :send {:to #{root-id} :route route :text (str "please run this:\n" (ops-block run-line))})
          ask (get-in sent [:result :message])
          ;; two deliveries to the witness: the approval card, then root's reply
          got (deliveries w "inbox" #(>= (count %) 2) 20000)
          card (first (filter #(= :approval (kind-of root %)) got))
          reply (first (filter #(contains? #{nil :status} (kind-of root %)) got))] ; an interim reply is a status
      (is (:ok sent) (pr-str sent))
      (testing "the card: a Message of kind approval from root, in-reply-to the ask, naming the Task"
        (is (some? card) (pr-str got))
        (is (= root-id (field root card :message/from)))
        (is (= ask (field root card :message/in-reply-to)))
        (is (uuid? (field root card :message/task)))
        (is (str/includes? (str (field root card :message/text)) "echo gated-hello")))
      (let [task (field root card :message/task)
            tf (fields w task)]
        (testing "the Task, as the approver reads it (its Grant): pending, who asked, what op, who decides, when it expires"
          (is (= "approval" (v tf :task/kind)))
          (is (= :pending (v tf :task/state)))
          (is (= root-id (v tf :task/requested-by)))
          (is (= "act" (get-in (v tf :task/op) [:op])))
          (is (= "task.run" (name (get-in (v tf :task/op) [:args :action]))))
          (is (= [(:id witness)] (v tf :task/approvers)))
          (is (instance? java.util.Date (v tf :task/expires-at)))
          (is (uuid? (v tf :task/evaluation)))
          (is (= ask (v tf :task/message)))
          (is (= :conflict (get-in (act! root "task.approve" {:task (str task)}) [:err :code]))
              "the requester never decides its own gate, even as root")
          (is (string? (v tf :task/summary))))
        (testing "the model saw err approval-pending in its results and replied without retrying"
          (let [ev (v tf :task/evaluation)
                results (field root ev :evaluation/results)]
            (is (= 1 (count results)))
            (is (false? (:ok (first results))))
            (is (= :approval-pending (some-> (get-in (first results) [:err :code]) name keyword)) (pr-str results))
            (is (= task (some-> (get-in (first results) [:err :task]) str parse-uuid)))
            (is (some? reply) "root replied")
            (is (= "" (field root root-id :loop/stalled)))))
        (testing "nothing ran: no Task of the run exists yet"
          (let [tasks (get-in (ws/request!! root :history {:after 0 :limit 1000}) [:result :events])]
            (is (not-any? #(and (= :field-set (:event/kind %)) (= :task/command (get-in % [:event/body :name])) (= "echo gated-hello" (get-in % [:event/body :value]))) tasks))))
        (testing "a non-approver may not decide it"
          (let [r (ws/request!! root :act {:class :principal :action :create :args {:driven-by :person :name "bystander"}})
                c (fx/attach-as world (get-in r [:result :result :token]))]
            (is (= :unresolved (get-in (act! c "task.approve" {:task (str task)}) [:err :code])))
            (is (= :pending (field w task :task/state)))
            (ws/close! c)))
        (testing "the approver approves: the op runs as root with the Task as via, the Task is done, root is woken with the result"
          (let [r (act! w "task.approve" {:task (str task) :note "go"})
                res (get-in r [:result :result])]
            (is (:ok r) (pr-str r))
            (is (= "done" (:state res)) (pr-str res))
            (is (str/includes? (str (:outcome res)) "gated-hello") (pr-str res))
            (let [tf (fields w task)]
              (is (= :done (v tf :task/state)))
              (is (= (:id witness) (v tf :task/decided-by)))
              (is (= "go" (v tf :task/note)))
              (is (true? (get-in (v tf :task/result) [:ok]))))
            (let [events (get-in (ws/request!! root :history {:last 500}) [:result :events])
                  run-cmd (last (filter #(and (= :field-set (:event/kind %)) (= :task/command (get-in % [:event/body :name])) (= "echo gated-hello" (get-in % [:event/body :value]))) events))]
              (is (some? run-cmd) "the run's own Task was created")
              (is (= root-id (:event/actor run-cmd)) "as the requester")
              (is (= task (:event/via run-cmd)) "with the approval Task as via"))
            (let [more (deliveries w "inbox" #(>= (count %) 1) 20000)
                  continuation (first more)]
              (is (some? continuation) "root woke on the approval-result and answered the witness again")
              (is (= root-id (field root continuation :message/from))))))
        (ws/close! w)))))

(deftest deny-tells-the-requester-and-nothing-runs
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          _ (ws/request!! w :send {:to #{root-id} :route route :text (str "please run this:\n" (ops-block run-line))})
          got (deliveries w "inbox" #(>= (count %) 2) 20000)
          card (first (filter #(= :approval (kind-of root %)) got))
          task (field root card :message/task)
          r (act! w "task.deny" {:task (str task) :reason "not on my watch"})]
      (is (:ok r) (pr-str r))
      (is (= "denied" (get-in r [:result :result :state])))
      (is (= :denied (field w task :task/state)))
      (is (= "not on my watch" (field w task :task/reason)))
      (testing "the requester is told and wakes; approving afterwards is a conflict"
        (let [more (deliveries w "inbox" #(>= (count %) 1) 20000)]
          (is (some? (first more)) "root answered after the denial"))
        (is (= :conflict (get-in (act! w "task.approve" {:task (str task)}) [:err :code]))))
      (testing "no run happened"
        (let [events (get-in (ws/request!! root :history {:after 0 :limit 1000}) [:result :events])]
          (is (not-any? #(and (= :field-set (:event/kind %)) (= :task/command (get-in % [:event/body :name])) (= "echo gated-hello" (get-in % [:event/body :value]))) events))))
      (ws/close! w))))

(deftest an-expired-task-is-expired-and-the-requester-told
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)]
      (fx/policy! world root {:object root-id :field :policy/approval-ttl-s :type :number :value 0 :reader :both})
      (ws/request!! w :send {:to #{root-id} :route route :text (str "please run this:\n" (ops-block run-line))})
      (let [got (deliveries w "inbox" #(>= (count %) 2) 20000)
            card (first (filter #(= :approval (kind-of root %)) got))
            task (field root card :message/task)]
        (Thread/sleep 20)
        (is (= [task] (gates/expire! world)))
        (is (= :expired (field w task :task/state)))
        (is (= :conflict (get-in (act! w "task.approve" {:task (str task)}) [:err :code])))
        (testing "the requester was told (a Message of kind approval-result about the Task, delivered to root)"
          (let [told (until (fn [] (some (fn [e] (let [m (get-in e [:event/body :message])]
                                                   (when (and (= :delivered (:event/kind e)) (= root-id (get-in e [:event/body :recipient]))
                                                              (= task (field root m :message/task))
                                                              (= :approval-result (kind-of root m)))
                                                     m)))
                                        (get-in (ws/request!! root :history {:stream root-id :limit 2000}) [:result :events])))
                           5000)]
            (is (uuid? told)))))
      (ws/close! w))))

(deftest ungated-ops-and-sessions-are-untouched
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)]
      (testing "an op the policy does not name runs at once"
        (ws/request!! w :send {:to #{root-id} :route route :text (str "please run this:\n" (ops-block (set-field-line root-id "loop/motto" "ungated")))})
        (let [got (deliveries w "inbox" #(>= (count %) 1) 20000)]
          (is (= 1 (count got)) "one delivery: the reply, no card")
          (is (nil? (kind-of root (first got))))
          (is (= "ungated" (field root root-id :loop/motto)))))
      (testing "a session's own task.run is never gated"
        (let [r (act! root "task.run" {:adapter "shell.local" :cmd "echo session-run"})]
          (is (:ok r) (pr-str r))
          (is (= 0 (get-in r [:result :result :exit])))
          (is (str/includes? (str (get-in r [:result :result :stdout])) "session-run"))))
      (testing "the second of two gated ops in a block stands behind the first: one card, one Task"
        (ws/request!! w :send {:to #{root-id} :route route :text (str "please run this:\n" (ops-block run-line run-line))})
        (let [got (deliveries w "inbox" #(>= (count %) 2) 20000)
              cards (filter #(= :approval (kind-of root %)) got)]
          (is (= 1 (count cards)) (pr-str (map #(kind-of root %) got)))
          (let [task (field root (first cards) :message/task)
                ev (field w task :task/evaluation)
                results (field root ev :evaluation/results)]
            (is (= 2 (count results)))
            (is (= :held (some-> (get-in (second results) [:err :code]) name keyword)) (pr-str (second results))))))
      (testing "the policy is a Field on the Principal with the shipped defaults"
        (is (= ["task.run" "shell" "grant" "principal.create"] (field root root-id :policy/approval)))
        (is (= 86400 (field root root-id :policy/approval-ttl-s))))
      (ws/close! w))))
