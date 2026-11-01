(ns knowledge.world.delegate-test
  "Short-lived subagents and the work handed to them (decisions 2026-10-31)."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.model-api :as model]
            [knowledge.world.delegate :as delegate]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.loop.core :as loop]
            [knowledge.world.loop.core]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.ws-client :as ws])
  (:import [java.util Date]))

(defn- v [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- result [r] (assert (:ok r) (pr-str r)) (get-in r [:result :result]))
(defn- task! [root title] (get-in (ws/request!! root :act {:class :task :action :create :args {:title title}}) [:result :object]))
(defn- later [ms] (Date. (+ (System/currentTimeMillis) ms)))
(defn- retired? [world id] (objects/retired? (act/db world) id))
(defn- active [world id] (resolve/active-grants (act/db world) #{id} (Date.)))

(defn- ttl-child! [root nm extra]
  (result (act! root "principal.create"
                (merge {:name nm :charter "a test subagent" :ttl-s 600 :grant {:scope {:prefix nm} :ops ["read"]}} extra))))

(deftest a-ttl-makes-it-temporary
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          {:keys [principal until route supervisor]} (ttl-child! root "tmp" {})
          st (delegate/state (act/db world) principal)]
      (is (some? until))
      (is (= root-id (:parent st)))
      (is (inst? (:until st)))
      (testing "its Grants end with it"
        (is (every? #(inst? (:grant/until %)) (active world principal))))
      (testing "nobody can write what it is"
        (is (= :conflict (keyword (name (get-in (ws/request!! root :set-field
                                                              {:object principal :field :principal/ephemeral :type :map :value {}})
                                                [:err :code]))))))
      (testing "still here while its time runs"
        (is (empty? (delegate/sweep! world (Date.))))
        (is (not (retired? world principal))))
      (testing "past its time it goes from every list, and its record stays"
        (is (= [principal] (delegate/sweep! world (later 700000))))
        (is (retired? world principal))
        (is (retired? world route))
        (is (retired? world supervisor))
        (is (empty? (active world principal)))
        (is (not-any? #{route} (v world root-id :principal/routes)))
        (is (= "a test subagent" (v world principal :instructions/charter)) "the record reads back")))))

(deftest it-ends-with-its-task
  (fx/with-world [{:keys [world root]}]
    (let [t (task! root "a job")
          {:keys [principal]} (ttl-child! root "worker" {:for (str t)})]
      (is (= t (delegate/task-of (act/db world) principal)))
      (is (empty? (delegate/sweep! world (Date.))))
      (delegate/result! world principal "all done" nil)
      (is (= :done (keyword (name (v world t :task/state)))))
      (is (= "all done" (:text (v world t :task/result))))
      (is (= [principal] (delegate/sweep! world (Date.))))
      (is (retired? world principal)))))

(deftest it-cannot-outlive-or-outrank-its-parent
  (fx/with-world [{:keys [world root]}]
    (let [{parent :principal} (ttl-child! root "parent" {:ttl-s 100})
          st (delegate/state (act/db world) parent)
          capped (delegate/until-for (act/db world) parent 9000 (Date.))]
      (is (not (.after capped (:until st))) "a child's end is never after its parent's")
      (is (every? #(not (:grant/delegable %)) (active world parent)) "it can give nothing away")
      (testing "a Task's Principal goes when its parent does"
        (let [t (task! root "orphaned")
              {child :principal} (ttl-child! root "child" {:for (str t)})]
          (delegate/mark! world child (assoc (delegate/state (act/db world) child) :parent parent))
          (delegate/retire! world parent "test")
          (is (some #{child} (delegate/sweep! world (Date.))))
          (is (= :failed (keyword (name (v world t :task/state))))))))))

(deftest progress-is-read-not-heard
  (fx/with-world [{:keys [world root]}]
    (let [t (task! root "slow")
          {:keys [principal]} (ttl-child! root "slowpoke" {:for (str t)})
          head (log/head (:log world))]
      (is (:ok (delegate/progress! world principal t "halfway")))
      (is (= "halfway" (v world t :task/progress)))
      (is (empty? (filter #(= :sent (:event/kind %)) (log/read-from (:log world) head 1000))) "no Message"))))

(deftest a-deadline-is-heard-once
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          t (task! root "late")
          {:keys [principal]} (ttl-child! root "late" {:for (str t)})
          told #(filter (fn [m] (and (= root-id (first (v world m :message/to)))
                                     (str/includes? (str (v world m :message/text)) "passed its deadline")))
                        (objects/objects-with-field (act/db world) :message/text))]
      (ws/request!! root :set-field {:object t :field :task/due :type :instant :value (Date. 1000)})
      (is (empty? (delegate/sweep! world (Date.))))
      (is (true? (v world t :task/overdue-told)))
      (delegate/sweep! world (Date.))
      (is (= 1 (count (told))))
      (is (not (retired? world principal)) "the parent decides; a deadline ends nothing"))))

(deftest a-stall-fails-the-task
  (fx/with-world [{:keys [world root]}]
    (let [t (task! root "stalls")
          {:keys [principal]} (ttl-child! root "stuck" {:for (str t)})]
      (ws/request!! root :set-field {:object principal :field :loop/stalled :type :string :value "evaluation failed: boom"})
      (is (= [principal] (delegate/sweep! world (Date.))))
      (is (= :failed (keyword (name (v world t :task/state)))))
      (is (str/includes? (:reason (v world t :task/result)) "boom")))))

(deftest a-delegated-task-comes-back-and-the-subagent-goes
  (fx/with-world [{:keys [world root]}]
    (let [{:keys [delegated principal name deadline-s]} (result (act! root "delegate" {:task "say something"}))]
      (is (= 900 deadline-s))
      (is (some? name))
      (is (inst? (v world delegated :task/due)))
      (is (= principal (first (filter #(= delegated (delegate/task-of (act/db world) %))
                                      (objects/objects-with-field (act/db world) :principal/ephemeral)))))
      (testing "the stub's final text is the result, and the subagent is retired"
        (is (until #(= :done (some-> (v world delegated :task/state) clojure.core/name keyword)) 15000))
        (is (seq (:text (v world delegated :task/result))))
        (is (until #(retired? world principal) 8000))
        (is (empty? (active world principal)))))))

(deftest what-is-narrower-stays-narrower
  (fx/with-world [{:keys [world root]}]
    (let [{:keys [principal]} (ttl-child! root "narrow" {})]
      (is (every? (fn [g] (not-any? #{:write :delegate} (when (not= #{principal} (:grant/scope g)) (remove #{:evaluate} (:grant/ops g)))))
                  (remove #(contains? (:grant/ops %) :create) (active world principal)))
          "a temporary Principal writes only itself"))))

(deftest an-approved-delegation-wakes-nobody-until-the-result
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          obj (fn [class fields]
                (get-in (ws/request!! root :act {:class class :action :create :args {:fields fields}}) [:result :object]))
          put! (fn [o k t v] (ws/request!! root :set-field {:object o :field k :type t :value v :reader :both}))
          card (fn [action ok]
                 (let [t (obj :object {})
                       m (obj :object {})]
                   (put! t :task/requested-by [:ref :principal] p)
                   (put! t :task/op :map {:op "act" :args {:action action}})
                   (put! t :task/result :map {:ok ok :text "x"})
                   (put! m :message/kind :keyword :approval-result)
                   (put! m :message/task [:ref :task] t)
                   m))
          own? (fn [m] (#'loop/own-approval-result? (act/db world) p m))]
      (is (false? (boolean (own? (card "delegate" true)))) "the subagent's result is the next wake")
      (is (true? (boolean (own? (card "delegate" false)))) "a delegation that failed has something to say")
      (is (true? (boolean (own? (card "task.run" true)))) "any other approval still wakes"))))
