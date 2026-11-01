(ns knowledge.world.loop.realwork-test
  "What real development needs of a wake: turns while it makes progress, and done meaning verified.
   Part B of the project's own notes went 0 of 12 for want of both."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.code :as code]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.core :as loop]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.loop.worklog :as worklog]
            [knowledge.world.ws-client :as ws]))

(def turn-limit #'loop/turn-limit)
(def progress #'loop/progress)
(def untested #'loop/untested-edits)
(def claimed #'loop/claimed-writes)
(def verify #'loop/verify-challenge)

(defn- run [cmd exit] {:ok true :op "act" :args {:action "task.run" :args {:cmd cmd}}
                       :result {:task "t" :exit exit}})
(defn- edit [n] {:ok true :op "act" :args {:action "file.edit" :args {:name n}}})
(defn- read-of [n] {:ok true :op "read" :args {:name n}})

(defn- task! [root fields]
  (let [t (get-in (ws/request!! root :act {:class :task :action :create :args {:title "t"}}) [:result :object])]
    (doseq [[k [type v]] fields]
      (ws/request!! root :set-field {:object t :field k :type type :value v :reader :both}))
    t))

(defn- result-of [t]
  {:object/fields {:message/kind {:field/value :approval-result} :message/task {:field/value t}}})

(deftest what-a-card-did-reaches-the-work-log
  (fx/with-world [{:keys [world root]}]
    (let [approved #'loop/approved-result
          edit-op {:op "act" :args {:action "file.edit" :args {:name "p/a.c" :old "x" :new "y"}}}
          edit-card (task! root {:task/op [:map edit-op] :task/result [:map {:ok true :text "ok {}"}]})
          run-t (task! root {:task/exit [:number 2] :task/command [:string "make listener"]
                             :task/stderr [:string "a.c:9: error: no such thing"]
                             :task/failing [:string "a.c:9: error: no such thing"]})
          run-card (task! root {:task/op [:map {:op "act" :args {:action "task.run" :args {:cmd "make listener"}}}]
                                :task/result [:map {:ok true :text "ok {}"}]
                                :task/ran [[:ref :task] run-t]})
          failed-card (task! root {:task/op [:map edit-op]
                                   :task/result [:map {:ok false :text "err no-match: no such file: p/a.c"}]})
          denied-card (task! root {:task/op [:map edit-op]})
          db (act/db world)
          log (-> {:files {"p/a.c" {:text "old" :turn 1}} :order ["p/a.c"] :n 1}
                  (worklog/add [(approved db (result-of edit-card))]))]
      (testing "an approved edit: the file is no longer current, and it must be built"
        (is (= ["p/a.c"] (map :name (:edits log))))
        (is (empty? (:files log)) "what was read of it is out of date")
        (is (zero? (:quiet log)) "a change is not a turn of reading")
        (is (str/includes? (worklog/render log 1 8) "VERIFY")))
      (testing "an approved build that failed engages FIX, with the failing lines"
        (let [log (worklog/add log [(approved db (result-of run-card))])]
          (is (worklog/fixing? log))
          (is (= #{"edit" "read"} (worklog/held-tools log)))
          (is (str/includes? (worklog/render log 1 8) "FIX"))
          (is (str/includes? (worklog/render log 1 8) "a.c:9: error: no such thing"))))
      (testing "an approved op that failed says why"
        (let [r (approved db (result-of failed-card))]
          (is (false? (:ok r)))
          (is (str/includes? (first (:failed (worklog/add nil [r]))) "no such file"))))
      (testing "a decision with no result ran nothing"
        (is (nil? (approved db (result-of denied-card))))))))

(deftest a-wake-at-its-cap-reports-as-its-answer
  (let [said (atom nil)
        log (-> nil (worklog/add [{:op "act" :args {:action "file.edit" :args {:name "p/a.c"}} :ok true :result {:name "p/a.c"}}])
                (worklog/add [{:op "act" :args {:action "task.run" :args {:cmd "make"}} :ok true
                               :result {:exit 2 :command "make" :failing "a.c:1: error: nope"}}]))]
    (with-redefs-fn {#'loop/send-reply! (fn [_ _ _ _ _ text] (reset! said text))}
      #(#'loop/tell-stopped! nil nil {:work? true :max-iterations 8 :max-turns 30} {:log log :done [] :evaluations [:e]}))
    (is (str/includes? @said "Stopped after 8 turns"))
    (is (str/includes? @said "Changed:\n- p/a.c"))
    (is (str/includes? @said "Verified:\n- nothing ran"))
    (is (str/includes? @said "Unfinished:\n- `make` FAILED, exit 2\na.c:1: error: nope"))))

(deftest done-means-the-goal-was-checked
  (let [challenge #'loop/goal-challenge
        ctx {:work? true :m-state {:object/fields {:message/text {:field/value "Make the line stop printing."}}}}
        st {:log {:edits [{:turn 1 :name "p/a.c"}]}}]
    (is (str/includes? (challenge ctx st "Done, the build passes.") "stop printing") "the task, in its words")
    (is (nil? (challenge ctx st "Done.\n\nProved: ran it twice, no line printed.")))
    (is (nil? (challenge ctx st "Not proved: the box has no display.")))
    (is (nil? (challenge ctx (assoc st :goal-asked? true) "Done.")) "asked once")
    (is (nil? (challenge ctx {:log {}} "It is three lines.")) "nothing changed, nothing to prove")
    (is (nil? (challenge {:work? false} st "Done.")) "a question stays a question")))

(deftest a-wake-goes-on-while-it-gets-somewhere
  (let [ctx {:max-iterations 8 :max-turns 30}]
    (is (= 8 (turn-limit ctx {})) "any wake has its eight")
    (is (= 8 (turn-limit ctx {:progress-at 3})))
    (is (= 16 (turn-limit ctx {:progress-at 12})) "a few turns past the last that did something")
    (is (= 30 (turn-limit ctx {:progress-at 29})) "and never past the ceiling")
    (is (= 1 (turn-limit {:max-iterations 1 :max-turns 1} {:progress-at 1})) "a limit a person set holds")))

(deftest a-long-read-is-told-to-get-on
  (let [note #'loop/work-note]
    (is (nil? (note {:i 7 :done [(read-of "a")]})))
    (is (some? (note {:i 8 :done [(read-of "a")]})))
    (is (nil? (note {:i 12 :done [(read-of "a") (edit "a.c")]})) "it has started")
    (is (nil? (note {:i 12 :done [(run "make" 2)]})) "so has this")))

(deftest going-round-is-not-progress
  (let [{:keys [seen fresh?]} (progress #{} [(read-of "a.c")])]
    (is fresh?)
    (is (not (:fresh? (progress seen [(read-of "a.c")]))) "the same read again")
    (is (:fresh? (progress seen [(read-of "b.c")])) "a new one")
    (is (not (:fresh? (progress #{} [{:ok false :op "read" :args {:name "a.c"}}]))) "a failure finds nothing"))
  (testing "edit, test, edit, test is the work"
    (let [s1 (:seen (progress #{} [(run "make test" 1)]))]
      (is (not (:fresh? (progress s1 [(run "make test" 1)]))) "the same run with nothing changed")
      (let [s2 (:seen (progress s1 [(edit "a.c")]))]
        (is (:fresh? (progress s2 [(run "make test" 0)])) "the same run after a change")))))

(deftest done-means-it-passed
  (is (= ["a.c"] (untested {:done [(edit "a.c")]})) "changed and never built")
  (is (= ["a.c"] (untested {:done [(run "make" 0) (edit "a.c")]})) "built before the change")
  (is (= ["a.c"] (untested {:done [(edit "a.c") (run "make" 2)]})) "built, and it failed")
  (is (nil? (untested {:done [(edit "a.c") (run "make" 0)]})))
  (is (nil? (untested {:done [(edit "docs/page.md")]})) "a page has nothing to build")
  (is (= ["a.c"] (untested {:done [] :approved-edit "a.c"})) "a card's edit arrives with the decision")
  (is (nil? (untested {:done [(run "make" 0)] :approved-edit "a.c"})))
  (is (some? (verify {:done [(edit "a.c")]})))
  (is (nil? (verify {:done [(edit "a.c")] :verify-asked? true})) "asked once"))

(deftest a-claim-of-writing-needs-a-write
  (is (= ["star_data_new.h"] (claimed {:done [(read-of "star_data_new.h")]} "I wrote star_data_new.h as a new file.")))
  (is (nil? (claimed {:done [(run "python gen.py" 0)]} "I wrote star_data_new.h")) "a run can write")
  (is (nil? (claimed {:done [(edit "a.c")]} "I created a.c")))
  (is (nil? (claimed {:done []} "The file was read."))))

(deftest forgiving-arguments
  (is (= {:type "string"} (lops/clean-args {:type ":string"})))
  (is (= {:range [1 40]} (lops/clean-args {:range "1,40"})))
  (is (= {:range [1 40]} (lops/clean-args {:range "[1, 40]"}))))

(deftest a-run-finds-its-shell
  (let [run #'lops/task-run-args]
    (is (= {"action" "task.run" "args" {"cmd" "ls" "adapter" "s"}}
           (run nil {"action" "task.run" "object" "s" "args" {"cmd" "ls"}})))
    (is (= {"action" "task.run" "args" {"cmd" "ls" "adapter" "s"}}
           (run nil {"action" "task.run" "adapter" "s" "args" {"cmd" "ls"}})))))

(deftest a-run-on-the-wrong-object-finds-the-shell
  (let [run #'lops/task-run-args]
    (with-redefs-fn {#'lops/sole-shell (fn [_] "the-shell")}
      #(is (= {"action" "task.run" "args" {"cmd" "ls" "adapter" "the-shell"}}
              (run nil {"action" "run" "object" "a-principal" "args" {"cmd" "ls"}}))))))

(deftest a-guessed-action-is-the-tool-it-meant
  (let [tool #'lops/as-tool]
    (is (= {:name "read" :arguments {"name" "a.c"}}
           (tool nil {:name "act" :arguments "{\"action\":\"code.read\",\"args\":{\"path\":\"a.c\"}}"})))
    (is (= "read" (:name (tool nil {:name "read" :arguments "{}"}))))))

(deftest a-project-says-how-it-tests
  (let [d (doto (java.io.File/createTempFile "orbis" "") .delete .mkdirs)]
    (is (nil? (code/test-command (.getPath d))))
    (spit (io/file d "go.mod") "module x")
    (is (= "go test ./..." (code/test-command (.getPath d))))))

(deftest an-agent-cannot-widen-through-root
  (fx/with-world [{:keys [world root witness]}]
    (let [made (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "asker"}})
          asker (get-in made [:result :object])
          refuse #'loop/refuse-widening
          op {:op "act" :args {:action "project.add" :args {:name "x" :path "/tmp/x"}}}]
      (is (= :not-from-a-person (get-in (refuse world {:sender asker} op) [:err :code])))
      (is (nil? (:err (refuse world {:sender (:id witness)} op))) "a person may ask")
      (is (nil? (:err (refuse world {:sender asker} {:op "read" :args {:name "a"}}))) "a read widens nothing"))))

(deftest a-denial-covers-the-end
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          t (get-in (ws/request!! root :act {:class :task :action :create :args {:title "deny"}}) [:result :object])
          put! (fn [k type v] (ws/request!! root :set-field {:object t :field k :type type :value v :reader :both}))]
      (put! :task/requested-by [:ref :principal] p)
      (put! :task/summary :string "run `rm notes.txt` through shell.local")
      (put! :task/reason :string "no")
      (put! :task/decided-at :instant (java.util.Date.))
      (ws/request!! root :move {:task t :to :denied})
      (let [edit {:action "file.edit" :args {:name "proj/notes.txt" :old "x" :new ""}}
            other {:action "file.edit" :args {:name "proj/other.txt" :old "x" :new ""}}
            db (act/db world)]
        (when (= :denied (get-in (objects/fields db t) [:task/state :field/value]))
          (is (= "after-denial" (gates/gated? db p :act edit)) "the same file, another way")
          (is (nil? (gates/denied-before db p :act other)) "another file is another matter")
          (is (nil? (gates/admission db p :act edit nil)) "no rule admits it"))))))
