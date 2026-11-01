(ns knowledge.world.boot.first-person-test
  "A person never needs setup in a new world (decisions 2026-10-31): the first person is born with a
   Route to root and an owner Grant short of :delegate, and sits on the Route of every Principal
   created after it. Fresh worlds only: upgrade! leaves an existing world's Grants alone."
  (:require [clojure.java.io :as io]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- granted-to [c person]
  (->> (get-in (ws/request!! c :history {:stream person :limit 1000}) [:result :events])
       (filter #(= :granted (:event/kind %)))))

(deftest the-first-person-needs-no-setup
  (fx/with-world [{:keys [world dir root]}]
    (let [root-id (:principal root)
          token (slurp (io/file dir "person.token"))
          p (fx/attach-as world token)
          person (:principal p)]
      (is (= person (gates/first-person (act/db world))))
      (testing "the Route to root exists, both ends on it"
        (let [route (resolve-as p "root.person" :read)]
          (is (some? route))
          (is (= #{root-id person} (set (field world route :route/members))))
          (is (contains? (set (field world person :principal/routes)) route))
          (is (:ok (ws/request!! p :send {:to #{root-id} :route route :text "/fast hello"})))))
      (testing "the names a person needs resolve: root's namespace is on nobody else's chain"
        (doseq [n ["root.model" "root.workspace" "root.route" "harness" "shell.local"]]
          (is (some? (resolve-as p n :read)) n)))
      (testing "the owner's Grant is the world's: a person does not descend from root"
        (let [g (first (filter #(= :everything (:scope (:event/body %))) (granted-to root person)))]
          (is (= (:world-id world) (:event/actor g)))))
      (testing "the owner Grant: everything, every op, delegable: a person makes Principals without root"
        (doseq [op [:read :reference :transmit :write :evaluate :create :delegate]]
          (is (some? (resolve-as p "root" op)) (str op)))
        (is (:ok (ws/request!! p :grant {:grantee person :scope #{root-id} :ops #{:read}}))))
      (testing "a Principal created later has the person on its Route"
        (let [r (ws/request!! root :act {:action :principal.create
                                         :args {:name "later" :charter "A later Principal."
                                                :grant {:scope {:prefix "later"} :ops ["read"]}}} 20000)
              {:keys [principal route]} (get-in r [:result :result])]
          (is (:ok r) (pr-str r))
          (is (contains? (set (field world route :route/members)) person))
          (is (contains? (set (field world person :principal/routes)) route))
          (is (:ok (ws/request!! p :send {:to #{principal} :route route :text "/fast hello"})))))
      (ws/close! p))))

(deftest an-existing-world-keeps-its-grants
  (fx/with-world [{:keys [world dir root]}]
    (let [p (fx/attach-as world (slurp (io/file dir "person.token")))
          person (:principal p)
          before (count (granted-to root person))]
      (is (not-any? #{:first-person} (map first kz/later-steps)) "the owner Grant is no later step")
      (kz/upgrade! world)
      (is (= before (count (granted-to root person))) "upgrade! issues the person nothing")
      (ws/close! p))))
