(ns knowledge.world.act.authority-test
  "What a Principal is cannot be written by the Principal, and write on an Object does not reach it
   (decisions 2026-10-31). Each case here was a live escalation before kernel-fields."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.ws-client :as ws]))

(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- driven [world id] (:principal/driven-by (objects/entity (act/db world) id)))
(defn- sf [obj k t v] {:op :set-field :args {:object obj :field k :type t :value v :reader :both}})
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))

(defn- process! [root n]
  (let [r (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name n}})
        id (get-in r [:result :object])]
    (fx/grant! root id #{id} #{:read :write})
    {:id id :token (get-in r [:result :result :token])}))

(deftest a-principal-cannot-become-a-person
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          {:keys [id token]} (process! root "narrow")
          gate (field world id :policy/approval)]
      (testing "from its loop and its own session, every spelling"
        (doseq [k [:principal/driven-by "principal/driven-by" ":principal/driven-by"]]
          (let [[r] (lops/execute! world id nil [(sf id k :keyword :person)])]
            (is (false? (:ok r)) (pr-str k r))
            (is (= :conflict (code r))))
          ;; the socket decodes the Field name as a keyword, so a stray string stops at the schema
          (when token
            (is (false? (:ok (ws/request!! (fx/attach-as world token) :set-field
                                           (:args (sf id k :keyword :person))))))))
        (is (= :process (driven world id))))
      (testing "root neither, on itself or another"
        (doseq [t [root-id id]]
          (is (= :conflict (code (ws/request!! root :set-field (:args (sf t :principal/driven-by :keyword :person)))))))
        (is (= :process (driven world root-id))))
      (testing "so its own session still cannot move its gate"
        (when token
          (is (= :person-only (code (ws/request!! (fx/attach-as world token) :set-field
                                                  (:args (sf id :policy/approval [:list :string] [])))))))
        (doseq [k [:policy/approval "policy/approval" ":policy/approval"]]
          (is (= :person-only (code (first (lops/execute! world id nil [(sf id k [:list :string] [])])))) (pr-str k)))
        (is (= gate (field world id :policy/approval)))))))

(deftest a-credential-is-not-a-field
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          before (field world (:id witness) :principal/token-hash)
          t (session/new-token)
          [r] (lops/execute! world root-id nil [(sf (:id witness) :principal/token-hash :string (session/token-hash t))])]
      (is (false? (:ok r)) (pr-str r))
      (is (= :conflict (code r)))
      (is (= before (field world (:id witness) :principal/token-hash)))
      (is (thrown? Throwable (fx/attach-as world t)) "the planted token does not attach"))))

(deftest a-namespace-is-not-a-field
  (fx/with-world [{:keys [world root]}]
    (let [{:keys [id]} (process! root "narrow")
          [r] (lops/execute! world id nil [(sf id :principal/namespace :ref (:principal root))])]
      (is (= :conflict (code r))))))

(deftest ordinary-fields-still-write
  (fx/with-world [{:keys [world root]}]
    (let [{:keys [id]} (process! root "narrow")
          [r] (lops/execute! world id nil [(sf id "instructions/own" :string "mine")])]
      (is (:ok r) (pr-str r))
      (is (= "mine" (field world id :instructions/own))))))

(deftest reach-stops-at-scope
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          {:keys [id]} (process! root "narrow")
          {other :id} (process! root "other")]
      (testing "root configures another"
        (is (:ok (first (lops/execute! world root-id nil [(sf id :instructions/own :string "from root")])))))
      (testing "the reverse does not resolve"
        (is (= :unresolved (code (first (lops/execute! world id nil [(sf root-id :instructions/own :string "x")])))))
        (is (= :unresolved (code (first (lops/execute! world id nil [(sf other :instructions/own :string "x")])))))
        ;; through the loop a grant is gated first; what a person would approve still does not resolve
        (is (= :unresolved (code (act/grant! world id {:grantee other :scope #{id} :ops #{:read}}))))))))

(deftest what-a-person-has-seen-is-theirs
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          person (:id witness)
          k :view/seen
          value {:root 5}]
      (testing "no agent marks a person's turn read, root included, on any path"
        (let [[r] (lops/execute! world root-id nil [(sf person k :map value)])]
          (is (= :person-only (code r)) (pr-str r)))
        (is (= :person-only (code (ws/request!! root :set-field (:args (sf person k :map value)))))))
      (testing "the person's own session may"
        (fx/grant! root person #{person} #{:read :write})
        (let [w (fx/attach-as world (:token witness))
              r (ws/request!! w :set-field (:args (sf person k :map value)))]
          (is (:ok r) (pr-str r))
          (is (some? (field world person k))))))))
