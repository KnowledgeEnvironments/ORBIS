(ns knowledge.world.projects-test
  "What a Principal's projects are, and adding one in one op (decisions 2026-10-31)."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.core :as loop-core]
            [knowledge.world.serialize.core]
            [knowledge.world.projects :as projects]
            [knowledge.world.ws-client :as ws]))

(defn- put! [dir rel text]
  (let [f (io/file dir rel)]
    (io/make-parents f)
    (spit f text)))

(defn- project! []
  (let [dir (fx/temp-dir)]
    (put! dir "README.md" "<!-- note -->\n# Grid\n\nThe GRiD Compass emulator.\n")
    (put! dir "src/a.c" "int a;\n")
    dir))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- v [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))

(deftest adding-a-project-is-one-op
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          r (act! root "project.add" {:name "grid" :path dir})
          id (get-in r [:result :result :principal])]
      (is (:ok r) (pr-str r))
      (is (str/includes? (get-in r [:result :result :readme]) "GRiD Compass"))
      (testing "the Principal has a mount, a shell, a charter and a Route"
        (is (= 2 (count (v world id :principal/adapters))))
        (is (str/includes? (v world id :instructions/charter) "GRiD Compass"))
        (is (seq (v world id :principal/routes))))
      (testing "its Grant is narrower than root's: it cannot reach root"
        (let [{:keys [ok]} (act/set-field! world id {:object (:principal root) :field :instructions/own :type :string :value "x" :reader :both})]
          (is (nil? ok))))
      (testing "the same name twice is refused before a card"
        (is (= :conflict (get-in (actions/preflight world (act/db world) (:principal root)
                                                    {:action "project.add" :args {:name "grid" :path dir}})
                                 [:err :code])))))))

(deftest a-path-that-is-no-directory-is-refused-before-a-card
  (fx/with-world [{:keys [world root]}]
    (is (= :invalid-args (get-in (actions/preflight world (act/db world) (:principal root)
                                                    {:action "project.add" :args {:name "g" :path "/no/such/place"}})
                                 [:err :code])))))

(deftest adding-a-project-waits-for-a-person-even-in-unsafe-mode
  (fx/with-world [{:keys [world root]}]
    (is (= "project.add" (gates/gated? (act/db world) (:principal root) :act {:action "project.add" :args {}})))
    (is (some #{"project.add"} unsafe/default-except))))

(deftest status-comes-from-the-record-and-the-project
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          _ (act! root "project.add" {:name "grid" :path dir})
          {:keys [projects next]} (projects/status (act/db world) (:principal root))
          [{:keys [name path about state contents owner]} :as all] projects]
      (is (= 1 (count all)) "the harness is not a project")
      (is (= "grid" name))
      (is (= (.getCanonicalPath (io/file dir)) path))
      (is (str/includes? about "GRiD Compass"))
      (is (= "idle" state))
      (is (= "grid" owner))
      (is (some #{"src/"} contents))
      (is (string? next)))))

(deftest status-needs-no-card-and-any-principal-can-ask
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          _ (act! root "project.add" {:name "grid" :path dir})
          id (objects/lookup (act/db world) (:world-id world) "grid")
          r (act! root "projects" {})]
      (is (:ok r) (pr-str r))
      (is (nil? (gates/gated? (act/db world) (:principal root) :act {:action "projects" :args {}})))
      (testing "a project's Principal sees its own, not root's others"
        (is (= ["grid"] (map :name (:projects (projects/status (act/db world) id)))))))))

(deftest the-prompt-says-what-the-projects-are
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)]
      (is (str/includes? (projects/line (act/db world) root-id true) "add_project") "none yet: how to add one")
      (is (nil? (projects/line (act/db world) root-id false)))
      (act! root "project.add" {:name "grid" :path (project!)})
      (let [l (projects/line (act/db world) root-id true)]
        (is (str/includes? l "grid ("))
        (is (str/includes? l "`projects`"))))))

(deftest a-citation-rests-on-a-read-that-succeeded
  (let [cited @#'loop-core/cited-as-read
        note @#'loop-core/with-unread-note
        challenge @#'loop-core/unread-challenge
        failed-read {:op "read" :args {:name "grid/STATUS.md"} :err {:code :unresolved}}
        st {:done [] :failed [failed-read]}]
    (testing "quoted after every read failed"
      (is (= ["grid/STATUS.md"] (cited st "grid: `grid/STATUS.md` says one line.")))
      (is (string? (challenge st "According to STATUS.md all is well."))))
    (testing "a true report of the failure is not a citation"
      (is (nil? (cited st "I could not read grid/STATUS.md.")))
      (is (nil? (cited st "grid/STATUS.md does not exist."))))
    (testing "another name, or a later good read, is fine"
      (is (nil? (cited st "See README.md.")))
      (is (nil? (cited (assoc st :done [{:op "read" :ok true :args {:name "grid/STATUS.md"}}]) "STATUS.md says so."))))
    (testing "a command that failed counts as a failed read"
      (let [run {:op "act" :ok true :args {:args {:cmd "cat grid/STATUS.md"}} :result {:exit 1 :task "t"}}]
        (is (= ["grid/STATUS.md"] (cited {:done [run] :failed []} "STATUS.md says all is well.")))))
    (testing "asked once, then the kernel says so itself"
      (is (nil? (challenge (assoc st :cited? true) "STATUS.md says so.")))
      (is (str/includes? (note nil {} st "STATUS.md says so.") "Not verified"))
      (is (= "See README.md." (note nil {} st "See README.md."))))))

(deftest a-failed-read-says-where-to-go
  (fx/with-world [{:keys [world root]}]
    (let [hint @#'loop-core/read-hint
          r {:op "read" :args {:name "/tmp/x/README.md"} :ok false :err {:code :unresolved :message "does not resolve"}}]
      (is (str/includes? (get-in (hint (act/db world) (:principal root) r) [:err :message]) "add_project"))
      (is (= {:op "act"} (hint (act/db world) (:principal root) {:op "act"}))))))

(deftest with-nothing-said-the-kernel-reports-the-record
  (let [f @#'loop-core/found-so-far]
    (is (nil? (f [])))
    (let [t (f [{:op "act" :ok true :args {:args {:cmd "ctest"}} :result {:exit 0 :task "t" :stdout "100% tests passed\n"}}
                {:op :act :ok true :args {:args {:pattern "boot"}} :result {:result {:pattern "boot" :results [{:path "g/a.c"}]}}}
                {:op "read" :ok true :args {:name "g/a.c"}}])]
      (is (str/includes? t "`ctest` exited 0: 100% tests passed"))
      (is (str/includes? t "`boot` matched in g/a.c"))
      (is (str/includes? t "read g/a.c")))))

(deftest a-denial-says-what-is-left
  (is (str/includes? gates/denied-next "do not repeat"))
  (is (str/includes? gates/denied-next "add_project")))

(deftest a-status-reaches-the-model-whole
  (let [lines @#'knowledge.world.serialize.core/op-result-lines
        p {:name "atlas" :path "/p/atlas" :owner "atlas" :state "idle" :about "a control plane"
           :git {:branch "main" :last-commit "abc fix" :uncommitted 2} :tests "ctest"
           :last-run {:command "ctest" :exit 0} :last-said "done" :contents ["a/" "b"]}
        ls (lines {:result {:projects [(assoc p :name "grid") p] :next "answer"} :object "x"})
        text (str/join "\n" ls)]
    (is (str/includes? text "## grid at"))
    (is (str/includes? text "## atlas at /p/atlas"))
    (is (str/includes? text "last commit abc fix, 2 uncommitted"))
    (is (str/includes? text "next: answer")))
  (testing "any action's result is unwrapped, not clipped as one key"
    (let [lines @#'knowledge.world.serialize.core/op-result-lines
          long-text (apply str (repeat 300 "x"))]
      (is (some #(str/includes? % long-text) (lines {:result {:text long-text :other 1}}))
          "the text is whole"))))

(deftest a-named-directory-is-mounted-not-shelled
  (fx/with-world [{:keys [world root]}]
    (let [f @#'loop-core/unmounted-dir
          dir (.getCanonicalPath (io/file (project!)))
          sub (str dir "/src")
          id (:principal root)
          ctx {:m-state {:object/fields {:message/text {:field/value (str "set up the project under " dir ".")}}}}]
      (is (= dir (f world id ctx (str "ls -la " dir))))
      (is (= dir (f world id ctx (str "cat \"" sub "/a.c\""))) "under it, quoted")
      (is (nil? (f world id ctx "ls -la")) "a command that names no path")
      (is (nil? (f world id ctx "ls /usr/bin")) "a path nobody named")
      (is (= dir (f world id ctx "cat /no/such/place/README.md")) "a path that is not there, with a directory named")
      (is (str/includes? (@#'loop-core/named-directory-note world id ctx) dir) "the wake is told")
      (act! root "project.add" {:name "grid" :path dir})
      (is (nil? (f world id ctx (str "ls " sub))) "mounted now")
      (is (nil? (@#'loop-core/named-directory-note world id ctx)) "and the note goes"))))

(deftest test-is-the-project-s-own-command-in-one-step
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          _ (put! dir "CMakeLists.txt" "project(x)\nenable_testing()\n")
          _ (put! dir "CMakePresets.json" "{\"testPresets\":[{\"name\":\"debug\"}]}")
          _ (act! root "project.add" {:name "grid" :path dir})
          id (objects/lookup (act/db world) (:world-id world) "grid")
          line @#'knowledge.world.loop.ops/tool-line
          l (line {:db (act/db world) :p id} {:name "test" :arguments "{}"})]
      (is (str/includes? l "ctest --preset debug"))
      (is (str/includes? l "task.run")))))

(deftest what-a-card-held-back-comes-back-with-the-decision
  (fx/with-world [{:keys [world root witness]}]
    (let [note @#'gates/held-note
          t (random-uuid)]
      (is (nil? (note (act/db world) t)))
      (with-redefs-fn {#'gates/field (fn [_ _ k] (when (= k :task/held) ["project.add {\"name\":\"atlas\"}"]))}
        #(is (str/includes? (note (act/db world) t) "atlas"))))))

(deftest a-tool-name-is-not-an-answer
  (let [f @#'loop-core/not-an-answer]
    (is (string? (f "act")))
    (is (string? (f "add_project")))
    (is (nil? (f "act now, it says so in the README")))))

(deftest a-charter-of-two-words-gives-way-to-the-readme
  (fx/with-world [{:keys [world root]}]
    (let [id (get-in (act! root "project.add" {:name "grid" :path (project!) :charter "grid project"}) [:result :result :principal])]
      (is (str/includes? (v world id :instructions/charter) "GRiD Compass")))))

(deftest a-hidden-failure-shows-nothing
  (fx/with-world [{:keys [world]}]
    (let [f @#'loop-core/blind-challenge
          run (fn [cmd exit] {:op "act" :ok true :args {:args {:cmd cmd}} :result {:exit exit :task "t"}})
          st {:done [(run "ls src/harness 2>/dev/null" 1)]}]
      (is (string? (f world {} st "There is no code here, and the repository is not mounted.")))
      (is (nil? (f world {} st "It lists three files.")) "no claim of absence")
      (is (nil? (f world {} (assoc st :blind? true) "There is no code here.")) "once")
      (is (nil? (f world {} {:done [(run "ls src/harness" 1)]} "There is no code here.")) "its errors were on the record")
      (is (nil? (f world {} {:done [(run "ls src 2>/dev/null" 0)]} "There is no code here.")) "it worked")
      (is (str/includes? (@#'loop-core/with-unread-note world {} st "There is no code here.") "errors hidden")))))

(deftest the-prompt-says-where-its-own-source-is
  (fx/with-world [{:keys [world root witness]}]
    (let [s (projects/source-line (act/db world) (:principal root))]
      (when s
        (is (str/includes? s "harness/README.md"))
        (is (str/includes? s "by name"))))
    (is (nil? (projects/source-line (act/db world) (:id witness))) "a Principal who cannot read it is not told")))

(deftest a-card-names-the-adapter
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          _ (act! root "project.add" {:name "grid" :path dir})
          shell (objects/lookup (act/db world) (:world-id world) "grid.shell")
          line (gates/summary (act/db world) :act {:action "task.run" :args {:cmd "ls" :adapter (str shell)}})]
      (is (str/includes? line "through grid.shell"))
      (is (not (str/includes? line (str shell)))))))

(deftest a-stopped-wake-reports-the-status-it-has
  (let [t (@#'loop-core/found-so-far [{:op "act" :ok true :args {:action "projects"}
                                       :result {:result {:projects [{:name "grid" :state "idle" :about "a machine"}
                                                                    {:name "atlas" :state "idle" :git {:last-commit "abc x" :uncommitted 2}}]}}}])]
    (is (str/includes? t "grid, idle: a machine"))
    (is (str/includes? t "Last commit abc x, 2 uncommitted"))))
