(ns knowledge.world.adapters.shell-test
  "The host shell Adapter (decisions 2026-10-30 'act run'): run! captures exit, stdout and stderr
   with caps and a timeout; through the world a run is a Task the Principal owns, moved
   open -> active -> done, readable by anyone with a Grant and refused to anyone without one."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.perform :as perform]
            [knowledge.world.adapters.shell :as shell]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(deftest a-run-s-cwd-takes-the-forms-a-model-is-told
  (let [dir (doto (java.io.File/createTempFile "orbis" "") .delete .mkdirs)
        home (.getCanonicalPath dir)
        sub (doto (java.io.File. dir "src") .mkdirs)
        at #(some-> (perform/run-dir home %) .getPath)]
    (is (= home (at nil)))
    (is (= (.getCanonicalPath sub) (at "src")))
    (is (= (.getCanonicalPath sub) (at (str home "/src"))) "absolute")
    (is (= home (at home)))
    (is (= (.getCanonicalPath sub) (at "mount-name/src")) "with the mount's name")
    (is (= home (at "mount-name")))
    (is (not (.exists (java.io.File. (at "src/nope/deeper")))) "a path that is not there stays not there")))

(deftest a-compiler-s-line-comes-first
  (is (= "a.c:1:2: error: nope\nxcodebuild: failed to find cache"
         (shell/failing-lines "xcodebuild: failed to find cache\na.c:1:2: error: nope"))))

(deftest run-captures-exit-and-streams
  (testing "exit code, both streams"
    (let [r (shell/run! {:cmd "echo hi; echo oops 1>&2; exit 3" :cwd "/tmp"})]
      (is (= 3 (:exit r)))
      (is (= "hi\n" (:stdout r)))
      (is (= "oops\n" (:stderr r)))
      (is (false? (:timed-out r)))))
  (testing "a command that does not exist fails with the shell's code"
    (is (= 127 (:exit (shell/run! {:cmd "definitely-not-a-command-xyz"})))))
  (testing "output is capped at 64 KiB but counted in full"
    (let [r (shell/run! {:cmd "head -c 200000 /dev/zero | tr '\\0' x"})]
      (is (= shell/output-cap (count (:stdout r))))
      (is (= 200000 (:stdout-total r)))))
  (testing "a timeout kills the command"
    (let [r (shell/run! {:cmd "sleep 10; echo late" :timeout-s 1})]
      (is (true? (:timed-out r)))
      (is (= -1 (:exit r)))
      (is (not (str/includes? (:stdout r) "late")))
      (is (< (:duration-ms r) 8000)))))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- v [r k] (get-in r [:result :object/fields k :field/value]))

(deftest a-run-is-a-task-on-the-log
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          shell-id (resolve-as root "shell.local")
          r (ws/request!! root :act {:object shell-id :action :run :args {:cmd "echo hello from $PWD; printf err 1>&2; exit 2"}})
          task (get-in r [:result :object])
          res (get-in r [:result :result])]
      (is (some? shell-id) "kernel-zero bound shell.local")
      (is (:ok r) (pr-str r))
      (is (= 2 (:exit res)))
      (is (str/starts-with? (:stdout res) "hello from /"))
      (is (= "err" (:stderr res)))
      (is (= "done" (:state res)))
      (testing "the Task carries the run as Fields and reached done through task-moved events on its stream and the owner's"
        (let [t (ws/request!! root :read {:object task})
              moves (filter #(= :task-moved (:event/kind %)) (get-in (ws/request!! root :history {:stream task}) [:result :events]))
              on-root (filter #(and (= :task-moved (:event/kind %)) (= task (get-in % [:event/body :task])))
                              (get-in (ws/request!! root :history {:stream root-id :limit 1000}) [:result :events]))]
          (is (= :task (get-in t [:result :object/class])))
          (is (= root-id (v t :task/owner)))
          (is (= :done (v t :task/state)))
          (is (= 2 (v t :task/exit)))
          (is (= "echo hello from $PWD; printf err 1>&2; exit 2" (v t :task/command)))
          (is (= shell-id (v t :task/adapter)))
          (is (str/starts-with? (v t :task/stdout) "hello from /"))
          (is (= [[:open :active] [:active :done]] (map (juxt #(get-in % [:event/body :from]) #(get-in % [:event/body :to])) moves)))
          (is (= 2 (count on-root)))))
      (testing "large output lands as a blob on the Task; the result carries the first 2 KiB"
        (let [r (ws/request!! root :act {:object shell-id :action :run :args {:cmd "head -c 30000 /dev/zero | tr '\\0' y"}})
              t (ws/request!! root :read {:object (get-in r [:result :object])})]
          (is (:ok r) (pr-str r))
          (is (map? (v t :task/stdout)) "a blob receipt")
          (is (= 30000 (v t :task/stdout-total)))
          (is (str/starts-with? (get-in r [:result :result :stdout]) (apply str (repeat 2048 "y"))))
          (is (str/includes? (get-in r [:result :result :stdout]) "more chars on the Task"))))
      (testing "a timeout cancels the Task"
        (let [r (ws/request!! root :act {:object shell-id :action :run :args {:cmd "sleep 10" :timeout-s 1}})
              t (ws/request!! root :read {:object (get-in r [:result :object])})]
          (is (:ok r) (pr-str r))
          (is (= "cancelled" (get-in r [:result :result :state])))
          (is (= :cancelled (v t :task/state)))
          (is (true? (v t :task/timed-out)))))
      (testing "a background run returns immediately, lists active, wakes once, and can be cancelled"
        (let [r (act! root "task.run" {:adapter "shell.local" :cmd "sleep 10" :timeout-s 20 :background true})
              task (get-in r [:result :result :task])]
          (is (:ok r) (pr-str r))
          (is (= "active" (get-in r [:result :result :state])))
          (is (some #{task} (map :task (get-in (act! root "task.list" {:state "active"}) [:result :result :tasks]))))
          (is (:ok (act! root "task.cancel" {:task (str task)})))
          (Thread/sleep 100)
          (is (= :cancelled (v (ws/request!! root :read {:object task}) :task/state)))))
      (testing "cwd must stay inside the Adapter's locator"
        (is (= :invalid (get-in (ws/request!! root :act {:object shell-id :action :run :args {:cmd "pwd" :cwd "../.."}}) [:err :code]))))
      (testing "a Principal without evaluate on the Adapter gets nothing, and cannot read the Task"
        (let [w (fx/attach-as world (:token witness))]
          (is (= :unresolved (get-in (ws/request!! w :act {:object shell-id :action :run :args {:cmd "id"}}) [:err :code])))
          (is (= :unresolved (get-in (ws/request!! w :read {:object task}) [:err :code])))
          (fx/grant! root (:id witness) #{task} #{:read})
          (is (= 2 (v (ws/request!! w :read {:object task}) :task/exit)))
          (ws/close! w)))
      (testing "move is an op: the owner reopens the Task"
        (let [m (ws/request!! root :move {:task task :to :open})]
          (is (:ok m) (pr-str m))
          (is (= :open (v (ws/request!! root :read {:object task}) :task/state))))))))

(deftest task-output-pages-with-range
  ;; `read {object field range}` pages any text or bytes Field, a Task's stdout included
  (fx/with-world [{:keys [root]}]
    (let [shell (get-in (ws/request!! root :resolve {:name "shell.local" :op :read}) [:result :object])
          r (ws/request!! root :act {:object shell :action :run :args {:cmd "seq 1 3000"}})
          task (get-in r [:result :object])
          page (ws/request!! root :read {:object task :field :task/stdout :range [1 5]})
          f (fn [res k] (get-in res [:result :object/fields k :field/value]))]
      (is (:ok r) (pr-str r))
      (is (> (get-in r [:result :result :stdout-total]) 4096) "big enough to be a blob")
      (is (:ok page) (pr-str page))
      (is (= "1\n2\n3\n4\n5" (f page :task/stdout)))
      (is (= [1 5] (f page :task/stdout-range)))
      (is (= 3000 (f page :task/stdout-lines)))
      (is (nil? (f page :task/stdout-range-truncated)))
      (testing "the last page, and a whole read still returns the receipt"
        (let [tail (ws/request!! root :read {:object task :field :task/stdout :range [2998 3000]})]
          (is (= "2998\n2999\n3000" (f tail :task/stdout))))
        (let [whole (ws/request!! root :read {:object task :field :task/stdout})]
          (is (not (string? (f whole :task/stdout))) "unpaged: bytes or a receipt, as stored")))
      (testing "a short string Field pages too"
        (is (:ok (ws/request!! root :set-field {:object (:principal root) :field :notes/lines :type :string :value "a\nb\nc"})))
        (let [p2 (ws/request!! root :read {:object (:principal root) :field :notes/lines :range [2 3]})]
          (is (= "b\nc" (f p2 :notes/lines)))
          (is (= 3 (f p2 :notes/lines-lines))))))))

