(ns knowledge.world.act.json-values-test
  "The JSON form carries ids as strings (ADR 0002); a Field set over it must land in the shape its
   declared type names, or a Principal's routes and models set by a JSON client (a loop, the listener)
   are strings the fold and the loop cannot match (decisions 2026-10-30, found by the multi-project test)."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(deftest coerce-value-by-declared-type
  (let [u "29038d56-9e93-4ebd-8384-cfd4eda40414"]
    (is (uuid? (act/coerce-value [:ref :route] u)))
    (is (every? uuid? (act/coerce-value [:list [:ref :route]] [u u])))
    (is (= {:a (parse-uuid u)} (act/coerce-value [:map [:ref :object]] {:a u})))
    (is (= :process (act/coerce-value :keyword "process")))
    (is (= 42 (act/coerce-value :number "42")))
    (is (true? (act/coerce-value :boolean "true")))
    (is (inst? (act/coerce-value :instant "2026-10-30T17:00:00Z")))
    (testing "values already in shape, or unparsable, pass through"
      (is (= (parse-uuid u) (act/coerce-value [:ref :route] (parse-uuid u))))
      (is (= "not an id" (act/coerce-value [:ref :route] "not an id")))
      (is (= "text" (act/coerce-value :string "text"))))))

(deftest a-json-client-sets-refs-that-the-world-can-follow
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          j (fx/attach-root world :json)
          route (get-in (ws/request!! root :resolve {:name "root.route" :op :read}) [:result :object])
          p (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "agents.json"}}) [:result :object])
          r (ws/request!! j :set-field {:object (str p) :field "principal/routes" :type ["list" ["ref" "route"]] :value [(str route)] :reader "both"})
          back (get-in (ws/request!! root :read {:object p}) [:result :object/fields :principal/routes :field/value])]
      (is (:ok r) (pr-str r))
      (is (= [route] back) "the ref came back as a UUID, not the string the JSON client sent")
      (is (uuid? (first back)))
      (ws/close! j))))
