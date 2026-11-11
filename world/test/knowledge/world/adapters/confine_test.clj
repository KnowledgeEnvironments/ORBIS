(ns knowledge.world.adapters.confine-test
  "The box: each test tries a way out and asserts what happened, on the real sandbox of this host."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.adapters.confine :as confine]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.adapters.harness :as harness]
            [knowledge.world.adapters.shell :as shell]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.pane.core :as core]
            [knowledge.world.ws-client :as ws])
  (:import [java.util Date]))

(def canary "canary-b7e1")

(defn- world-of
  "A data dir with what a world keeps, a project beside it, and a directory that is neither."
  []
  (let [base (.getCanonicalPath (io/file (fx/temp-dir)))
        data (io/file base "data")
        root (io/file base "project")
        other (io/file base "other")]
    (doseq [d [(io/file data "log") root other]] (.mkdirs d))
    (spit (io/file data "person.token") canary)
    (spit (io/file data "root.token") canary)
    (spit (io/file data "world.env") (str "export KE_MODEL_KEY=" canary))
    (spit (io/file data "log" "seg") canary)
    (spit (io/file other "note") canary)
    (spit (io/file root ".envrc") (str "export KE_MODEL_KEY=" canary))
    (spit (io/file root "main.txt") "in the root")
    {:base base :data (.getPath data) :root (.getPath root) :other (.getPath other)
     :box (confine/spec {:root root :data-dir data :adapter (random-uuid) :ports [8000]})}))

(defn- run [{:keys [box root]} cmd]
  (shell/run! {:cmd cmd :cwd root :box box :timeout-s 60}))

(defn- leaked? [{:keys [stdout stderr]}] (str/includes? (str stdout stderr) canary))

(deftest the-box-holds
  (let [{:keys [data root other base] :as w} (world-of)]
    (testing "inside the root a command works"
      (let [r (run w "cat main.txt; echo made > made.txt; cat made.txt; echo $TMPDIR")]
        (is (zero? (:exit r)) (pr-str r))
        (is (str/includes? (:stdout r) "in the root"))
        (is (str/includes? (:stdout r) "made"))
        (is (str/includes? (:stdout r) "scratch"))))
    (testing "credentials, the log and the Model's key"
      (doseq [f ["person.token" "root.token" "world.env" "log/seg"]]
        (let [r (run w (str "cat " data "/" f))]
          (is (pos? (:exit r)) f)
          (is (not (leaked? r)) f)))
      (let [r (run w "cat .envrc")] (is (pos? (:exit r))) (is (not (leaked? r)))))
    (testing "listing the data dir"
      (is (not (str/includes? (:stdout (run w (str "ls " data))) "person.token"))))
    (testing "a person's home"
      (doseq [d ["~" "~/.ssh" "~/Library/Keychains" "~/Documents"]]
        (is (str/includes? (:stdout (run w (str "ls " d " 2>&1"))) "not permitted") d)))
    (testing "writing outside the root"
      (doseq [path [(str other "/w") "~/box-canary" (str data "/w") (str base "/w") "/usr/local/box-canary"]]
        (let [r (run w (str "echo x > " path))]
          (is (pos? (:exit r)) path)))
      (is (not (.exists (io/file other "w"))))
      (is (not (.exists (io/file (System/getProperty "user.home") "box-canary")))))
    (testing "a symlink out"
      (let [r (run w (str "ln -s " data "/person.token out; cat out"))]
        (is (not (leaked? r)) (pr-str r)))
      (let [r (run w (str "ln -s " other " dir; cat dir/note"))]
        (is (not (leaked? r)) (pr-str r))))
    (testing "a hard link out"
      (let [r (run w (str "ln " data "/person.token hard; cat hard"))]
        (is (not (leaked? r)) (pr-str r)))
      (let [r (run w (str "ln " data "/world.env hard2; cat hard2"))]
        (is (not (leaked? r)) (pr-str r))))
    (testing "moving the data dir, or a secret, out of the way"
      (is (not (leaked? (run w (str "mv " data "/person.token x; cat x")))))
      (is (.exists (io/file data "person.token"))))
    (testing "a child and a grandchild keep the box"
      (is (not (leaked? (run w (str "/bin/sh -c 'cat " data "/person.token'")))))
      (is (not (leaked? (run w (str "python3 -c \"print(open('" data "/person.token').read())\"")))))
      (is (not (leaked? (run w (str "nohup /bin/sh -c 'cat " data "/person.token' & wait"))))))
    (testing "a nested sandbox-exec gets no more"
      (is (not (leaked? (run w (str "sandbox-exec -p '(version 1)(allow default)' cat " data "/person.token"))))))
    (testing "the host's own ways out"
      (is (not (str/includes? (:stdout (run w "launchctl list 2>&1 | wc -l")) "  1"))
          "launchd lists nothing it would act on")
      (let [canary-file (io/file other "launchd")
            r (run w (str "launchctl submit -l orbis.confine.probe -- /bin/sh -c 'cat " data "/person.token > " canary-file "'"))]
        (Thread/sleep 400)
        (is (not (.exists canary-file)) (pr-str r)))
      (let [r (run w "osascript -e 'tell application \"Finder\" to name of startup disk' 2>&1")]
        (is (pos? (:exit r)) (pr-str r)))
      (let [r (run w "pbpaste | wc -c")]
        (is (= "0" (str/trim (:stdout r))) "the clipboard is out of reach")))
    (testing "the environment holds no key"
      (is (not (re-find #"KE_|_KEY|TOKEN" (:stdout (run w "env"))))))
    (testing "a port the Adapter was not given"
      (is (pos? (:exit (run w "curl -s -m 3 http://example.com:81/"))) "denied by the sandbox"))))

(deftest build-tools-work-inside
  (let [{:keys [root] :as w} (world-of)]
    (spit (io/file root "hello.c") "#include <stdio.h>\nint main(){puts(\"built\");return 0;}\n")
    (spit (io/file root "Makefile") "all: hello\n\t./hello\nhello: hello.c\n\tcc -o hello hello.c\n")
    (testing "a project's tests may make a world of their own in scratch, token and all"
      (let [r (run w "mkdir -p $TMPDIR/w && echo mine > $TMPDIR/w/root.token && cat $TMPDIR/w/root.token")]
        (is (zero? (:exit r)) (pr-str (select-keys r [:stdout :stderr])))
        (is (str/includes? (:stdout r) "mine"))))
    (testing "make and cc"
      (let [r (run w "make")]
        (is (zero? (:exit r)) (pr-str (select-keys r [:stdout :stderr])))
        (is (str/includes? (:stdout r) "built"))))
    (testing "cmake"
      (.mkdirs (io/file root "cm"))
      (spit (io/file root "cm" "CMakeLists.txt")
            "cmake_minimum_required(VERSION 3.20)\nproject(h C)\nadd_executable(h ../hello.c)\n")
      (let [r (run w "export PATH=$PATH:/opt/local/bin:/opt/homebrew/bin; cd cm && cmake -B b . && cmake --build b && ./b/h")]
        (is (zero? (:exit r)) (pr-str (select-keys r [:stdout :stderr])))
        (is (str/includes? (:stdout r) "built"))))
    (testing "clojure"
      (.mkdirs (io/file root "clj" "src"))
      (spit (io/file root "clj" "deps.edn") "{:paths [\"src\"]}")
      (spit (io/file root "clj" "src" "a.clj") "(ns a) (defn -main [] (println (+ 40 2)))")
      (let [r (run w "export PATH=$PATH:/opt/homebrew/bin; cd clj && clojure -M -m a")]
        (is (zero? (:exit r)) (pr-str (select-keys r [:stdout :stderr])))
        (is (str/includes? (:stdout r) "42"))))))

(deftest a-root-inside-the-data-dir-is-still-boxed
  ;; self-hosting: shell.local runs in <data>/src
  (let [{:keys [data] :as w} (world-of)
        root (io/file data "src")
        _ (.mkdirs root)
        box (confine/spec {:root root :data-dir data :adapter (random-uuid)})
        r (shell/run! {:cmd "echo here > a; cat a; cat ../person.token ../log/seg ../world.env; ls .." :cwd (.getPath root) :box box})]
    (is (str/includes? (:stdout r) "here") (pr-str r))
    (is (not (leaked? r)))
    (is (not (str/includes? (:stdout r) "person.token")))))

(deftest a-data-dir-inside-the-root-is-still-boxed
  ;; a development world: the repository is the root and data/ is under it
  (let [base (.getCanonicalPath (io/file (fx/temp-dir)))
        data (io/file base "data")
        _ (.mkdirs (io/file data "log"))
        _ (spit (io/file data "log" "seg") canary)
        _ (spit (io/file data "person.token") canary)
        box (confine/spec {:root base :data-dir data :adapter (random-uuid)})
        r (shell/run! {:cmd "cat data/person.token data/log/seg; mv data dota; cat dota/person.token; ls data" :cwd base :box box})]
    (is (not (leaked? r)) (pr-str r))
    (is (.exists (io/file data "person.token")))))

(deftest a-mount-hands-over-no-secret
  (let [{:keys [root]} (world-of)]
    (spit (io/file root "person.token") canary)
    (is (nil? (fs/safe-file root ".envrc")))
    (is (nil? (fs/safe-file root "person.token")))
    (is (some? (fs/safe-file root "main.txt")))))

(deftest too-wide-a-root-is-refused
  (is (confine/too-wide? "/"))
  (is (confine/too-wide? (System/getProperty "user.home")))
  (is (confine/too-wide? (.getParent (io/file (System/getProperty "user.home")))))
  (is (not (confine/too-wide? (fx/temp-dir)))))

;; ---------------------------------------------------------------- through a world

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- shell-of [c] (get-in (ws/request!! c :resolve {:name "shell.local" :op :read}) [:result :object]))
(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- result [r] (let [x (:result r)] (if (contains? x :result) (:result x) x)))

(defn- may-act!
  "What root grants a person in a real world by default: write on the Adapter, evaluate on the actions."
  [root witness shell]
  (fx/grant! root (:id witness) #{shell} #{:read :evaluate :write})
  (doseq [n ["adapter.unconfine" "adapter.confine" "approval.unsafe.off"]]
    (let [a (get-in (ws/request!! root :resolve {:name (str "action/" n) :op :read}) [:result :object])]
      (fx/grant! root (:id witness) #{a} #{:read :evaluate}))))

(defn- edn-of [dir] (read-string (slurp (io/file dir "release.edn"))))

(defn- until [f ms]
  (let [deadline (+ (System/currentTimeMillis) ms)]
    (loop [] (or (f) (when (< (System/currentTimeMillis) deadline) (Thread/sleep 25) (recur))))))

(deftest a-run-and-a-terminal-are-boxed
  (fx/with-world [{:keys [world root witness]}]
    (let [data (:data-dir world)
          person (fx/attach-as world (:token witness))
          shell (shell-of root)]
      (fx/grant! root (:id witness) #{shell} #{:read :evaluate :write})
      (testing "task.run"
        (let [r (act! root "task.run" {:adapter "shell.local" :cmd (str "cat " data "/root.token")})]
          (is (pos? (get-in r [:result :result :exit])) (pr-str r))))
      (testing "a terminal, on a real pty"
        (with-redefs [core/shell-command (fn [] ["/bin/zsh" "-f"])]
          (let [pane (:pane (result (act! root "pane.open" {:kind "terminal" :target "shell.local" :cols 100 :rows 24})))
                screen #(:text (result (act! root "pane.read" {:pane (str pane)})))]
            (is pane)
            (act! root "pane.send" {:pane (str pane) :text (str "cat " data "/root.token; echo done-$((40+2))\n")})
            (is (until #(str/includes? (screen) "done-42") 8000) (screen))
            (is (str/includes? (screen) "not permitted") (screen))
            (act! root "pane.send" {:pane (str pane) :text "exit\n"})))))))

(deftest no-sandbox-no-shell
  (fx/with-world [{:keys [root]}]
    (with-redefs [confine/primitive (fn [] nil)]
      (is (= :unconfined (code (act! root "task.run" {:adapter "shell.local" :cmd "echo no"}))))
      (is (= :unconfined (code (act! root "pane.open" {:kind "terminal" :target "shell.local"})))))))

(deftest a-person-lifts-the-box-for-a-while
  (fx/with-world [{:keys [world root witness]}]
    (let [data (:data-dir world)
          person (fx/attach-as world (:token witness))
          shell (shell-of root)
          wid (:world-id world)]
      (may-act! root witness shell)
      (testing "nobody writes the Field"
        (doseq [k [:adapter/confined :adapter/network :adapter/read :adapter/locator]]
          (doseq [c [root person]]
            (is (= :conflict (code (ws/request!! c :set-field {:object shell :field k :type :string :value "x" :reader :both})))
                (pr-str k)))))
      (testing "root's session cannot, its loop is carded"
        (is (= :person-only (code (act! root "adapter.unconfine" {:adapter (str shell) :minutes 5}))))
        (is (authority/must-card? (act/db world) :act {:action "adapter.unconfine" :args {}})))
      (testing "a person can, and it shows"
        (let [r (act! person "adapter.unconfine" {:adapter (str shell) :minutes 5})]
          (is (:ok r) (pr-str r)))
        (is (unsafe/unconfined? (act/db world) shell (Date.)))
        (let [[e] (field world wid :world/unsafe)]
          (is (= (str shell) (:principal e)))
          (is (= "unconfined" (:kind e))))
        (is (nil? (:box (unsafe/box-for world shell))))
        (let [r (act! root "task.run" {:adapter "shell.local" :cmd (str "cat " data "/root.token")})]
          (is (zero? (get-in r [:result :result :exit])) "out of the box, it reads")))
      (testing "the indicator's OFF ends it"
        (is (:ok (act! person "approval.unsafe.off" {:principal (str shell)})))
        (is (empty? (field world wid :world/unsafe)))
        (is (some? (:box (unsafe/box-for world shell)))))
      (testing "a forged Field counts as confined"
        (is (not (unsafe/unconfined? (act/db world) (random-uuid) (Date.))))))))

(deftest time-puts-it-back
  (fx/with-world [{:keys [world root witness]}]
    (let [person (fx/attach-as world (:token witness))
          shell (shell-of root)]
      (may-act! root witness shell)
      (act! person "adapter.unconfine" {:adapter (str shell) :minutes 1})
      (is (seq (field world (:world-id world) :world/unsafe)))
      (let [later (Date. (+ (System/currentTimeMillis) 120000))]
        (is (not (unsafe/unconfined? (act/db world) shell later)))
        (is (= [shell] (mapv :principal (unsafe/expire! world later)))))
      (is (nil? (field world shell :adapter/confined)))
      (is (empty? (field world (:world-id world) :world/unsafe))))))

(deftest the-network-is-open-and-root-or-a-person-sets-it
  (fx/with-world [{:keys [world root witness]}]
    (let [person (fx/attach-as world (:token witness))
          shell (shell-of root)]
      (may-act! root witness shell)
      (testing "open by default"
        (is (= "all" (:network (:box (unsafe/box-for world shell)))))
        (is (nil? (.get (confine/environment (:box (unsafe/box-for world shell))) "ORBIS_BOX")) "so a build may fetch"))
      (testing "root may tighten a shell, and so may a person"
        (is (:ok (act! root "adapter.confine" {:adapter (str shell) :network "model"})))
        (is (= "1" (.get (confine/environment (:box (unsafe/box-for world shell))) "ORBIS_BOX")) "a build uses what is on disk")
        (is (:ok (act! person "adapter.confine" {:adapter (str shell) :network "all" :read ["/tmp"]})))
        (is (str/includes? (confine/seatbelt-profile (:box (unsafe/box-for world shell))) "*:*")))
      (testing "root's own leash is a person's"
        (is (= :person-only (code (act! root "adapter.confine" {:adapter (str shell) :leash false}))))))))

(deftest a-loop-is-boxed-too
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          r (first (lops/execute! world root-id nil [{:op :set-field :args {:object (shell-of root) :field :adapter/confined
                                                                           :type :string :value "x" :reader :both}}]))]
      (is (false? (:ok r)) (pr-str r)))))

(deftest a-release-comes-out-of-the-box
  (fx/with-world [{:keys [world root]}]
    (let [src (io/file (:data-dir world) "src")
          sh (fn [& a] (let [p (.start (doto (ProcessBuilder. ^java.util.List (vec a)) (.directory src) (.redirectErrorStream true)))]
                         (slurp (.getInputStream p)) (.waitFor p)))
          evil (io/file (:data-dir world) "ran-outside")]
      (.mkdirs src)
      (sh "git" "init" "-q")
      (sh "git" "config" "user.email" "a@b") (sh "git" "config" "user.name" "a")
      (.mkdirs (io/file src "world/src/knowledge/world/log"))
      (spit (io/file src "world/src/knowledge/world/log/segment.clj") "(ns x)\n(def version 7)\n")
      (spit (io/file src "a.evil") "x")
      (spit (io/file src ".gitattributes") "*.evil filter=evil\n")
      (sh "git" "add" "-A") (sh "git" "commit" "-qm" "one")
      ;; what an agent with the working copy can do: a filter that runs when the tree is written out
      (sh "git" "config" "filter.evil.smudge" (str "touch " evil "; cat"))
      (sh "git" "config" "filter.evil.clean" "cat")
      (let [r (act! root "release.make" {})
            {:keys [commit log-version]} (result r)
            dest (io/file (:data-dir world) "releases" (str commit))]
        (is (:ok r) (pr-str r))
        (is (= 7 log-version))
        (is (.isFile (io/file dest "a.evil")))
        (is (= commit (:commit (edn-of dest))))
        (is (not (.canWrite (io/file dest "a.evil"))) "read-only")
        (is (not (.exists evil)) "the working copy's config ran inside the box, not as the world")
        (is (empty? (filter #(str/starts-with? (.getName ^java.io.File %) ".tmp") (.listFiles (.getParentFile dest))))))
      (testing "from the box a release cannot be written"
        (let [box (confine/spec {:root src :data-dir (:data-dir world) :adapter (random-uuid)})
              r (shell/run! {:cmd "mkdir ../releases/x" :cwd (.getPath src) :box box})]
          (is (pos? (:exit r)) (pr-str r))))
      (testing "a rev is not an argument"
        (is (= :invalid-args (code (act! root "release.make" {:rev "--output=/tmp/x"}))))))))

(deftest a-repository-cannot-run-as-the-world
  (fx/with-world [{:keys [world]}]
    (let [dir (doto (io/file (fx/temp-dir) "repo") .mkdirs)
          sh (fn [& args]
               (let [p (.start (doto (ProcessBuilder. ^java.util.List (vec args)) (.directory dir) (.redirectErrorStream true)))]
                 (slurp (.getInputStream p)) (.waitFor p)))
          evil (io/file (:data-dir world) "ran-as-the-world")
          token (io/file (:data-dir world) "root.token")]
      (sh "git" "init" "-q")
      (sh "git" "config" "user.email" "a@b") (sh "git" "config" "user.name" "a")
      (spit (io/file dir "a.evil") "x")
      (spit (io/file dir ".gitattributes") "*.evil filter=evil\n")
      (sh "git" "add" "-A") (sh "git" "commit" "-qm" "one")
      ;; what a shell can write: both run on a plain status
      (sh "git" "config" "core.fsmonitor" (str "touch " evil))
      (sh "git" "config" "filter.evil.clean" (str "touch " evil "; cat " token "; cat"))
      ;; the same size as the index, or git knows it changed without running the filter
      (spit (io/file dir "a.evil") "y")
      (let [r (binding [harness/*data-dir* (:data-dir world)]
                (harness/git (.getPath dir) "status" "--porcelain"))]
        (is (not (.exists evil)) (pr-str r))
        (is (not (str/includes? (:out r) (str/trim (slurp token)))) "the token stayed unread"))
      (testing "the control: outside a world's box that filter does run"
        (spit (io/file dir "a.evil") "z")
        (harness/git (.getPath dir) "status" "--porcelain")
        (is (.exists evil))))))

(deftest a-failed-stage-of-a-pipeline-fails-the-run
  (let [run #(shell/run! {:cmd % :cwd "." :timeout-s 10})]
    (is (not= 0 (:exit (run "false | tail -1"))))
    (is (= 0 (:exit (run "true | tail -1"))))
    (is (= "a.c:1:2: error: no\nmake: *** failed"
           (shell/failing-lines "a.c:1:2: error: no\nok line\nmake: *** failed\n")))
    (is (nil? (shell/failing-lines "all fine\n")))))
