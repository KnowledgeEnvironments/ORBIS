(ns knowledge.world.authority-test
  "Who may do what, as one table. docs/authority.md says it in words; this says it in the running world."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.authority :as authority]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws]))

(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- outcome [r] (if (:ok r) :ok (code r)))

(def writes
  "A Field, its type and value."
  {:plain [:instructions/own :string "mine"]
   :policy [:policy/approval [:list :string] []]
   :unsafe [:policy/unsafe :map {:on true}]
   :kernel [:principal/driven-by :keyword :person]
   :token [:principal/token-hash :string "x"]
   :gateway [:gateway/url :string "http://x"]
   :model [:principal/model-fallback :string "x"]
   :seen [:view/seen :map {}]})

(def table
  "What each kind of actor gets for each write."
  {:person {:plain :ok :policy :ok :unsafe :conflict :kernel :conflict :token :conflict :gateway :conflict :model :ok :seen :ok}
   :root-loop {:plain :ok :policy :person-only :unsafe :person-only :kernel :conflict :token :conflict :gateway :conflict :model :person-only :seen :person-only}
   :root-socket {:plain :ok :policy :person-only :unsafe :person-only :kernel :conflict :token :conflict :gateway :conflict :model :person-only :seen :person-only}
   :agent-loop {:plain :ok :policy :person-only :unsafe :person-only :kernel :conflict :token :conflict :gateway :conflict :model :person-only :seen :person-only}
   :agent-socket {:plain :ok :policy :person-only :unsafe :person-only :kernel :conflict :token :conflict :gateway :conflict :model :person-only :seen :person-only}})

(deftest who-may-write-what
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          r (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "narrow"}})
          narrow (get-in r [:result :object])
          token (get-in r [:result :result :token])
          _ (fx/grant! root narrow #{narrow} #{:read :write})
          _ (fx/grant! root (:id witness) #{narrow} #{:read :write})
          person (fx/attach-as world (:token witness))
          agent (fx/attach-as world token)
          args (fn [[k t v]] {:object narrow :field k :type t :value v :reader :both})
          op (fn [w] {:op :set-field :args (args w)})
          by {:person #(ws/request!! person :set-field (args %))
              :root-socket #(ws/request!! root :set-field (args %))
              :agent-socket #(ws/request!! agent :set-field (args %))
              :root-loop #(first (lops/execute! world root-id nil [(op %)]))
              :agent-loop #(first (lops/execute! world narrow nil [(op %)]))}]
      (doseq [[actor row] table
              [write expected] row]
        (testing (str actor " writing " write)
          (is (= expected (outcome ((by actor) (writes write))))))))))

(deftest a-field-is-the-same-field-however-it-is-spelled
  (doseq [f [:policy/approval "policy/approval" ":policy/approval"]]
    (is (= :policy/approval (authority/field-key f)))
    (is (= :person-only (code (authority/write-refusal f false))) (pr-str f))
    (is (nil? (authority/write-refusal f true)) (pr-str f)))
  (doseq [f [:principal/driven-by "principal/driven-by" ":principal/driven-by"]]
    (is (= :conflict (code (authority/write-refusal f true))) (pr-str f))))

(deftest what-the-kernel-relies-on-cannot-be-cleared
  (doseq [f [:principal/driven-by :principal/token-hash :world/release :adapter/locator :gateway/url
             :policy/unsafe :world/unsafe]]
    (is (= :conflict (code (authority/clear-refusal f true))) (pr-str f)))
  (is (= :person-only (code (authority/clear-refusal :policy/approval false))))
  (testing "clearing is a write: what stops one stops the other"
    (doseq [f [:principal/ephemeral :world/manual :adapter/read]]
      (is (= :conflict (code (authority/clear-refusal f true))) (pr-str f)))
    (doseq [f [:principal/model-choice :principal/model-fallback :conversation/model]]
      (is (= :person-only (code (authority/clear-refusal f false))) (pr-str f))
      (is (nil? (authority/clear-refusal f true)) (pr-str f))))
  (is (nil? (authority/clear-refusal :policy/approval true)))
  (is (nil? (authority/clear-refusal :notes/today false))))

(deftest a-person-is-a-session-not-a-name
  (fx/with-world [{:keys [world root witness]}]
    (let [db (act/db world)
          person (:id witness)
          root-id (:principal root)]
      (testing "a person's Principal, from a loop or an approved op, is not their session"
        (is (authority/person-session? db person true))
        (is (not (authority/person-session? db person false)))
        (is (not (authority/person-session? db root-id true))))
      (testing "require-person! throws person-only otherwise"
        (is (nil? (authority/require-person! db person {:session? true})))
        (doseq [[p opts] [[person {:session? false}] [root-id {:session? true}] [person {}]]]
          (is (= :person-only (get-in (try (authority/require-person! db p opts)
                                           (catch clojure.lang.ExceptionInfo e (ex-data e)))
                                      [:err :code])))))
      (testing "the session gate"
        (is (authority/passes-session-gate? db person {:session? true}))
        (is (authority/passes-session-gate? db root-id {:session? true}) "the operator's key: no token Object")
        (is (authority/passes-session-gate? db person {:scratch? true}))
        (is (not (authority/passes-session-gate? db person {:session? false :token (random-uuid)})))))))

(deftest what-no-rule-admits
  (fx/with-world [{:keys [world]}]
    (let [db (act/db world)]
      (is (authority/must-card? db :act {:action "adapter.unconfine"}))
      (is (authority/must-card? db :act {:action "world.restart" :args {:release "ffff"}}))
      (is (not (authority/must-card? db :act {:action "world.restart" :args {}})))
      (is (not (authority/must-card? db :act {:action "task.run"}))))))
