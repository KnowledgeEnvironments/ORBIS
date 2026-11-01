(ns knowledge.world.act.create-test
  "act create for model, adapter and route (spec 5.6, 5.7, 5.10; decisions 2026-10-30)."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- create! [c class args] (ws/request!! c :act {:class class :action :create :args args}))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))

(deftest create-model-adapter-route
  (fx/with-world [{:keys [world root witness]}]
    (testing "a Model never stores a key, only the env var name"
      (let [r (create! root :model {:kind :model :format :openai-chat :residency :tenant
                                     :base-url "http://example.test/v1" :model-id "m" :key-env "KE_MODEL_KEY" :name "models.m"})
            id (get-in r [:result :object])
            fs (fields root id)]
        (is (:ok r) (pr-str r))
        (is (= :model (get-in (ws/request!! root :read {:object id}) [:result :object/class])))
        (is (= :model (v fs :model/kind)))
        (is (= :tenant (v fs :model/residency)))
        (is (= "http://example.test/v1" (v fs :model/base-url)))
        (is (= "KE_MODEL_KEY" (v fs :model/key-env)))
        (is (not-any? #(re-find #"key$" (name %)) (remove #{:model/key-env} (keys fs))) "no key Field")
        (is (= id (get-in (ws/request!! root :resolve {:name "models.m" :op :read}) [:result :object])))))
    (testing "an Adapter of kind transport and a Route on it, with a reader :agent template"
      (let [a (get-in (create! root :adapter {:binds :transport :locator "irc://example" :sync :both}) [:result :object])
            r (create! root :route {:transport a :address "#ops" :template "You are terse." :input-tags [a] :name "routes.ops"})
            route (get-in r [:result :object])
            fs (fields root route)]
        (is (uuid? a))
        (is (:ok r) (pr-str r))
        (is (= :transport (v (fields root a) :adapter/binds)))
        (is (= a (v fs :route/transport)))
        (is (= "#ops" (v fs :route/address)))
        (is (= [a] (v fs :route/input-tags)))
        (is (= :agent (get-in fs [:route/template :field/reader])))
        (is (= "You are terse." (v fs :route/template)))))
    (testing "without :create on the class schema the witness gets nothing"
      (let [w (fx/attach-as world (:token witness))]
        (is (= :unresolved (get-in (create! w :model {:kind :model}) [:err :code])))
        (is (= :unresolved (get-in (create! w :route {:address "x"}) [:err :code])))
        (ws/close! w)))
    (testing "still unbuilt classes (a Trigger is creatable since )"
      (is (= :not-implemented (get-in (create! root :serialization {}) [:err :code]))))))

(deftest route-members-are-explicit-and-maintained
  ;; route/members from members/to at creation, then kept in step with principal/routes
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          wid (:id witness)
          a (get-in (create! root :adapter {:binds :transport :locator "irc://example" :sync :both}) [:result :object])
          r (create! root :route {:transport a :address "#two" :members [root-id wid] :name "routes.two"})
          route (get-in r [:result :object])
          members #(set (v (fields root route) :route/members))
          root-route (get-in (ws/request!! root :resolve {:name "root.route" :op :read}) [:result :object])]
      (is (:ok r) (pr-str r))
      (is (= #{root-id wid} (members)) "members from the create args")
      (testing "root.route knows root, from kernel-zero's write to principal/routes"
        (is (= #{root-id} (set (v (fields root root-route) :route/members)))))
      (testing "a Principal adding the Route to its principal/routes joins; removing it leaves"
        (fx/grant! root wid #{wid} #{:read :write})
        (let [w (fx/attach-as world (:token witness))
              other (get-in (create! root :route {:transport a :address "#three" :name "routes.three"}) [:result :object])]
          (is (= [] (v (fields root other) :route/members)) "no members until someone routes on it")
          (is (:ok (ws/request!! w :set-field {:object wid :field :principal/routes :type [:list [:ref :route]] :value [other]})))
          (is (= [wid] (v (fields root other) :route/members)))
          (is (:ok (ws/request!! w :set-field {:object wid :field :principal/routes :type [:list [:ref :route]] :value []})))
          (is (= [] (v (fields root other) :route/members)))
          (is (= #{root-id wid} (members)) "an unrelated Route is untouched")
          (testing "the maintained write is on the log with the writer as actor and previous set"
            (let [evs (get-in (ws/request!! root :history {:stream other}) [:result :events])
                  fs (filter #(and (= :field-set (:event/kind %)) (= :route/members (get-in % [:event/body :name]))) evs)]
              (is (= 2 (count fs)))
              (is (every? #(= wid (:event/actor %)) fs))
              (is (= [wid] (get-in (last fs) [:event/body :previous])))))
          (ws/close! w))))))

