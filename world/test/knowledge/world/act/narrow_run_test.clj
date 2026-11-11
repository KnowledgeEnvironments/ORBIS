(ns knowledge.world.act.narrow-run-test
  "A Principal that holds :evaluate on a host Adapter can run a command there with nothing else
   (decisions 2026-10-30, found by the multi-project test): the Task is the kernel's record of the run,
   created under the Grant that admitted the run, not through schema.Task resolution."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(deftest a-narrow-principal-runs-its-shell
  (fx/with-world [{:keys [world root]}]
    (let [dir (fx/temp-dir)
          _ (spit (str dir "/hello.txt") "hi")
          p (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "agents.narrow"}}) [:result :object])
          token (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :person :name "agents.narrow.person"}}) [:result :result :token])
          shell (get-in (ws/request!! root :act {:class :adapter :action :create :args {:binds :host :locator dir :kind :shell :name "narrow.shell"}}) [:result :object])
          other (get-in (ws/request!! root :act {:class :adapter :action :create :args {:binds :host :locator dir :kind :shell :name "other.shell"}}) [:result :object])
          person (get-in (ws/request!! root :resolve {:name "agents.narrow.person" :op :read}) [:result :object])]
      (fx/grant! root person #{shell} #{:evaluate})
      (let [c (fx/attach-as world token)
            r (ws/request!! c :act {:object shell :action :run :args {:cmd "ls" :timeout-s 30}})]
        (is (:ok r) (pr-str r))
        (is (= 0 (get-in r [:result :result :exit])))
        (is (clojure.string/includes? (str (get-in r [:result :result :stdout])) "hello.txt"))
        (testing "the Task exists, owned by the runner, readable by its creator"
          (let [task (get-in r [:result :result :task])
                t (ws/request!! c :read {:object task})]
            (is (:ok t) (pr-str t))
            (is (= :task (get-in t [:result :object/class])))
            (is (= person (get-in t [:result :object/fields :task/owner :field/value])))
            (is (= :done (get-in t [:result :object/fields :task/state :field/value])))))
        (testing "without :evaluate on the Adapter the run is unresolved"
          (is (= :unresolved (get-in (ws/request!! c :act {:object other :action :run :args {:cmd "ls"}}) [:err :code]))))
        (ws/close! c))
      (fx/delete-tree! dir))))
