(ns knowledge.world.release-test
  "world.restart, without the process going anywhere: the gate, the refusals, the request the
   supervisor reads and the report when it comes back (decisions 2026-10-31). The whole loop, a real
   supervisor and a real restart, is scripts/test-selfhost.sh."
  (:require [clojure.java.io :as io]
            [clojure.java.shell :as sh]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.segment :as segment]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.release :as release]
            [knowledge.world.ws-client :as ws]))

(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- restart-op [args] {:op :act :args {:action :world.restart :args args}})
(def running-commit "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
(def next-commit "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
(def old-commit "cccccccccccccccccccccccccccccccccccccccc")

(defn- release-dir! [data commit log-version]
  (let [d (io/file data "releases" commit)]
    (.mkdirs d)
    (spit (io/file d "release.edn") (pr-str {:commit commit :version "test" :log-version log-version}))
    d))

(defn- supervised
  "The world as a supervisor would have started it: on running-commit, with two more releases."
  [world exits]
  (let [data (:data-dir world)
        here (release-dir! data running-commit segment/version)
        _ (release-dir! data next-commit segment/version)
        _ (release-dir! data old-commit (dec segment/version))
        w (assoc world :config (assoc (:config world) :release running-commit :harness-root (.getPath here)
                                      :on-restart #(swap! exits conj %)))]
    (release/boot! w)
    w))

(deftest a-world-with-no-supervisor-refuses
  (fx/with-world [{:keys [world root]}]
    (is (= :unsupervised (code (first (lops/execute! world (:principal root) nil [(restart-op {:reason "x"})])))))
    (is (= :unsupervised (code (ws/request!! root :act {:action :world.restart :args {:reason "x"}}))))))

(deftest a-supervised-world-says-what-it-runs
  (fx/with-world [{:keys [world]}]
    (let [w (supervised world (atom []))]
      (is (= running-commit (:commit (field w (:world-id w) :world/release))))
      (is (= (.getCanonicalPath (io/file (:data-dir w) "releases" running-commit))
             (field w (objects/lookup (act/db w) (:world-id w) "harness") :adapter/locator))))))

(deftest refusals-come-before-the-gate
  (fx/with-world [{:keys [world root]}]
    (let [w (supervised world (atom []))
          id (:principal root)
          tasks #(count (objects/objects-of-class (act/db w) :task))
          before (tasks)]
      (is (= :unresolved (code (first (lops/execute! w id nil [(restart-op {:reason "x" :release "dddddd"})])))))
      (is (= :log-version (code (first (lops/execute! w id nil [(restart-op {:reason "x" :release old-commit})])))))
      (is (= before (tasks)) "no card for something that cannot run"))))

(deftest restarting-as-it-is-is-an-ordinary-gate
  (fx/with-world [{:keys [world root]}]
    (let [exits (atom [])
          w (supervised world exits)
          id (:principal root)]
      (testing "a card by default"
        (is (= :approval-pending (code (first (lops/execute! w id nil [(restart-op {:reason "to pick up a Field"})])))))
        (is (empty? @exits)))
      (testing "unsafe mode may admit it"
        (is (not (authority/must-card? (act/db w) :act {:action "world.restart" :args {:reason "x"}})))
        (is (not (authority/must-card? (act/db w) :act {:action "world.restart" :args {:reason "x" :release (subs running-commit 0 8)}})))))))

(deftest a-new-kernel-is-always-a-persons-own-card
  (fx/with-world [{:keys [world root witness]}]
    (let [exits (atom [])
          w (supervised world exits)
          id (:principal root)
          op (restart-op {:reason "new kernel" :release next-commit})]
      (is (authority/must-card? (act/db w) :act (:args op)))
      (testing "no rule and no unsafe mode admits it"
        (with-redefs [unsafe/live? (constantly true)]
          (is (nil? (gates/admission (act/db w) id :act (:args op) nil)))))
      (testing "from a loop it is a card, from a process's socket it is refused"
        (is (= :approval-pending (code (first (lops/execute! w id nil [op])))))
        (is (= :person-only (code (actions/handle! :world.restart w id {:reason "x" :release next-commit} {:session? true}))))
        (is (empty? @exits)))
      (testing "a person's own hand runs it"
        (let [person (gates/first-person (act/db w))
              t (first (filter #(= "approval" (field w % :task/kind)) (objects/objects-of-class (act/db w) :task)))]
          (testing "but never in a batch"
            (let [r (gates/decide-many! w person {:tasks [(str t)] :decision "approve"})]
              (is (= 0 (:decided r)) (pr-str r))
              (is (empty? @exits))))
          (let [r (gates/approve! w person {:task t})]
            (is (not (:err r)) (pr-str r))
            (is (= [release/restart-code] @exits))
            (let [req (slurp (io/file (:data-dir w) "restart.request"))]
              (is (str/includes? req (str "release=" next-commit)))
              (is (str/includes? req "reason=new kernel")))
            (is (= next-commit (:to (field w (:world-id w) :world/restart))))))))))

(deftest no-principal-forges-the-running-release
  (fx/with-world [{:keys [world root]}]
    (let [w (supervised world (atom []))
          wid (:world-id w)
          [r] (lops/execute! w (:principal root) nil
                             [{:op :set-field :args {:object wid :field :world/release :type :map :value {:commit next-commit} :reader :both}}])]
      (is (= :conflict (code r)) (pr-str r))
      (is (= running-commit (:commit (field w wid :world/release)))))))

(deftest a-resumed-wake-is-answered-not-restarted
  (fx/with-world [{:keys [world root]}]
    (let [exits (atom [])
          w (supervised world exits)
          m (random-uuid)
          first-go (release/request! w (:principal root) {:reason "once"} {:message m})
          again (release/request! w (:principal root) {:reason "once"} {:message m})]
      (is (get-in first-go [:ok :result :restarting]))
      (is (get-in again [:ok :result :already]))
      (is (= 1 (count @exits))))))

(deftest the-world-that-comes-back-reports
  (fx/with-world [{:keys [world]}]
    (let [w (supervised world (atom []))
          f (io/file (:data-dir w) "restart.outcome")
          person (gates/first-person (act/db w))]
      (spit f (str "result=rolled-back\nfrom=" next-commit "\nto=" running-commit "\nreason=health check failed\n"))
      (release/boot! w)
      (is (not (.exists f)) "reported once")
      (let [rows (get-in (:ok ((requiring-resolve 'knowledge.world.notify/list-of) w person {})) [:result :rows])]
        (is (= "urgent" (:level (first rows))))
        (is (str/includes? (:text (first rows)) "health check failed"))
        (is (str/includes? (:text (first rows)) (subs next-commit 0 12))))
      (let [status (filter #(= :status (some-> (field w % :message/kind) name keyword))
                           (objects/objects-of-class (act/db w) :message))]
        (is (some #(str/includes? (str (field w % :message/text)) "health check failed") status))))))

(deftest a-release-behind-the-running-code-is-refused
  (let [data (doto (java.io.File/createTempFile "orbis-rel" "") .delete .mkdirs)
        src (doto (io/file data "src") .mkdirs)
        git (fn [& args] (let [r (apply sh/sh "git" (concat args [:dir src]))] (assert (zero? (:exit r)) (pr-str r)) (str/trim (:out r))))
        commit (fn [m] (git "-c" "user.name=t" "-c" "user.email=t@t" "commit" "-q" "--allow-empty" "-m" m) (git "rev-parse" "HEAD"))]
    (git "init" "-q" "-b" "work")
    (let [old (commit "one")
          newer (commit "two")
          world {:data-dir (.getCanonicalPath data) :config {:release newer}}
          err (release/make! world {:rev old})]
      (is (= :conflict (get-in err [:err :code])) (pr-str err))
      (is (str/includes? (str (get-in err [:err :message])) "git merge origin/master"))
      (testing "a commit that has the running code in it is not refused for that"
        (is (not= :conflict (get-in (release/make! world {:rev newer}) [:err :code])))))))

