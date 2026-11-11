(ns knowledge.world.act.fork-history-test
  "A forked world through the protocol: it boots without re-running kernel-zero,
   mints its own tokens, marks itself a fork, answers `history since-fork` and the action
   `world.diff` with only its own events, and a Principal created in the parent after the fork is
   invisible to it."
  (:require [clojure.java.io :as io]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.act.core :as act]
            [knowledge.world.system :as system]
            [knowledge.world.tools :as tools]
            [knowledge.world.ws-client :as ws]))

(defn- create! [c class args] (ws/request!! c :act {:class class :action :create :args args}))

(deftest a-forked-world-is-the-same-world-with-its-own-future
  (let [pdir (fx/temp-dir)
        parent (fx/start-world pdir 0)
        proot (fx/attach-root parent)
        before (get-in (create! proot :object {:fields {:n 1 :name "before-fork"}}) [:result :object])
        root-id (:principal proot)
        child-dir (io/file (fx/temp-dir) "child")]
    (try
      (let [{:keys [at]} (tools/fork! pdir child-dir)
            ;; the parent goes on: an Object the child must never see
            after (get-in (create! proot :object {:fields {:n 2 :name "after-fork"}}) [:result :object])
            child (system/start (fx/config (str child-dir) 0))]
        (try
          (testing "the child boots as the same world: same id, root is root, no second kernel-zero"
            (is (= (:world-id parent) (:world-id child)))
            (is (= root-id (objects/lookup (act/db child) (:world-id child) "root")))
            (is (= (count (objects/objects-of-class (act/db parent) :principal))
                   (count (objects/objects-of-class (act/db child) :principal)))
                "no second kernel-zero: the same Principals, not a new root")
            (is (.exists (io/file child-dir "root.token")) "a token of its own")
            (is (not= (slurp (io/file pdir "root.token")) (slurp (io/file child-dir "root.token"))) "not the parent's"))
          (let [croot (fx/attach-root child)]
            (try
              (testing "the fork is marked on the world Principal"
                (let [f (:object/fields (get-in (ws/request!! croot :read {:object (:world-id child)}) [:result]))]
                  (is (= at (get-in f [:world/forked-at :field/value])))
                  (is (string? (get-in f [:world/fork-id :field/value])))))
              (testing "what the parent did before the fork is here; what it did after is not"
                (is (:ok (ws/request!! croot :read {:object before})))
                (is (= :unresolved (get-in (ws/request!! croot :read {:object after}) [:err :code]))))
              (testing "since-fork lists only the child's own events, with a summary"
                (let [mine (get-in (create! croot :object {:fields {:n 3 :name "in-child"}}) [:result :object])
                      r (ws/request!! croot :history {:since-fork true})
                      res (:result r)]
                  (is (:ok r) (pr-str r))
                  (is (= at (get-in res [:fork :forked-at])))
                  (is (every? #(> (:event/seq %) at) (:events res)))
                  (is (some #(= mine (:event/stream %)) (:events res)))
                  (is (pos? (get-in res [:summary :events])))
                  (is (some #(= mine (:object %)) (get-in res [:summary :objects-created])))
                  (testing "the action world.diff answers the same for a loop"
                    (let [a (ws/request!! croot :act {:action :world.diff :args {}})]
                      (is (:ok a) (pr-str a))
                      (is (= at (get-in a [:result :result :fork :forked-at])))))))
              (testing "the parent never saw the child's Object"
                (is (= :unresolved (get-in (ws/request!! proot :read {:name "in-child"}) [:err :code]))))
              (finally (ws/close! croot))))
          (finally (system/stop child))))
      (testing "a world that is not a fork answers conflict"
        (is (= :conflict (get-in (ws/request!! proot :history {:since-fork true}) [:err :code]))))
      (finally
        (ws/close! proot)
        (system/stop parent)
        (fx/delete-tree! pdir)
        (fx/delete-tree! (.getParentFile child-dir))))))
