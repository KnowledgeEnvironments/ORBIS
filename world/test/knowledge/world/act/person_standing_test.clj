(ns knowledge.world.act.person-standing-test
  "A person's Grant goes only by a person's hand, and a card that cannot run is never raised
   (decisions 2026-10-31)."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws]))

(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- resolved [root n] (get-in (ws/request!! root :resolve {:name n :op :read}) [:result :object]))

(defn- person! [world root witness targets]
  (doseq [a ["task.approve" "task.decide-many" "approval.rule.add"]]
    (fx/grant! root (:id witness) #{(resolved root (str "action/" a))} #{:read :evaluate}))
  (fx/grant! root (:id witness) (set targets) #{:read :write})
  (fx/grant! root (:id witness) #{(:id witness)} #{:read})
  (fx/grant! root (:id witness) #{(resolved root "root.route")} #{:transmit})
  (fx/attach-as world (:token witness)))

(defn- pending [world requester]
  (let [db (act/db world)]
    (filter #(and (= "approval" (field world % :task/kind)) (= requester (field world % :task/requested-by))
                  (= :pending (some-> (field world % :task/state) name keyword)))
            (objects/objects-of-class db :task))))

(deftest root-cannot-take-a-persons-grant-without-a-card
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          g (get-in (ws/request!! root :grant {:grantee (:id witness) :scope #{root-id} :ops #{:read}}) [:result :grant])
          revoked? #(boolean (:grant/revoked (objects/state (act/db world) g)))]
      (testing "its socket session is refused"
        (is (= :person-only (code (ws/request!! root :revoke {:grant g}))))
        (is (not (revoked?))))
      (testing "its loop raises a card, not a revoke"
        (let [[r] (lops/execute! world root-id nil [{:op :revoke :args {:grant (str g)}}])]
          (is (false? (:ok r)) (pr-str r))
          (is (= :approval-pending (code r)) (pr-str r))
          (is (not (revoked?)))))
      (testing "a person's own session may"
        (fx/grant! root (:id witness) #{g} #{:delegate})
        (let [w (fx/attach-as world (:token witness))]
          (is (:ok (ws/request!! w :revoke {:grant g})))
          (is (revoked?)))))))

(deftest a-grant-between-agents-stays-as-it-was
  (fx/with-world [{:keys [world root]}]
    (let [a (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "a"}}) [:result :object])
          g (get-in (ws/request!! root :grant {:grantee a :scope #{a} :ops #{:read}}) [:result :grant])]
      (is (not (authority/person-grant? (act/db world) g)))
      (is (:ok (ws/request!! root :revoke {:grant g}))))))

(deftest no-rule-admits-a-persons-grant
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          g (get-in (ws/request!! root :grant {:grantee (:id witness) :scope #{root-id} :ops #{:read}}) [:result :grant])
          w (person! world root witness [root-id])]
      (is (:ok (ws/request!! w :act {:action :approval.rule.add
                                     :args {:principal (str root-id) :action "revoke:person"}})))
      (is (= :revoke:person (keyword (gates/gated? (act/db world) root-id :revoke {:grant (str g)}))))
      (is (nil? (gates/admission (act/db world) root-id :revoke {:grant (str g)} nil)) "a rule never reaches it"))))

(deftest a-grant-that-cannot-resolve-raises-no-card
  (fx/with-world [{:keys [world root witness]}]
    (let [n (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "n"}}) [:result :object])
          _ (fx/grant! root n #{n} #{:read :write})
          before (count (objects/objects-of-class (act/db world) :task))
          [r] (lops/execute! world n nil [{:op :grant :args {:grantee (:id witness) :scope :everything :ops #{:read}}}])]
      (is (= :unresolved (code r)) (pr-str r))
      (is (= before (count (objects/objects-of-class (act/db world) :task))) "no card")
      (testing "a Grant root can make still waits for its person"
        (let [[r] (lops/execute! world (:principal root) nil
                                 [{:op :grant :args {:grantee n :scope #{n} :ops #{:read}}}])]
          (is (= :approval-pending (code r)) (pr-str r)))))))
