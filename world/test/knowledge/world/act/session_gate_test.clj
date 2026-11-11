(ns knowledge.world.act.session-gate-test
  "A token held by a process is no way round a gate (decisions 2026-10-31): the gate is the loop's,
   met at the protocol for any session that is not a person's or the operator's key."
  (:require [clojure.java.io :as io]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))

(defn- process!
  "A process Principal root makes, able to run on shell.local, with its own token."
  [root n]
  (let [r (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name n}})
        id (get-in r [:result :object])
        shell (get-in (ws/request!! root :resolve {:name "shell.local" :op :read}) [:result :object])
        run (get-in (ws/request!! root :resolve {:name "action/task.run" :op :read}) [:result :object])]
    (fx/grant! root id #{id} #{:read :write})
    (fx/grant! root id #{shell} #{:read :evaluate})
    (when run (fx/grant! root id #{run} #{:read :evaluate}))
    ;; a Principal a person's root makes can carry its own card
    (let [schema (get-in (ws/request!! root :resolve {:name "schema.Task" :op :read}) [:result :object])]
      (fx/grant! root id #{schema} #{:read :create}))
    {:id id :token (get-in r [:result :result :token]) :shell shell}))

(deftest a-process-session-meets-the-gate
  (fx/with-world [{:keys [world root witness]}]
    (let [{:keys [id token]} (process! root "worker")
          c (fx/attach-as world token)]
      (is (some? token))
      (testing "a gated run is a card, and nothing ran"
        (let [r (act! c "task.run" {:adapter "shell.local" :cmd "echo from-socket"})]
          (is (= :approval-pending (code r)) (pr-str r))
          (let [task (parse-uuid (str (get-in r [:err :data :task])))]
            (is task (pr-str r))
            (is (= :pending (field world task :task/state)))
            (is (= id (field world task :task/requested-by))))))
      (testing "asking again is the same card"
        (let [before (count (objects/objects-of-class (act/db world) :task))]
          (act! c "task.run" {:adapter "shell.local" :cmd "echo from-socket"})
          (is (= before (count (objects/objects-of-class (act/db world) :task))))))
      (testing "what is not gated goes on"
        (is (:ok (ws/request!! c :set-field {:object id :field :instructions/own :type :string :value "mine" :reader :both})))
        (is (= "mine" (field world id :instructions/own)))))))

(deftest the-card-can-be-decided
  (fx/with-world [{:keys [world root witness]}]
    (let [{:keys [token]} (process! root "worker")
          c (fx/attach-as world token)
          r (act! c "task.run" {:adapter "shell.local" :cmd "echo decided"})
          task (parse-uuid (str (get-in r [:err :data :task])))
          ;; no wake and no person among its creators: the oldest person in the world decides
          [approver] (field world task :task/approvers)
          token (if (= approver (:id witness)) (:token witness) (slurp (io/file (:data-dir world) "person.token")))
          person (fx/attach-as world token)
          _ (fx/grant! root approver
                       #{(get-in (ws/request!! root :resolve {:name "action/task.approve" :op :read}) [:result :object])}
                       #{:read :evaluate})
          a (act! person "task.approve" {:task (str task)})]
      (is (:ok a) (pr-str a))
      (is (= :done (field world task :task/state)) (pr-str (field world task :task/result))))))

(deftest a-rule-admits-as-in-the-loop
  (fx/with-world [{:keys [world root witness]}]
    (let [{:keys [id token]} (process! root "worker")
          c (fx/attach-as world token)
          person (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{id} #{:read :write})
      (let [add (get-in (ws/request!! root :resolve {:name "action/approval.rule.add" :op :read}) [:result :object])]
        (fx/grant! root (:id witness) #{add} #{:read :evaluate}))
      (let [r (act! person "approval.rule.add" {:principal (str id) :action "task.run" :match {:preset "read-only"}})]
        (is (:ok r) (pr-str r)))
      (let [r (act! c "task.run" {:adapter "shell.local" :cmd "ls"})]
        (is (:ok r) (pr-str r))
        (is (some? (get-in r [:result :result :approval])) "a Task records it")))))

(deftest who-passes
  (fx/with-world [{:keys [world root witness]}]
    (testing "a person's own session"
      (let [person (fx/attach-as world (:token witness))
            shell (get-in (ws/request!! root :resolve {:name "shell.local" :op :read}) [:result :object])]
        (fx/grant! root (:id witness) #{shell} #{:read :evaluate})
        (let [run (get-in (ws/request!! root :resolve {:name "action/task.run" :op :read}) [:result :object])]
          (when run (fx/grant! root (:id witness) #{run} #{:read :evaluate})))
        (is (:ok (act! person "task.run" {:adapter "shell.local" :cmd "echo person"})))))
    (testing "the operator's key"
      (is (:ok (act! root "task.run" {:adapter "shell.local" :cmd "echo operator"}))))
    (testing "root on a token it issued is a process like any other"
      (let [r (act! root "token.issue" {:principal (str (:principal root)) :label "second"})
            secret (or (get-in r [:result :result :secret]) (get-in r [:result :secret]))]
        (when secret
          (let [c (fx/attach-as world secret)]
            (is (= :approval-pending (code (act! c "task.run" {:adapter "shell.local" :cmd "echo second"}))))))))))

(deftest the-loop-is-not-gated-twice
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          before (count (objects/objects-of-class (act/db world) :task))
          [r] (lops/execute! world root-id nil [{:op :act :args {:action :task.run :args {:adapter "shell.local" :cmd "echo loop"}}}])]
      (is (= :approval-pending (keyword (name (get-in r [:err :code])))) (pr-str r))
      (is (= (inc before) (count (objects/objects-of-class (act/db world) :task)))))))

(deftest a-doomed-grant-raises-no-card
  (fx/with-world [{:keys [world root]}]
    (let [{:keys [id token]} (process! root "worker")
          c (fx/attach-as world token)
          before (count (objects/objects-of-class (act/db world) :task))]
      (is (= :unresolved (code (ws/request!! c :grant {:grantee id :scope #{(:principal root)} :ops #{:write}}))))
      (is (= before (count (objects/objects-of-class (act/db world) :task)))))))
