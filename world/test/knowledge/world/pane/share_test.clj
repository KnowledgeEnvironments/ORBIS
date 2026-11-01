(ns knowledge.world.pane.share-test
  "A person's terminal is closed to agents until that person shares it, with one agent, for a time."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.ws-client :as ws])
  (:import [java.time Duration Instant]
           [java.util Date]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- result [r] (let [x (:result r)] (if (contains? x :result) (:result x) x)))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- by-name [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- loop-op [world p action args]
  (first (lops/execute! world p nil [{:op :act :args {:action action :args args}}])))

(defn- person-terminal!
  "A person's session and a terminal it opened: {:person client :id id :pane pane}."
  [root world witness]
  (let [w (fx/attach-as world (:token witness))
        id (:id witness)]
    (fx/grant! root id #{(by-name root "shell.local")} #{:read :evaluate})
    (doseq [a ["pane.open" "pane.share" "pane.unshare" "pane.read" "approval.unsafe.off"]]
      (fx/grant! root id #{(by-name root (str "action/" a))} #{:read :evaluate}))
    (let [r (act! w "pane.open" {:kind "terminal" :target "shell.local"})]
      {:person w :id id :pane (:pane (result r))})))

(defn- share! [w pane agent mode minutes]
  (act! w "pane.share" {:pane (str pane) :principal (str agent) :mode mode :minutes minutes}))

(deftest closed-until-shared
  (fx/with-world [{:keys [world root witness]}]
    (let [agent (:principal root)
          {:keys [person pane]} (person-terminal! root world witness)]
      (testing "an agent cannot tell it exists"
        (is (= :unresolved (code (act! root "pane.read" {:pane (str pane)}))))
        (is (= :unresolved (code (ws/request!! root :read {:object pane}))))
        (is (not-any? #(= pane (:pane %)) (:panes (result (act! root "pane.list" {})))))
        (is (empty? (filter #(= pane (:event/stream %))
                            (get-in (ws/request!! root :history {:stream pane}) [:result :events])))))
      (testing "reading"
        (is (:ok (share! person pane agent "read" 30)))
        (is (:ok (act! root "pane.read" {:pane (str pane)})))
        (is (some #(= pane (:pane %)) (:panes (result (act! root "pane.list" {}))))))
      (testing "reading is not typing"
        (is (= :unresolved (code (act! root "pane.send" {:pane (str pane) :text "x"}))))))))

(deftest typing-takes-a-card-whatever-the-policy
  (fx/with-world [{:keys [world root witness]}]
    (let [agent (:principal root)
          {:keys [person id pane]} (person-terminal! root world witness)
          send {:pane (str pane) :text "date\n"}]
      (is (:ok (share! person pane agent "type" 30)))
      (is (= "shell" (gates/gated? (act/db world) agent :act {:action "pane.send" :args send})))
      (testing "even with an empty policy"
        (let [plain {:action "pane.send" :args send}]
          (fx/grant! root id #{agent} #{:read :write})
          (is (:ok (ws/request!! person :set-field {:object agent :field :policy/approval :type [:list :string]
                                                    :value [] :reader :both})))
          (is (= "shell" (gates/gated? (act/db world) agent :act plain)))))
      (testing "unsafe mode does not admit it"
        (is (:ok (unsafe/arm! world id agent {:minutes 30})))
        (let [r (loop-op world agent "pane.send" send)]
          (is (false? (:ok r)) (pr-str r))
          (is (= :approval-pending (code r)) (pr-str r)))))))

(deftest it-shows-and-it-ends
  (fx/with-world [{:keys [world root witness]}]
    (let [agent (:principal root)
          wid (:world-id world)
          {:keys [person pane]} (person-terminal! root world witness)
          listed #(filter (fn [e] (= "shared-terminal" (:kind e))) (field world wid :world/unsafe))]
      (is (:ok (share! person pane agent "read" 30)))
      (is (= [(str pane)] (map :principal (listed))) "the indicator arms")
      (is (= [(str agent)] (map :agent (listed))))
      (testing "the indicator's OFF ends it"
        (is (:ok (act! person "approval.unsafe.off" {:principal (str pane)})))
        (is (empty? (listed)))
        (is (= :unresolved (code (act! root "pane.read" {:pane (str pane)})))))
      (testing "pane.unshare ends it"
        (is (:ok (share! person pane agent "read" 30)))
        (is (:ok (act! person "pane.unshare" {:pane (str pane) :principal (str agent)})))
        (is (empty? (listed)))
        (is (= :unresolved (code (act! root "pane.read" {:pane (str pane)})))))
      (testing "time ends it, at use and in the sweep"
        (is (:ok (share! person pane agent "read" 30)))
        (let [later (Date/from (.plus (Instant/now) (Duration/ofMinutes 31)))]
          (is (some? (resolve/resolves? (act/db world) agent (str "/" pane) :read)))
          (is (nil? (resolve/resolves? (act/db world) agent (str "/" pane) :read later)) "at use")
          (is (= [pane] (map :principal (unsafe/expire! world later))))
          (is (empty? (listed)) "the sweep")
          (is (nil? (field world pane :pane/shared)) "the Field is gone"))))))

(deftest only-a-person-shares-and-nothing-writes-it
  (fx/with-world [{:keys [world root witness]}]
    (let [agent (:principal root)
          {:keys [person id pane]} (person-terminal! root world witness)
          forged [{:principal (str agent) :mode "type" :until "2099-01-01T00:00:00Z" :by (str id) :at "2026-01-01T00:00:00Z"}]]
      (testing "a loop cannot share"
        (is (false? (:ok (loop-op world agent "pane.share" {:pane (str pane) :principal (str agent) :mode "type" :minutes 5})))))
      (testing "root's socket cannot"
        (is (false? (:ok (share! root pane agent "type" 5)))))
      (testing "neither Field can be written"
        (is (= :conflict (code (ws/request!! root :set-field {:object pane :field :pane/shared :type [:list :map]
                                                               :value forged :reader :both}))))
        (is (= :conflict (code (ws/request!! root :set-field {:object pane :field :pane/person :type :boolean
                                                               :value false :reader :both})))))
      (testing "a forged share that reached the log still fails: too long, so out of time"
        (log/append!! (:log world) (log/pending :field-set pane (:world-id world)
                                                {:name :pane/shared :type [:list :map] :value forged :reader :both}))
        (is (= :unresolved (code (act! root "pane.read" {:pane (str pane)})))))
      (testing "minutes are required and bounded"
        (is (false? (:ok (share! person pane agent "read" 481))))
        (is (false? (:ok (act! person "pane.share" {:pane (str pane) :principal (str agent) :mode "read"})))))
      (testing "one agent at a time, by name: there is no share with everyone"
        (is (false? (:ok (share! person pane "*" "read" 5))))))))

(deftest an-approved-card-types
  (fx/with-world [{:keys [world root witness]}]
    (let [agent (:principal root)
          {:keys [person pane]} (person-terminal! root world witness)
          _ (is (:ok (share! person pane agent "type" 30)))
          r (loop-op world agent "pane.send" {:pane (str pane) :text "echo typed-by-a-card\n"})
          task (get-in r [:err :task])]
      (is (= :approval-pending (code r)) (pr-str r))
      (is (some? task) (pr-str r))
      (let [decided (gates/approve! world (gates/first-person (act/db world)) {:task (parse-uuid (str task))})]
        (is (:ok decided) (pr-str decided)))
      (Thread/sleep 800)
      (is (re-find #"typed-by-a-card" (pr-str (result (act! person "pane.read" {:pane (str pane)}))))))))
