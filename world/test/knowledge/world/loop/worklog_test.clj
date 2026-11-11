(ns knowledge.world.loop.worklog-test
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.adapters.model-api :as model]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.loop.worklog :as w]))

(defn- read-of [name text]
  {:op :read :args {:name name} :ok true
   :result {:object/fields {:file/text {:field/value text} :file/lines {:field/value 3}}}})
(defn- edit-of [name]
  {:op :act :args {:action "file.edit" :args {:name name}} :ok true :result {:result {:name name :diff "-a\n+b"}}})
(defn- run-of [cmd exit]
  {:op :act :args {:action "task.run" :args {:cmd cmd}} :ok true
   :result {:result {:task "t1" :exit exit :command cmd :stdout-tail "ok" :stderr-tail "boom"}}})

(deftest a-run-that-ran-no-tests-is-no-pass
  (let [none {:op :act :args {:action "task.run" :args {:cmd "clojure -M:test --focus x"}} :ok true
              :result {:result {:exit 0 :command "clojure -M:test --focus x"
                                :stdout "WARNING: All 591 tests were skipped."}}}
        log (-> nil (w/add [(edit-of "t.clj")]) (w/add [none]))]
    (is (= ["t.clj"] (w/untested log #"\.md$")))
    (is (w/fixing? log))
    (is (str/includes? (w/render log 1 8) "RAN NO TESTS"))
    (is (str/includes? (w/report log #"\.md$") "ran no tests"))
    (is (str/includes? (w/report log #"\.md$") "Verified:\n- nothing ran and passed"))))

(deftest the-note-says-a-failure-once
  (let [fail {:op :act :args {:action "task.run" :args {:cmd "make"}} :ok true
              :result {:result {:exit 2 :command "make" :stdout "cc a.c" :stderr "a.c:1: error: nope\nnoise"
                                :failing "a.c:1: error: nope"}}}
        note (w/render (-> nil (w/add [(run-of "ls" 0)]) (w/add [fail])) 1 8 "Make it stop.")]
    (is (str/includes? note "The task: Make it stop."))
    (is (str/includes? note "failing lines:\na.c:1: error: nope"))
    (is (not (str/includes? note "noise")) "the failing lines stand for the tail")
    (is (not (re-find #"(?s)`ls`.*stderr" note)) "an older run is a line"))
  (testing "a card waiting is not a refusal"
    (let [log (w/add nil [{:op :act :args {:action "file.edit" :args {:name "a.c"}} :ok false
                           :err {:code :approval-pending :message "awaits approval"}}])]
      (is (empty? (:failed log))))))

(deftest a-wake-out-of-turns-reports-in-the-kernel-s-words
  (testing "changed, built and broken"
    (let [t (w/report (-> nil (w/add [(edit-of "p/a.c")])
                          (w/add [{:op :act :args {:action "task.run" :args {:cmd "make"}} :ok true
                                   :result {:result {:exit 2 :command "make" :failing "a.c:1: error: nope"}}}]))
                      #"\.md$")]
      (is (str/includes? t "Changed:\n- p/a.c"))
      (is (str/includes? t "Verified:\n- nothing ran and passed"))
      (is (str/includes? t "`make` FAILED, exit 2\na.c:1: error: nope"))
      (is (str/includes? t "the goal is not shown to hold"))))
  (testing "changed and passed"
    (let [t (w/report (-> nil (w/add [(edit-of "p/a.c")]) (w/add [(run-of "make" 0)])) #"\.md$")]
      (is (str/includes? t "Verified:\n- `make` exit 0"))
      (is (not (str/includes? t "changed, and nothing passed")))))
  (testing "nothing done still has the three parts"
    (let [t (w/report nil #"\.md$")]
      (is (str/includes? t "Changed:\n- nothing")))))

(deftest a-work-ask-is-told-from-a-question
  (is (w/work? "fix the log level"))
  (is (w/work? "Add a focused test, and run it."))
  (is (not (w/work? "what does this file do?")))
  (is (not (w/work? "set up the project"))))

(deftest what-was-read-stays-open-until-it-changes
  (let [log (w/settle (w/add nil [(read-of "a/b.clj" "(ns b)")]))]
    (is (w/open? log {:op :read :args {:name "a/b.clj"}}))
    (is (not (w/open? log {:op :read :args {:name "a/b.clj" :range [1 2]}})) "a range is a new question")
    (is (str/includes? (w/render log 2 30) "(ns b)"))
    (testing "an edit with nothing to show closes it, and the note says what changed"
      (let [after (w/settle (w/add log [(edit-of "a/b.clj")]))]
        (is (not (w/open? after {:op :read :args {:name "a/b.clj"}})))
        (is (str/includes? (w/render after 3 30) "Changed"))
        (is (str/includes? (w/render after 3 30) "VERIFY"))))
    (testing "an edit with the file as it stands keeps it open, so the next `old` is copied"
      (let [after (w/settle (w/add log [(assoc (edit-of "a/b.clj") :current {:name "a/b.clj" :text "(ns b2)" :lines 1})]))
            note (w/render after 3 30)]
        (is (w/open? after {:op :read :args {:name "a/b.clj"}}))
        (is (str/includes? note "(ns b2)"))
        (is (not (str/includes? note "(ns b)\n")))))
    (testing "a file read by its bare name is the file the edit located under the mount"
      (let [after (w/settle (w/add log [(assoc (edit-of "grid/a/b.clj") :current {:name "grid/a/b.clj" :text "(ns b3)" :lines 1})]))]
        (is (nil? (get-in after [:files "a/b.clj"])) "the stale copy goes")
        (is (= "(ns b3)" (get-in after [:files "grid/a/b.clj" :text])))))
    (testing "a file too long to hold keeps the lines around the change"
      (let [long-text (str/join "\n" (map #(str "line " %) (range 1 2001)))
            r (-> (edit-of "big.c")
                  (assoc-in [:args :args :new] "line 1000")
                  (assoc :current {:name "big.c" :text long-text :lines 2000}))
            f (get-in (w/settle (w/add nil [r])) [:files "big.c"])]
        (is (:open f))
        (is (= [960 1040] (:range f)))
        (is (str/includes? (:text f) "line 1000"))
        (is (not (str/includes? (:text f) "line 900\n")))))))

(deftest the-newest-files-stay-whole-and-the-rest-by-name
  (let [big (apply str (repeat (+ 100 w/open-chars) "x"))
        log (w/settle (w/add (w/add nil [(read-of "old" big)]) [(read-of "new" "small")]))
        note (w/render log 3 30)]
    (is (str/includes? note "small"))
    (is (not (str/includes? note big)))
    (is (str/includes? note "- old"))
    (is (< (count note) (+ w/open-chars 2000)))))

(deftest reading-on-and-on-is-stopped
  (let [log (reduce (fn [l n] (w/add l [(read-of (str "f" n) "x")])) nil (range w/force-after))]
    (is (w/hold-reading? log))
    (is (str/includes? (w/render log 10 30) "WRITE NOW"))
    (is (not (w/hold-reading? (w/add log [(edit-of "f1")]))) "a change opens reading again")))

(deftest when-reading-closes-a-change-is-required
  (let [body (model/request-body (assoc (model/create {:mode :real :url "http://x" :model "m"})
                                        :tools? true :only w/write-tools) [] :off)]
    (is (= #{"edit" "run" "test"} (set (map #(get-in % [:function :name]) (:tools body)))))
    (is (= "required" (:tool_choice body)))))

(deftest looking-through-a-shell-is-still-reading
  (let [log (reduce (fn [l _] (w/add l [(run-of "sed -n 1,40p a.clj" 0)])) nil (range w/force-after))]
    (is (w/hold-reading? log))
    (is (not (w/hold-reading? (w/add log [(run-of "make test" 0)]))))))

(deftest a-run-is-kept-with-where-its-output-is
  (let [log (w/add (w/add nil [(edit-of "a")]) [(run-of "make test" 2)])
        note (w/render log 3 30)]
    (is (str/includes? note "`make test` FAILED, exit 2"))
    (is (str/includes? note "boom"))
    (is (str/includes? note "field task/stdout"))
    (is (str/includes? note "FIX"))))

(deftest the-log-survives-a-card
  (let [k [:p (random-uuid)]]
    (w/remember k (w/add nil [(read-of "a" "x")]))
    (is (= ["a"] (:order (w/recall k))))))

(deftest a-call-written-as-parameters-is-a-call
  (let [{:keys [ops]} (lops/parse {:text "<tool_call>read<parameter name=\"name\">a/b.clj</arg_value></tool_call>"})]
    (is (= "a/b.clj" (get-in (first ops) [:args :name])) (pr-str ops))))

(defn- failed-run-of [cmd]
  (assoc-in (run-of cmd 2) [:result :result :failing] "src/a.cpp:3:1: error: nope"))

(deftest a-failed-build-holds-the-next-turn-to-fixing
  (let [log (-> nil (w/add [(edit-of "a.cpp")]) (w/add [(failed-run-of "make")]))]
    (is (w/fixing? log))
    (is (= #{"edit" "read"} (w/held-tools log)))
    (is (str/includes? (w/render log 3 30) "error: nope"))
    (is (str/includes? (w/render log 3 30) "FAILED, exit 2"))
    (testing "running it again unchanged is refused, with what failed"
      (let [refused (w/repeated-run log {:op :act :args {:action "task.run" :args {:cmd "make"}}})]
        (is (str/includes? refused "error: nope")))
      (is (nil? (w/repeated-run log {:op :act :args {:action "task.run" :args {:cmd "make test"}}}))))
    (testing "a change lets it run again"
      (let [changed (w/add log [(edit-of "a.cpp")])]
        (is (not (w/fixing? changed)))
        (is (nil? (w/repeated-run changed {:op :act :args {:action "task.run" :args {:cmd "make"}}})))))
    (testing "a failure that outlasts the turns lets go"
      (let [stuck (reduce (fn [l _] (w/add l [])) log (range 4))]
        (is (not (w/fixing? stuck)))
        (is (nil? (w/held-tools stuck)))
        (is (str/includes? (w/render stuck 7 30) "STUCK"))))))

(deftest a-failure-before-any-change-is-a-finding
  (is (not (w/fixing? (-> nil (w/add [(failed-run-of "make test")]))))))

(deftest a-look-is-not-a-failure-or-a-pass
  (let [log (-> nil (w/add [(edit-of "a.cpp")]) (w/add [(run-of "grep nothing a.cpp" 1)]))]
    (is (not (w/fixing? log)))
    (is (= ["a.cpp"] (w/untested log #"\.md$")) "grep is not a passing build"))
  (let [log (-> nil (w/add [(edit-of "a.cpp")]) (w/add [(run-of "ls" 0)]))]
    (is (= ["a.cpp"] (w/untested log #"\.md$"))))
  (let [log (-> nil (w/add [(edit-of "a.cpp")]) (w/add [(run-of "make" 0)]))]
    (is (nil? (w/untested log #"\.md$"))))
  (is (nil? (w/untested (-> nil (w/add [(edit-of "a.md")])) #"\.md$")) "a page needs no build"))

(deftest a-timeout-is-not-a-failure-to-fix
  (let [log (-> nil (w/add [(edit-of "a.cpp")]) (w/add [(assoc-in (run-of "make test" -1) [:result :result :timed-out] true)]))]
    (is (not (w/fixing? log)))
    (is (nil? (w/repeated-run log {:op :act :args {:action "task.run" :args {:cmd "make test"}}})))
    (is (str/includes? (w/render log 3 30) "TIMED OUT"))))

(deftest a-tool-spelled-as-an-op-is-that-tool
  (let [{:keys [ops]} (lops/parse {:text "```ops\n{\"op\":\"edit\",\"args\":{\"name\":\"m/a.md\",\"new\":\"x\"}}\n```"})]
    (is (= :act (keyword (name (:op (first ops))))) (pr-str ops))
    (is (= "file.edit" (some-> (get-in (first ops) [:args :action]) name)) (pr-str ops))))
