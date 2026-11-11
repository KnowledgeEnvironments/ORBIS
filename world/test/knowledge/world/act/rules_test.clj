(ns knowledge.world.act.rules-test
  "A gate its owner cannot move, and standing approval rules (decisions 2026-10-30)."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.rules :as rules]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws]))

(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- set-policy [p v] {:op :set-field :args {:object p :field :policy/approval :type [:list :string] :value v :reader :both}})

(defn- process!
  "A process Principal root makes, with write on itself. Returns its id."
  [root name]
  (let [id (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name name}})
                   [:result :object])]
    (fx/grant! root id #{id} #{:read :write})
    id))

(deftest a-principal-cannot-loosen-its-own-gate
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          narrow (process! root "narrow")
          before (field world narrow :policy/approval)]
      (is (seq before) "a process Principal is born with a policy")
      (testing "root's loop, a narrow loop"
        (doseq [p [root-id narrow]]
          (let [[r] (lops/execute! world p nil [(set-policy p [])])]
            (is (false? (:ok r)) (pr-str r))
            (is (= :person-only (keyword (name (get-in r [:err :code]))))))))
      (testing "root's own session, on itself and on another"
        (doseq [p [root-id narrow]]
          (is (= :person-only (get-in (ws/request!! root :set-field (:args (set-policy p []))) [:err :code])))))
      (is (= before (field world narrow :policy/approval)) "unchanged on the log")
      (testing "a person's own session may"
        (fx/grant! root (:id witness) #{narrow} #{:read :write})
        (let [w (fx/attach-as world (:token witness))
              r (ws/request!! w :set-field (:args (set-policy narrow ["grant"])))]
          (is (:ok r) (pr-str r))
          (is (= ["grant"] (field world narrow :policy/approval))))))))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))

(defn- delivered
  "Message ids delivered on sub until pred holds or timeout-ms passes."
  [c sub pred timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop [acc []]
      (if (or (pred acc) (> (System/currentTimeMillis) deadline))
        acc
        (let [push (ws/await-push c sub 2000)
              ids (keep #(when (= :delivered (:event/kind %)) (get-in % [:event/body :message])) (:events push))]
          (recur (into acc ids)))))))

(defn- listen!
  "witness reads itself, transmits on root.route, subscribes to its inbox; {:route :w}."
  [root witness world]
  (let [route (get-in (ws/request!! root :resolve {:name "root.route" :op :read}) [:result :object])
        w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{(:id witness)} #{:read})
    (fx/grant! root (:id witness) #{route} #{:transmit})
    (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
    {:route route :w w}))

(defn- task-of [world card] (field world card :message/task))
(defn- card-in [world ids] (first (filter #(= :approval (some-> (field world % :message/kind) name keyword)) ids)))

(deftest bind-and-grant-do-not-walk-around-the-gate
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)]
      (testing "an Object bound as a new action name that reaches a gated handler is gated by that handler"
        (let [o (get-in (ws/request!! root :act {:class :object :action :create
                                                 :args {:fields {:action/name "run2" :action/handler :task.run}}})
                        [:result :object])]
          (is (:ok (ws/request!! root :bind {:namespace (objects/world-id (act/db world)) :name "action/run2" :object o})))
          (is (= "task.run" (gates/gated? (act/db world) root-id :act {:action :run2 :args {}})))))
      (testing "a grant from a loop waits for a person, and an approved one runs as today"
        (let [{:keys [route w]} (listen! root witness world)
              target (process! root "beneficiary")
              line (str "{\"op\": \"grant\", \"args\": {\"grantee\": \"" target "\", \"scope\": [\"" target "\"], \"ops\": [\"read\"]}}")
              before (count (field world target :principal/grants))
              _ (ws/request!! w :send {:to #{root-id} :route route :text (str "please:\n" (ops-block line))})
              ids (delivered w "inbox" #(some? (card-in world %)) 20000)
              task (task-of world (card-in world ids))]
          (is (uuid? task))
          (is (= :pending (field world task :task/state)))
          (is (= before (count (field world target :principal/grants))) "nothing granted yet")
          (is (= :conflict (get-in (act! root "task.approve" {:task (str task)}) [:err :code]))
              "the requester never decides its own gate")
          (let [other (process! root "other")]
            (fx/grant! root other :everything #{:read :evaluate :write})
            (is (= :unresolved (get-in (gates/approve! world other {:task (str task)}) [:err :code]))
                "a Grant on everything does not make an approver"))
          (let [r (act! w "task.approve" {:task (str task)})]
            (is (:ok r) (pr-str r))
            (is (= :done (field world task :task/state)))
            (is (true? (:ok (field world task :task/result)))))
          (ws/close! w))))))

;; ---------------------------------------------------------------- the matcher

(defn- temp-root []
  (let [d (fx/temp-dir)]
    (spit (str d "/README.md") "hello\n")
    (.mkdirs (java.io.File. (str d "/src")))
    (spit (str d "/src/a.c") "int main;\n")
    d))

(defn- fits [m cmd root & [cwd]] (rules/match-command m {:cmd cmd :root root :cwd cwd}))
(def read-only {:preset "read-only"})

(deftest the-tokenizer-refuses-what-needs-a-shell
  (is (= ["grep" "a b" "src"] (mapv :text (:tokens (rules/tokenize "grep 'a b' src")))))
  (is (= ["cat" "x y"] (mapv :text (:tokens (rules/tokenize "cat \"x y\"")))))
  (doseq [cmd ["grep x; rm -rf ." "ls && rm x" "ls | sh" "ls > out" "cat < x" "echo $(whoami)" "echo $HOME"
               "echo `whoami`" "ls\nrm x" "(rm x)" "ls & rm x" "cat 'unterminated" "ls \\; rm x" "cat {a,b}" "" "   "]]
    (is (:refuse (rules/tokenize cmd)) cmd)))

(deftest the-read-only-preset
  (let [root (temp-root)]
    (doseq [cmd ["ls" "ls -la src" "cat README.md" "head -n 3 README.md" "tail -5 src/a.c" "wc -l README.md"
                 "grep -rn main src" "rg main" "find . -name '*.c'" "file README.md" "stat README.md" "du -sh ."
                 "tree src" "pwd" "git log --oneline -5" "git diff" "git status" "git show HEAD" "git blame README.md"]]
      (is (:matches (fits read-only cmd root)) cmd))
    (testing "injection and escape"
      (doseq [cmd ["grep x; rm -rf ." "cat ../../etc/passwd" "cat /etc/passwd" "cat ~/.ssh/id_rsa" "find . -delete"
                   "find . -exec rm {} ;" "find . -execdir ls +" "find . -fprint out" "git -c core.pager=x log"
                   "git log --output=x" "git diff -o x" "git log -c core.pager=x" "git push" "git commit -m x"
                   "cat $(whoami)" "cat `whoami`" "ls\nrm x" "rg --pre ./x main" "tree -o out" "file -C x"
                   "grep -f/etc/passwd x src" "grep --file=/etc/passwd x src" "cat src/../../x" "/bin/ls" "./ls"
                   "FOO=bar ls" "cat .*" "cat .envrc" "cat data/root.token"]]
        (is (not (:matches (fits read-only cmd root))) cmd)))
    (testing "never by a preset"
      (doseq [cmd ["sh x" "bash x" "zsh x" "env ls" "xargs rm" "python x.py" "perl x" "ruby x" "node x" "sudo ls" "rm x" "curl x"]]
        (is (not (:matches (fits read-only cmd root))) cmd)
        (is (not (:matches (fits {:preset "test"} cmd root))) cmd)))
    (testing "a symlink out of the root is out of the root"
      (java.nio.file.Files/createSymbolicLink (.toPath (java.io.File. (str root "/out")))
                                              (.toPath (java.io.File. "/etc/hosts"))
                                              (make-array java.nio.file.attribute.FileAttribute 0))
      (is (not (:matches (fits read-only "cat out" root)))))
    (testing "the working directory counts"
      (is (:matches (fits read-only "cat a.c" root "src")))
      (is (not (:matches (fits read-only "ls" root "../.."))))
      (is (not (:matches (fits read-only "cat ../../x" root "src")))))))

(deftest the-test-preset-and-explicit-programs
  (let [root (temp-root)
        t {:preset "test"}]
    (doseq [cmd ["make test" "make test-unit" "ctest --output-on-failure" "cargo test" "npm test" "pytest tests"]]
      (is (:matches (fits t cmd root)) cmd))
    (doseq [cmd ["make" "make install" "make clean" "cargo build" "npm install" "ctest -S x.cmake" "pytest -p evil"]]
      (is (not (:matches (fits t cmd root))) cmd))
    (testing "an explicit program is still subject to the operator and path checks"
      (let [m {:programs ["make"]}]
        (is (:matches (fits m "make install" root)))
        (is (not (:matches (fits m "make; rm x" root))))
        (is (not (:matches (fits m "make -f /etc/x" root))))))
    (testing "args-prefix and paths"
      (is (:matches (fits {:programs ["git"] :args-prefix ["log"]} "git log -3" root)))
      (is (not (:matches (fits {:programs ["git"] :args-prefix ["log"]} "git push" root))))
      (is (:matches (fits {:preset "read-only" :paths ["src"]} "cat a.c" root "src")))
      (is (:matches (fits {:preset "read-only" :paths ["src"]} "cat ../src/a.c" root "src")) "normalised")
      (is (not (:matches (fits {:preset "read-only" :paths ["src"]} "cat README.md" root)))))
    (is (not (:matches (fits {} "ls" root))) "a rule that names nothing matches nothing")))

;; ---------------------------------------------------------------- rules in a world

(def ls-line "{\"op\": \"act\", \"args\": {\"action\": \"task.run\", \"args\": {\"adapter\": \"shell.local\", \"cmd\": \"pwd\"}}}")
(def rm-line "{\"op\": \"act\", \"args\": {\"action\": \"task.run\", \"args\": {\"adapter\": \"shell.local\", \"cmd\": \"pwd; rm x\"}}}")

(defn- person-grants!
  "witness may read and write root, and call the rule actions."
  [root witness]
  (fx/grant! root (:id witness) #{(:principal root)} #{:read :write})
  (doseq [a ["approval.rule.add" "approval.rule.remove" "approval.rule.list" "approval.rule.test"
             "task.decide-many" "task.approve" "task.deny"]]
    (fx/grant! root (:id witness)
               #{(get-in (ws/request!! root :resolve {:name (str "action/" a) :op :read}) [:result :object])}
               #{:read :evaluate})))

(defn- add-rule! [w root args]
  (let [r (act! w "approval.rule.add" (merge {:principal (str (:principal root))} args))]
    (is (:ok r) (pr-str r))
    (get-in r [:result :result :rule])))

(defn- approval-tasks [world]
  (let [db (act/db world)]
    (filter #(= "approval" (field world % :task/kind)) (objects/objects-of-class db :task))))

(deftest a-rule-lets-a-matching-run-through-with-a-record
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (listen! root witness world)]
      (person-grants! root witness)
      (testing "only a person, from their own session"
        (is (= :person-only (get-in (act! root "approval.rule.add" {:principal (str root-id) :action "task.run" :match {:preset "read-only"}})
                                    [:err :code]))))
      (is (= :invalid-args (get-in (act! w "approval.rule.add" {:principal (str root-id) :action "task.run"}) [:err :code])))
      (let [rule (add-rule! w root {:action "task.run" :match {:preset "read-only"} :uses 2 :note "reading"})]
        (testing "the dry run says why"
          (let [t (fn [cmd] (get-in (act! w "approval.rule.test" {:principal (str root-id) :rule (:id rule) :cmd cmd}) [:result :result]))]
            (is (true? (:matches (t "ls"))))
            (is (false? (:matches (t "ls; rm x"))))
            (is (string? (:reason (t "ls; rm x"))))))
        (testing "a matching run: no card, a Task with task/rule, the result in the same wake"
          (ws/request!! w :send {:to #{root-id} :route route :text (str "go:\n" (ops-block ls-line))})
          (let [ids (delivered w "inbox" seq 20000)]
            (is (nil? (card-in world ids)))
            (let [t (first (approval-tasks world))]
              (is (= (:id rule) (field world t :task/rule)))
              (is (= :done (field world t :task/state)))
              (is (= (:id witness) (field world t :task/decided-by)))
              (is (inst? (field world t :task/decided-at)))
              (is (zero? (field world t :task/exit)))
              (is (uuid? (field world t :task/ran))))))
        (is (= 1 (:used (first (rules/rules (act/db world) root-id)))))
        (is (= 1 (:uses-left (first (rules/rules (act/db world) root-id)))))
        (testing "an operator gets a card"
          (ws/request!! w :send {:to #{root-id} :route route :text (str "go:\n" (ops-block rm-line))})
          (let [ids (delivered w "inbox" #(some? (card-in world %)) 20000)
                card (card-in world ids)]
            (is (some? card))
            (is (= :pending (field world (task-of world card) :task/state)))
            (is (= :denied (do (act! w "task.deny" {:task (str (task-of world card)) :reason "no"})
                               (field world (task-of world card) :task/state))))))
        (testing "removal"
          (is (:ok (act! w "approval.rule.remove" {:principal (str root-id) :rule (:id rule)})))
          (is (empty? (rules/rules (act/db world) root-id)))))
      (ws/close! w))))

(deftest a-rule-ends-by-uses-and-by-time
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          args {:action :task.run :args {:adapter "shell.local" :cmd "pwd"}}
          w (fx/attach-as world (:token witness))]
      (person-grants! root witness)
      (let [spent (add-rule! w root {:action "task.run" :match {:preset "read-only"} :uses 1})]
        (is (= (:id spent) (:id (rules/admit (act/db world) root-id "task.run" :act args))))
        (rules/used! world root-id (:id spent))
        (is (nil? (rules/admit (act/db world) root-id "task.run" :act args)) "uses exhausted")
        (rules/remove! world root-id (:id spent)))
      (let [old (add-rule! w root {:action "task.run" :match {:preset "read-only"} :until "2000-01-01T00:00:00Z"})]
        (is (nil? (rules/admit (act/db world) root-id "task.run" :act args)) "expired")
        (rules/remove! world root-id (:id old)))
      (testing "a rule only a person wrote counts"
        (let [forged {:id "r-x" :action "task.run" :match {:preset "read-only"} :by (str root-id)}]
          (fx/policy! world root {:object root-id :field rules/rules-field :type rules/rules-type :value [forged] :reader :both})
          (is (nil? (rules/admit (act/db world) root-id "task.run" :act args)))))
      (ws/close! w))))

(deftest deciding-and-remembering
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (listen! root witness world)
          card! (fn [line]
                  (ws/request!! w :send {:to #{root-id} :route route :text (str "go:\n" (ops-block line))})
                  (task-of world (card-in world (delivered w "inbox" #(some? (card-in world %)) 20000))))]
      (person-grants! root witness)
      (testing "remember: approve this one and keep a rule"
        (let [t (card! ls-line)
              r (act! w "task.approve" {:task (str t) :remember {:uses 5}})]
          (is (:ok r) (pr-str r))
          (is (= ["pwd"] (get-in (first (rules/rules (act/db world) root-id)) [:match :programs])))
          (is (= 5 (:uses-left (first (rules/rules (act/db world) root-id)))))
          (is (= "task.run" (:action (first (rules/rules (act/db world) root-id)))))))
      (rules/remove! world root-id (:id (first (rules/rules (act/db world) root-id))))
      (testing "decide-many: the requester never decides, a filter narrows, one result each"
        (let [ask (get-in (ws/request!! w :send {:to #{root-id} :route route :text "two runs follow"}) [:result :message])
              mk (fn [cmd] (:task (gates/gate! world root-id {:op :act :message ask :route route
                                                              :args {:action :task.run :args {:adapter "shell.local" :cmd cmd}}})))
              t1 (mk "ls one") t2 (mk "ls two")]
          (is (= :person-only (get-in (act! root "task.decide-many" {:tasks [(str t1)] :decision "approve"}) [:err :code])))
          (is (= :invalid-args (get-in (act! w "task.decide-many" {:decision "deny"}) [:err :code])))
          (let [r (act! w "task.decide-many" {:filter {:action "task.run"} :decision "deny" :reason "tidy"})
                res (get-in r [:result :result])]
            (is (:ok r) (pr-str r))
            (is (= 2 (:decided res)) (pr-str res))
            (is (= :denied (field world t1 :task/state)))
            (is (= "tidy" (field world t2 :task/reason))))
          (let [again (get-in (act! w "task.decide-many" {:tasks [(str t1)] :decision "approve"}) [:result :result])]
            (is (= 0 (:decided again)))
            (is (= :conflict (keyword (name (get-in again [:results 0 :err :code]))))))))
      (ws/close! w))))

;; ---------------------------------------------------------------- filtered Grant scopes

(deftest a-filtered-scope-covers-what-matches-at-resolution
  (fx/with-world [{:keys [world root witness]}]
    (let [w (fx/attach-as world (:token witness))
          id (:id witness)
          reads? (fn [o] (some? (get-in (ws/request!! w :read {:object o}) [:result :object/class])))
          mk (fn [class args] (get-in (ws/request!! root :act {:class class :action :create :args args}) [:result :object]))
          task (mk :task {:title "one"})
          note (mk :object {:fields {:notes/today "x"}})
          plain (mk :object {:fields {:other/thing "x"}})]
      (testing "by class"
        (is (not (reads? task)))
        (is (:ok (ws/request!! root :grant {:grantee id :scope {:class :task} :ops #{:read} :delegable false})))
        (is (reads? task))
        (is (not (reads? note)))
        (testing "an Object made later matches too"
          (is (reads? (mk :task {:title "two"})))))
      (testing "by Field prefix"
        (is (:ok (ws/request!! root :grant {:grantee id :scope {:field-prefix "notes/"} :ops #{:read} :delegable false})))
        (is (reads? note))
        (is (not (reads? plain))))
      (testing "constraints combine"
        (let [c (fx/attach-as world (get-in (ws/request!! root :act {:class :principal :action :create
                                                                      :args {:driven-by :person :name "test/second"}})
                                            [:result :result :token]))]
          (is (:ok (ws/request!! root :grant {:grantee (:principal c) :scope {:class :task :prefix "grid"} :ops #{:read} :delegable false})))
          (is (nil? (get-in (ws/request!! c :read {:object task}) [:result :object/class])) "a Task with no such name")
          (ws/close! c)))
      (testing "a scope naming nothing is refused"
        (is (= :invalid (get-in (ws/request!! root :grant {:grantee id :scope {} :ops #{:read}}) [:err :code]))))
      (testing "narrower only: a delegable class Grant covers a tighter filter, not a wider one"
        (fx/grant! root id #{id} #{:read})
        (fx/grant! root id {:class :task} #{:read :delegate} :delegable true)
        (let [other (:principal root)]
          (is (:ok (ws/request!! w :grant {:grantee other :scope {:class :task :prefix "a"} :ops #{:read}})))
          (is (= :unresolved (get-in (ws/request!! w :grant {:grantee other :scope {:prefix "a"} :ops #{:read}}) [:err :code])))))
      (testing "JSON form"
        (let [c (fx/attach-as world (fx/root-token world) :json)
              r (ws/request!! c :grant {:grantee id :scope {:class :message} :ops #{:read} :delegable false})]
          (is (:ok r) (pr-str r))
          (ws/close! c)))
      (ws/close! w))))

(deftest a-read-only-pipeline-is-checked-stage-by-stage
  (let [root (temp-root)
        ro {:preset "read-only"}]
    (testing "plain pipelines fit the preset"
      (is (:matches (fits ro "grep -n skywire README.md | head -20" root)))
      (is (:matches (fits ro "git log --oneline | head" root)))
      (is (:matches (fits ro "ls | wc -l" root))))
    (testing "every stage is held to the preset"
      (is (not (:matches (fits ro "cat README.md | sh" root))))
      (is (not (:matches (fits ro "ls | xargs rm" root))))
      (is (not (:matches (fits ro "cat README.md | tee out.txt" root))))
      (is (not (:matches (fits ro "grep x . | head > out" root))))
      (is (not (:matches (fits ro "ls | " root))))
      (is (not (:matches (fits ro "ls || rm -rf ." root))))
      (is (not (:matches (fits ro "ls | head; rm x" root))))
      (is (not (:matches (fits ro "grep 'a|b' ../../etc/passwd" root)))))
    (testing "a pipeline never fits a rule that only names programs"
      (is (not (:matches (fits {:programs ["ls" "sh"]} "ls | sh" root)))))))

(deftest a-recursive-read-does-not-reach-credentials
  (let [root (temp-root)
        ro {:preset "read-only"}]
    (is (:matches (fits ro "grep -r key ." root)) "nothing sensitive here")
    (spit (str root "/.envrc") "export X=1")
    (is (not (:matches (fits ro "grep -r key ." root))))
    (is (not (:matches (fits ro "grep -rn key ." root))))
    (is (not (:matches (fits ro "rg key" root))))
    (is (:matches (fits ro "grep key README.md" root)) "a named file is still fine")
    (is (:matches (fits ro "find . -name '*.md'" root)) "find lists names, not contents")))

(deftest a-read-may-quiet-its-stderr
  (let [dir (fx/temp-dir)
        ro {:preset "read-only"}
        fits? (fn [m cmd] (:matches (rules/match-command m {:cmd cmd :root dir :cwd nil})))]
    (is (fits? ro "ls 2>/dev/null"))
    (is (fits? ro "ls . 2>/dev/null | head -n 5"))
    (is (not (fits? ro "ls > /tmp/x")) "a write still refuses")
    (is (not (fits? ro "ls 2>/tmp/x")) "and stderr to a file")
    (is (not (fits? ro "ls 2>/dev/null; rm x")))
    (is (not (fits? {:programs ["ls"]} "ls 2>/dev/null")) "a named rule stays exact")))

(deftest what-a-developer-writes-to-read-and-test
  (let [root (fx/temp-dir)
        fits? (fn [preset cmd] (:matches (rules/match-command {:preset preset} {:cmd cmd :root root :cwd "."})))]
    (.mkdirs (java.io.File. root "sub"))
    (testing "sed -n with a line range reads"
      (is (fits? "read-only" "sed -n 1,20p a.txt"))
      (is (fits? "read-only" "sed -n 5p a.txt | head -3")))
    (testing "any other sed is not reading"
      (is (not (fits? "read-only" "sed -i s/a/b/ a.txt")))
      (is (not (fits? "read-only" "sed -n 1w/tmp/x a.txt")))
      (is (not (fits? "read-only" "sed s/a/b/ a.txt"))))
    (testing "cd into the project, and stderr joined, change nothing"
      (is (fits? "read-only" "cd sub && cat a.txt"))
      (is (fits? "test" "cd sub && make test 2>&1")))
    (testing "cd out of the root does not"
      (is (not (fits? "read-only" "cd .. && cat a.txt")))
      (is (not (fits? "read-only" "cd /etc && cat hosts"))))
    (testing "a test run may be filtered, by a reader only"
      (is (fits? "test" "make test 2>&1 | tail -20"))
      (is (not (fits? "test" "make test | sh")))
      (is (not (fits? "test" "make deploy | tail"))))))
