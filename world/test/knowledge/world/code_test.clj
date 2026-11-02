(ns knowledge.world.code-test
  "Searching, outlining and editing mounted files without a shell (decisions 2026-10-31)."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.code :as code]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws]))

(defn- put! [dir rel text]
  (let [f (io/file dir rel)]
    (io/make-parents f)
    (spit f text)))

(defn- project! []
  (let [dir (fx/temp-dir)]
    (put! dir "src/boot.cpp" "// loader\nint parseBootSector(char* s) {\n  return 0;\n}\n")
    (put! dir "docs/notes.md" "# Notes\n\nThe boot sector is parsed in C.\n\n## More\n")
    (put! dir "vendor/x/boot.c" "int parse_boot_sector(void);\n")
    (put! dir "build/gen.cpp" "parseBootSector\n")
    (put! dir "bin/blob.dat" "\u0000parseBootSector")
    (put! dir "README.md" "first line\nsecond line\nsecond line\n")
    dir))

(defn- principal! [root n dir]
  (let [_ (ws/request!! root :act {:action :principal.create
                                   :args {:name n :charter "test" :grant {:scope {:prefix n} :ops ["read" "write" "reference" "transmit"]}}})
        id (get-in (ws/request!! root :resolve {:name n :op :read}) [:result :object])
        m (ws/request!! root :act {:action :workspace.mount :args {:principal n :path dir :name n}})]
    (is (:ok m) (pr-str m))
    id))

(deftest search-finds-a-phrase-however-it-is-spelled
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          p (principal! root "proj" dir)
          {:keys [ok]} (code/search! world p {:pattern "boot sector"})
          paths (map :path (:results ok))]
      (is (= "whole" (:matched ok)))
      (is (= "proj/src/boot.cpp" (first paths)) "source first")
      (is (= "proj/vendor/x/boot.c" (last paths)) "others' code last")
      (is (not-any? #(str/includes? % "build/") paths) "a build tree is not the code")
      (is (not-any? #(str/includes? % "blob") paths) "nor is a binary")
      (is (= {:read {:name "proj/src/boot.cpp" :range [1 62]}} (:next ok)) "it says what to read next")
      (testing "words stand in when no line has the phrase, and it says so"
        (let [{:keys [ok]} (code/search! world p {:pattern "sector zebra"})]
          (is (str/starts-with? (:matched ok) "any word"))
          (is (seq (:results ok)))))
      (testing "narrowing"
        (is (= ["proj/docs/notes.md"] (map :path (:results (:ok (code/search! world p {:pattern "boot sector" :path "docs"}))))))
        (is (= ["proj/src/boot.cpp"] (map :path (:results (:ok (code/search! world p {:pattern "boot sector" :glob "*.cpp"})))))))
      (testing "a regular expression, and one that does not compile"
        (is (= 1 (count (:results (:ok (code/search! world p {:pattern "parse[A-Z]\\w+\\(char" :regex true}))))))
        (is (= :invalid-args (get-in (code/search! world p {:pattern "((" :regex true}) [:err :code])))))))

(deftest a-search-stays-inside-what-p-can-read
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          _ (principal! root "proj" dir)
          other (principal! root "other" (fx/temp-dir))]
      (is (empty? (:results (:ok (code/search! world other {:pattern "boot"})))))
      (is (= [] (:results (:ok (code/search! world other {:pattern "boot" :path "proj"}))))
          "another's mount is not a path of mine"))))

(deftest a-creator-reaches-its-projects-by-outline
  (fx/with-world [{:keys [world root]}]
    (principal! root "proj" (project!))
    (let [rows (:mounts (:ok (code/outline! world (:principal root) {})))
          row (first (filter #(= "proj" (str/replace (:name %) #"/$" "")) rows))]
      (is (some? row) (pr-str (map :name rows)))
      (is (some #{"src/"} (:dirs row))))))

(deftest an-outline-shows-the-shape
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          p (principal! root "proj" dir)]
      (is (= [{:line 2 :text "int parseBootSector(char* s) {"}]
             (:outline (:ok (code/outline! world p {:name "proj/src/boot.cpp"})))))
      (is (= [1 5] (map :line (:outline (:ok (code/outline! world p {:name "proj/docs/notes.md"}))))))
      (is (= ["bin/" "docs/" "src/" "vendor/"] (map :name (:dirs (:ok (code/outline! world p {:name "proj"})))))))))

(deftest an-edit-is-exact
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          p (principal! root "proj" dir)
          readme (io/file dir "README.md")
          before (slurp readme)]
      (testing "a miss and a double change nothing, and say why"
        (is (= :no-match (get-in (code/edit! world p {:name "proj/README.md" :old "absent" :new "x"}) [:err :code])))
        (let [r (code/edit! world p {:name "proj/README.md" :old "second line" :new "x"})]
          (is (= :ambiguous (get-in r [:err :code])))
          (is (str/includes? (get-in r [:err :message]) "lines 2, 3")))
        (is (= before (slurp readme))))
      (testing "one match"
        (let [{:keys [ok]} (code/edit! world p {:name "proj/README.md" :old "first line" :new "the $1 line"})]
          (is (= "the $1 line\nsecond line\nsecond line\n" (slurp readme)) "replacement text is literal")
          (is (= "@@ line 1 @@\n-first line\n+the $1 line" (:diff ok)))))
      (testing "at top is one line, no more"
        (code/edit! world p {:name "proj/README.md" :at "top" :new "<!-- reviewed -->\n"})
        (is (= "<!-- reviewed -->\nthe $1 line\nsecond line\nsecond line\n" (slurp readme))))
      (testing "at wins over an old sent beside it: the heading stays"
        (code/edit! world p {:name "proj/README.md" :at "top" :old "<!-- reviewed -->" :new "<!-- again -->"})
        (is (str/starts-with? (slurp readme) "<!-- again -->\n<!-- reviewed -->\n")))
      (testing "all"
        (code/edit! world p {:name "proj/README.md" :old "second line" :new "two" :all true})
        (is (= 2 (count (re-seq #"two" (slurp readme))))))
      (testing "a new file"
        (is (true? (get-in (code/edit! world p {:name "proj/new/dir.txt" :new "hello\n"}) [:ok :created])))
        (is (= "hello\n" (slurp (io/file dir "new/dir.txt")))))
      (testing "out of the mount"
        (is (= :unresolved (get-in (code/edit! world p {:name "proj/../outside.txt" :new "x"}) [:err :code])))
        (is (not (.exists (io/file (.getParentFile (io/file dir)) "outside.txt")))))
      (testing "the log has the diff"
        (let [db (act/db world)
              o (objects/lookup db (objects/world-id db) "proj/README.md")]
          (is (str/includes? (str (get-in (objects/fields db o) [:file/last-edit :field/value])) "+two")))))))

(deftest a-name-the-tree-does-not-have-is-flagged
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          _ (put! dir "src/Log.hpp" "namespace log {\nvoid info(int);\nvoid warn(int);\n}\nenum class Level { Info, Warn };\n")
          p (principal! root "proj" dir)
          edit (fn [new] (code/edit! world p {:name "proj/src/boot.cpp" :old "  return 0;" :new (str "  " new "\n  return 0;")}))
          flagged (fn [r] (get-in r [:ok :unknown-names]))]
      (testing "a member the tree has nowhere, of a namespace it knows: the guess every round made"
        (let [u (flagged (edit "log::debug(1);"))]
          (is (= 1 (count u)) (pr-str u))
          (is (str/includes? (first u) "log::debug"))
          (is (str/includes? (first u) "`log`"))))
      (testing "an enumerator the tree has nowhere"
        (is (str/includes? (first (flagged (edit "Level l = Level::Debug;"))) "Level::Debug")))
      (testing "a member the tree has, and a library's namespace, pass in silence"
        (is (nil? (flagged (edit "log::info(1);"))))
        (is (nil? (flagged (edit "std::puts(\"x\");")))))
      (testing "a name the file already had is not new"
        (is (nil? (flagged (edit "log::debug(2);")))) "the first edit put it in the file"))))

(deftest an-edit-is-behind-the-same-gate-as-a-shell
  (fx/with-world [{:keys [world root]}]
    (let [p (principal! root "proj" (project!))
          args {:action "file.edit" :args {:name "proj/README.md" :old "first line" :new "x"}}]
      (is (= "fs:write" (gates/gated? (act/db world) p :act args)))
      (let [[r] (lops/execute! world p nil [{:op :act :args args}])]
        (is (= :approval-pending (some-> (get-in r [:err :code]) name keyword)) (pr-str r))))))

(deftest a-bare-path-is-the-file-in-my-own-mount
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          p (principal! root "proj" dir)
          edit (fn [n & {:as more}] {:action "file.edit" :args (merge {:name n} more)})
          q (fn [a] (:args (code/qualified (act/db world) p :act a)))]
      (is (= "proj/src/boot.cpp" (:name (q (edit "src/boot.cpp" :old "loader" :new "x")))) "an existing file")
      (is (= "proj/src/new.cpp" (:name (q (edit "src/new.cpp" :new "x")))) "a new file in a directory that exists")
      (is (= "proj/src/boot.cpp" (:name (q (edit "proj/src/boot.cpp" :old "loader" :new "x")))) "already right")
      (is (= "proj/src/boot.cpp" (:name (q (edit (str dir "/src/boot.cpp") :old "loader" :new "x")))) "absolute, inside the mount")
      (testing "a new file in a directory that is not there is not guessed at"
        (is (= "memory/notes" (:name (q (edit "memory/notes" :new "x")))))
        (is (str/includes? (get-in (code/edit-refusal (act/db world) p {:name "memory/notes" :new "x"}) [:err :message])
                           "`proj/memory/notes`")))
      (testing "the card names the qualified path, and the edit lands"
        (let [[r] (lops/execute! world p nil [{:op :act :args (edit "src/boot.cpp" :old "loader" :new "reader")}])]
          (is (= :approval-pending (some-> (get-in r [:err :code]) name keyword)) (pr-str r))
          (is (= "proj/src/boot.cpp" (get-in r [:args :args :name])))))
      (testing "edit! takes the bare form too: an op stored before this ran still lands"
        (is (:ok (code/edit! world p {:name "docs/notes.md" :old "# Notes" :new "# Notes\n"})))
        (is (str/includes? (slurp (io/file dir "docs/notes.md")) "# Notes\n\n")))
      (testing "a read as well"
        (is (= "proj/README.md" (:name (code/qualified (act/db world) p :read {:name "README.md"})))))
      (testing "the Adapter's name for the mount's"
        (is (= "proj/README.md" (:name (code/qualified (act/db world) p :read {:name "proj.fs/README.md"}))))
        (is (= "proj/README.md" (:name (code/qualified (act/db world) p :read {:name "proj.fs/proj/README.md"})))))
      (testing "a new file named with `at` is still a new file"
        (is (:ok (code/edit! world p {:name "proj/src/fresh.c" :new "int x;\n" :at "end"})))
        (is (= "int x;\n" (slurp (io/file dir "src/fresh.c"))))))))

(deftest an-op-reads-the-way-a-model-writes-it
  (fx/with-world [{:keys [world root]}]
    (let [p (principal! root "proj" (project!))
          wake {:db (act/db world) :p p}
          [op] (:ops (lops/parse {:text "{\"op\":\"edit\",\"args\":{\"name\":\"proj/new.c\",\"new\":\"x\"}}"} wake))]
      (testing "a bare op line, outside any fence"
        (is (= "file.edit" (some-> (get-in op [:args :action]) name)) (pr-str op)))
      (let [id "11c05d29-fc3d-413a-a8e8-4ba08597669b"
            one (fn [n] (first (:ops (lops/parse {:tool-calls [{:name "read" :arguments (str "{\"name\":\"" n "\"}")}]} {:db (act/db world) :p p :ids {}}))))]
        (testing "an Object's id, written as a name"
          (is (= id (str (get-in (one (str "/" id)) [:args :object]))) (pr-str (one (str "/" id))))
          (let [r (one (str id "/task/stdout"))]
            (is (= id (str (get-in r [:args :object]))) (pr-str r))
            (is (= "task/stdout" (some-> (get-in r [:args :field]) str (str/replace #"^:" "")))))
          (is (= "proj/README.md" (get-in (one "proj/README.md") [:args :name])) "a path stays a path"))))))

(deftest an-edit-that-cannot-land-raises-no-card
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          p (principal! root "proj" dir)
          edit (fn [a] (first (lops/execute! world p nil [{:op :act :args {:action "file.edit" :args a}}])))
          pending? (fn [r] (= :approval-pending (some-> (get-in r [:err :code]) name keyword)))]
      (testing "a path in no mount says the exact form and what is close"
        (let [r (edit {:name "elsewhere/boot.cpp" :old "loader" :new "x"})
              m (get-in r [:err :message])]
          (is (not (pending? r)) (pr-str r))
          (is (= :unresolved (some-> (get-in r [:err :code]) name keyword)))
          (is (str/includes? m "`proj/elsewhere/boot.cpp`") m)
          (is (str/includes? m "`proj/src/boot.cpp`") m)))
      (testing "a missing file under the mount"
        (let [r (edit {:name "proj/src/boot.cc" :old "loader" :new "x"})]
          (is (not (pending? r)))
          (is (= :no-match (some-> (get-in r [:err :code]) name keyword)))))
      (testing "text that is not there, before a person sees it"
        (let [r (edit {:name "proj/README.md" :old "third line" :new "x"})]
          (is (not (pending? r)) (pr-str r))
          (is (some-> (get-in r [:err :code]) some?))))
      (testing "an edit that changes nothing"
        (is (not (pending? (edit {:name "proj/README.md" :old "first line" :new "first line"})))))
      (testing "a sound edit still goes to the person"
        (is (pending? (edit {:name "proj/README.md" :old "first line" :new "x"})))))))

(deftest an-approved-edit-that-fails-says-why
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          p (principal! root "proj" dir)]
      (testing "the file moved between the card and the decision"
        (let [{:keys [err]} (code/edit! world p {:name "proj/gone/boot.cpp" :old "loader" :new "x"})]
          (is (= :no-match (:code err)))
          (is (str/includes? (:message err) "`proj/src/boot.cpp`") (:message err)))))))

(deftest tools-read-as-the-model-means-them
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          p (principal! root "proj" dir)
          wake {:db (act/db world) :p p}
          one (fn [n a] (first (:ops (lops/parse {:text "" :tool-calls [{:name n :arguments a}]} wake))))]
      (is (= "code.search" (name (get-in (one "grep" "{\"pattern\":\"boot\"}") [:args :action]))))
      (is (= "file.edit" (name (get-in (one "edit" "{\"name\":\"proj/README.md\",\"new\":\"x\"}") [:args :action]))))
      (testing "run finds the one shell"
        (let [o (one "run" "{\"cmd\":\"ls\"}")]
          (is (= "task.run" (name (get-in o [:args :action]))))
          (is (string? (get-in o [:args :args :adapter])))))
      (testing "the op as the tool's name, and args nested in args: seen live, each cost a turn"
        (is (= :read (:op (one "read" "{\"args\":\"{\\\"name\\\":\\\"proj/README.md\\\"}\"}"))))
        (is (= "proj/README.md" (get-in (one "read" "{\"args\":\"{\\\"name\\\":\\\"proj/README.md\\\"}\"}") [:args :name])))
        (is (= :read (:op (one "{\"op\":\"read\",\"args\":{\"name\":\"proj/README.md\"}}" "{}"))))))))

(deftest a-file-read-shows-what-was-asked-for-with-its-line-numbers
  (let [read-text #'knowledge.world.serialize.core/read-text
        field (fn [v] {:field/value v})
        result (fn [text] {:object/id "x" :object/class :object
                           :object/fields {:file/path (field "src/a.c") :file/text (field text) :file/range (field [100 900])
                                           :file/lines (field 2000) :file/mime (field "text/x-csrc")}})
        args {:name "m/src/a.c" :range [100 900]}]
    (testing "a range past 600 chars, the clip that hid line 1802 from four reads running"
      (let [t (read-text (result (str/join "\n" (repeat 40 "0123456789 0123456789 0123456789"))) args)]
        (is (str/starts-with? t "file src/a.c lines 100-139 of 2000\n100: 0123"))
        (is (str/includes? t "139: 0123"))
        (is (not (str/includes? t "[... next")))))
    (testing "past the cap it says where to go on from"
      (let [t (read-text (result (str/join "\n" (repeat 800 "0123456789 0123456789 0123456789"))) args)]
        (is (<= (count t) 6400))
        (is (re-find #"\[\.\.\. next: read this name with range \[\d+, \d+\]\]" t))))))

(deftest a-project-says-how-it-is-tested
  (let [dir (fx/temp-dir)]
    (is (nil? (code/test-command dir)))
    (spit (io/file dir "Makefile") "test:\n")
    (is (= "make test" (code/test-command dir)))
    (spit (io/file dir "CMakeLists.txt") "")
    (is (= "cmake -S . -B build && cmake --build build && ctest --test-dir build" (code/test-command dir)))
    (spit (io/file dir "CMakePresets.json") "{\"testPresets\":[{\"name\":\"debug\"}]}")
    (is (= "cmake --preset debug && cmake --build --preset debug && ctest --preset debug" (code/test-command dir)))))

(deftest a-shell-carries-its-test-command-to-the-prompt
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          _ (spit (io/file dir "Makefile") "test:\n")
          p (principal! root "proj" dir)
          text (knowledge.world.act.actions/acting-text (act/db world) p)]
      (is (str/includes? text "tests: `make test`") text))))

(deftest a-line-written-with-the-line-below-it-is-not-doubled
  (fx/with-world [{:keys [world root]}]
    (let [dir (project!)
          p (principal! root "proj" dir)
          _ (put! dir "a.md" "# Title\n\nbody\n")
          e (code/edit! world p {:name "proj/a.md" :at "top" :old "# Title" :new "<!-- c -->\n# Title"})]
      (is (:ok e) (pr-str e))
      (is (= "<!-- c -->\n# Title\n\nbody\n" (slurp (io/file dir "a.md"))))
      (testing "with no old, the file's own first line counts"
        (code/edit! world p {:name "proj/a.md" :at "top" :new "<!-- d -->\n<!-- c -->"})
        (is (= "<!-- d -->\n<!-- c -->\n# Title\n\nbody\n" (slurp (io/file dir "a.md"))))))))

(deftest looking-twice-in-a-wake-is-looking-at-two-things
  (fx/with-world [{:keys [world root]}]
    (let [p (principal! root "proj" (project!))
          wake {:db (act/db world) :p p}
          call (fn [a] (first (:ops (lops/parse {:text "" :tool-calls [{:name "outline" :arguments a}]} wake))))
          first-one (first (lops/execute! world p nil 1 [(call "{\"name\":\"proj\"}")] {}))
          second-one (first (lops/execute! world p nil 1 [(call "{\"name\":\"proj/src\"}")] {:done [first-one]}))]
      (is (= "proj/src" (get-in second-one [:result :result :name])) "not the first outline again")
      (is (not (:repeated second-one))))))

(deftest a-repeat-is-the-same-edit-or-a-run-with-nothing-changed
  (let [edit (fn [old] {:action "file.edit" :args {:name "a.c" :old old :new ""}})
        run {:action "task.run" :args {:adapter "x" :cmd "make test"}}
        did (fn [a] {:op "act" :args a :ok true})]
    (is (nil? (lops/repeat-of [(did (edit "one"))] :act (edit "two"))) "two pieces of one file")
    (is (some? (lops/repeat-of [(did (edit "one"))] :act (edit "one"))))
    (is (some? (lops/repeat-of [(did run)] :act run)) "nothing changed")
    (is (some? (lops/repeat-of [(did run) {:op "read" :ok true :args {:name "a.c"}}] :act run)))
    (is (nil? (lops/repeat-of [(did run) (did (edit "one"))] :act run)) "the fix wants testing")
    (is (nil? (lops/repeat-of [(did run) (did {:action "task.run" :args {:adapter "x" :cmd "sed -i x a.c"}})] :act run)))))

(deftest a-read-the-model-can-no-longer-see-is-read-again
  (fx/with-world [{:keys [world root]}]
    (let [p (principal! root "proj" (project!))
          op {:op :read :args {:name "proj/README.md"}}
          [one] (lops/execute! world p nil 1 [op] {:shown []})
          again #(first (lops/execute! world p nil 1 [op] {:done [one] :shown %}))]
      (is (:repeated (again [one])) "last turn's read is there")
      (is (not (:repeated (again []))) "older than that, it is not")
      (is (not (:repeated (again [(again [one])]))) "and being told so shows nothing"))))
