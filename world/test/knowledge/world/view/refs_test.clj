(ns knowledge.world.view.refs-test
  "References in a view's title and text data are parsed by the loop and recorded on the view."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n op]
  (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))

(defn- fields [c id]
  (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))

(defn- value-of [fs k]
  (get-in fs [k :field/value]))

(deftest title-and-data-references-are-recorded-on-the-view
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          model-id (resolve-as root "root.model" :read)
          route (resolve-as root "root.route" :read)
          raw {:to #{(:id witness)}
               :route route
               :kind :text
               :title "About [[aa]]"
               :data {:markdown "Uses [[bb]]"}}
          {:keys [args err]} (lops/expand-args (act/db world) root-id
                                              {"aa" (str root-id) "bb" (str model-id)}
                                              :present raw)
          _ (is (nil? err) (pr-str err))
          presented (ws/request!! root :present args)
          _ (is (:ok presented) (pr-str presented))
          view-fields (fields root (get-in presented [:result :serialization]))
          title-refs (value-of view-fields :view/title/refs)
          data-refs (value-of view-fields :view/data/refs)]
      (testing "reference markup becomes display text"
        (is (= "About root" (value-of view-fields :view/title)))
        (is (= "Uses root.model" (get-in (value-of view-fields :view/data) [:markdown]))))
      (testing "the world records title and data spans separately"
        (is (= [root-id] (mapv :object title-refs)))
        (is (= [model-id] (mapv :object data-refs)))
        (is (= [{:start 6 :end 10 :kind :object :object root-id :name "root"}]
               title-refs))
        (is (= [{:start 5 :end 15 :kind :object :object model-id :name "root.model"}]
               data-refs))))))
