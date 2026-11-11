(ns knowledge.world.resolve.core-test
  "Two principals with different Grants resolve differently for the same name (spec 6)."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.index.core :as index]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.ws-client :as ws]))

(defn- create! [root class args]
  (let [r (ws/request!! root :act {:class class :action :create :args args})]
    (is (:ok r) (pr-str r))
    [(get-in r [:result :object]) (get-in r [:result :result :token])]))

(deftest different-grants-different-resolution
  (fx/with-world [{:keys [world root]}]
    (let [[ada ada-token] (create! root :principal {:driven-by :person :name "ada"})
          [bob bob-token] (create! root :principal {:driven-by :person :name "bob"})
          [team] (create! root :group {:kind :team :members #{bob}})
          [doc] (create! root :object {:fields {:title "plan"}})
          world-ns (:world-id world)
          bind (ws/request!! root :bind {:namespace world-ns :name "docs.plan" :object doc})
          _ (is (:ok bind) (pr-str bind))
          g-ada (fx/grant! root ada #{doc} #{:read :write})
          g-team (fx/grant! root team {:prefix "docs"} #{:read})
          db (index/db (:index world))]
      (testing "the predicate"
        (is (= g-ada (resolve/resolves? db ada "docs.plan" :read)))
        (is (= g-ada (resolve/resolves? db ada "docs.plan" :write)))
        (is (= g-team (resolve/resolves? db bob "docs.plan" :read)) "bob resolves through his group's prefix grant")
        (is (nil? (resolve/resolves? db bob "docs.plan" :write)))
        (is (nil? (resolve/resolves? db ada "docs.missing" :read)) "a miss is nothing")
        (is (nil? (resolve/resolves? db bob "docs.plan" :delegate)))
        (is (some? (resolve/resolves? db (:principal root) "docs.plan" :delegate)) "root resolves everything"))
      (testing "through the ops"
        (let [a (fx/attach-as world ada-token)
              b (fx/attach-as world bob-token)]
          (is (:ok (ws/request!! a :read {:object doc})))
          (is (:ok (ws/request!! b :read {:object doc})))
          (is (= :unresolved (get-in (ws/request!! b :set-field {:object doc :field :title :type :string :value "x"}) [:err :code])))
          (is (:ok (ws/request!! a :set-field {:object doc :field :title :type :string :value "x"})))
          (testing "revocation ends it at the next resolution"
            (is (:ok (ws/request!! root :revoke {:grant g-team})))
            (is (= :unresolved (get-in (ws/request!! b :read {:object doc}) [:err :code]))))
          (testing "a successful resolution is an event on both streams"
            (let [h (ws/request!! root :history {:stream doc :limit 500})
                  resolved (filter #(= :resolved (:event/kind %)) (get-in h [:result :events]))]
              (is (some #(= ada (:event/actor %)) resolved))
              (is (every? #(= doc (get-in % [:event/body :object])) resolved))))
          (ws/close! a) (ws/close! b))))))

(deftest a-recipient-reads-its-message
  ;; decisions 2026-10-30 'a recipient reads its Message': delivery is access for :read, and only for
  ;; the recipients; a Message sent to someone else stays unresolved
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (get-in (ws/request!! root :resolve {:name "root.route" :op :read}) [:result :object])
          w (fx/attach-as world (:token witness))
          to-witness (get-in (ws/request!! root :send {:to #{(:id witness)} :route route :text "for you"}) [:result :message])
          to-root (get-in (ws/request!! root :send {:to #{root-id} :route route :text "for me"}) [:result :message])]
      (is (uuid? to-witness))
      (testing "the witness holds no Grant on the Message, yet reads it because it is a recipient"
        (let [r (ws/request!! w :read {:object to-witness})]
          (is (:ok r) (pr-str r))
          (is (= "for you" (get-in r [:result :object/fields :message/text :field/value])))))
      (testing "a Message to someone else does not resolve, and a recipient may not write its Message"
        (is (= :unresolved (get-in (ws/request!! w :read {:object to-root}) [:err :code])))
        (is (= :unresolved (get-in (ws/request!! w :set-field {:object to-witness :field :message/text :type :string :value "x"}) [:err :code]))))
      (ws/close! w))))
