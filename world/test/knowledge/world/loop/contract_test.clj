(ns knowledge.world.loop.contract-test
  "The contract an agent actually meets: tool schemas, the shapes it sends, the errors it gets back.
   Written from root's own onboarding report on a real model (decisions 2026-11-01)."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.adapters.model-api :as model]
            [knowledge.world.adapters.responses :as responses]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.core]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws]))

(defn- project! []
  (let [dir (fx/temp-dir)]
    (spit (io/file dir "README.md") "first line\nread {object here\nthird line\n")
    dir))

(defn- mounted! [root n dir]
  (ws/request!! root :act {:action :principal.create
                           :args {:name n :charter "test" :grant {:scope {:prefix n} :ops ["read" "write" "reference"]}}})
  (let [id (get-in (ws/request!! root :resolve {:name n :op :read}) [:result :object])]
    (is (:ok (ws/request!! root :act {:action :workspace.mount :args {:principal n :path dir :name n}})))
    id))

(defn- tool [nm] (some #(when (= nm (get-in % [:function :name])) (:function %)) model/op-tools))

(defn- parsed [wake & calls]
  (:ops (lops/parse {:text "" :tool-calls (vec (for [[n a] (partition 2 calls)] {:name n :arguments (j/write-value-as-string a)}))} wake)))

(deftest the-responses-route-is-not-strict
  (testing "strict makes every property required, and the model fills what it does not mean with blanks"
    (let [fns (:tools (first (responses/tools model/op-tools)))]
      (is (seq fns))
      (is (every? #(false? (:strict %)) fns)))))

(deftest a-blank-is-not-an-argument
  (fx/with-world [{:keys [world root]}]
    (let [p (mounted! root "proj" (project!))
          wake {:db (act/db world) :p p :ids {}}]
      (testing "what GPT sent, four times running"
        (let [[o] (parsed wake "read" {"object" "" "name" "proj/README.md" "field" "" "range" []})]
          (is (nil? (:err o)) (pr-str o))
          (is (= {:name "proj/README.md"} (:args o)))))
      (testing "a count of nothing is no count, in an action's own args too"
        (let [[o] (parsed wake "history" {"object" "" "thread" "" "last" 0 "after" 0 "limit" 0})]
          (is (nil? (:err o)) (pr-str o))
          (is (= {} (:args o))))
        (let [[o] (parsed wake "run" {"cmd" "ls" "adapter" "" "cwd" "" "timeout-s" 0})]
          (is (= {:cmd "ls"} (select-keys (get-in o [:args :args]) [:cmd :cwd :timeout-s])) (pr-str o))))
      (testing "act around act is the one call"
        (let [[o] (parsed wake "act" {"action" "code.search" "args" {"action" "code.search" "args" {"pattern" "x"}}})]
          (is (= "x" (get-in o [:args :args :pattern])) (pr-str o))))
      (testing "where blank says something, it stays"
        (is (= "" (get-in (first (parsed wake "set-field" {"object" "proj" "field" "memory/notes" "type" "string" "value" ""})) [:args :value])))
        (is (= "" (get-in (first (parsed wake "edit" {"name" "proj/README.md" "old" "x" "new" ""})) [:args :args :new])))))))

(deftest a-reference-is-a-handle-a-name-or-an-id
  (fx/with-world [{:keys [world root]}]
    (let [p (mounted! root "proj" (project!))
          table {"a1" (str p)}
          wake {:db (act/db world) :p p :ids table}
          id-of #(get-in (first (parsed wake "read" {"object" %})) [:args :object])]
      (is (= p (id-of "a1")) "a handle")
      (is (= p (id-of "proj")) "a bound name")
      (is (= p (id-of (str p))) "a full id")
      (testing "a path in `object` is a read by name"
        (is (= {:name "proj/README.md"} (:args (first (parsed wake "read" {"object" "proj/README.md"}))))))
      (testing "history takes an Object as every other tool does"
        (is (= p (get-in (first (parsed wake "history" {"object" "a1"})) [:args :stream]))))
      (testing "no error says otherwise"
        (doseq [f ["src/knowledge/world/loop/ops.clj" "src/knowledge/world/serialize/core.clj"]]
          (is (not (str/includes? (slurp f) "never names")) f))))))

(deftest every-corrected-request-is-one-a-model-can-send
  (doseq [[k [nm args]] lops/corrected]
    (let [f (tool nm)
          known (set (map name (keys (get-in f [:parameters :properties]))))]
      (is f (str k))
      (is (every? known (keys args)) (str k " " args))
      (is (every? (set (keys args)) (get-in f [:parameters :required])) (str k " " args))))
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          wake {:db (act/db world) :p p :ids {}}
          err (fn [line] (:err (first (:ops (lops/parse {:text (str "```ops\n" line "\n```")} wake)))))]
      (testing "an error quotes a tool call, not the wire"
        (is (str/includes? (:message (err "{\"op\":\"nonsense\",\"args\":{}}")) "run {\"cmd\""))))))

(deftest a-call-missing-what-it-needs-says-so-before-anything-runs
  (fx/with-world [{:keys [world root]}]
    (let [wake {:db (act/db world) :p (:principal root) :ids {}}
          ops (parsed wake "edit" {"name" "" "old" "x"} "grep" {} "run" {"cmd" ""})]
      (testing "several in one turn come back together"
        (is (= 3 (count ops)))
        (is (every? #(= :invalid-args (get-in % [:err :code])) ops) (pr-str ops)))
      (is (str/includes? (get-in (first ops) [:err :message]) "edit needs name and new"))
      (is (str/includes? (get-in (first ops) [:err :message]) "Its arguments are")))))

(deftest every-action-is-told-or-is-a-tool
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          p (:principal root)
          told (actions/acting-text db p)
          tools (set (map #(get-in % [:function :name]) model/op-tools))
          by-tool (set (vals @#'lops/tool-actions))
          hidden @#'actions/as-tools]
      (testing "a hidden action has a tool of its own"
        (is (every? by-tool hidden) (pr-str (remove by-tool hidden)))
        (is (every? tools (keys @#'lops/tool-actions))))
      (testing "everything else a Principal may call is named"
        (doseq [{nm :name} (actions/available db p)
                :when (not (or (hidden nm) (actions/people-only nm)))]
          (is (str/includes? told nm) nm))))))

(deftest a-citation-from-a-wake-that-read-nothing-is-sent-back
  (let [cold @#'knowledge.world.loop.core/cold-citation]
    (is (cold {:done [] :failed []} "1. harness/README.md:1 says ORBIS"))
    (is (nil? (cold {:done [{:op :read}] :failed []} "harness/README.md:1 says ORBIS")) "it read something")
    (is (nil? (cold {:done [] :failed [] :cold? true} "harness/README.md:1")) "asked once")
    (is (nil? (cold {:done [] :failed []} "Hello, what can I do for you?")))))

(deftest grep-is-literal-unless-asked
  (fx/with-world [{:keys [world root]}]
    (let [p (mounted! root "proj" (project!))
          search (fn [args] (:result (first (lops/execute! world p nil
                                                           (parsed {:db (act/db world) :p p :ids {}} "grep" args)))))
          hits (fn [args] (:files-matching (or (:result (search args)) (search args))))]
      (is (pos? (hits {"pattern" "read {object"})) "seen live: a brace read as a regular expression")
      (is (pos? (hits {"pattern" "fir.t line" "regex" true})))
      (is (pos? (hits {"pattern" "fir.t line"})) "what only makes sense as a pattern still finds it"))))

(defn- steps
  "The tool calls in the first-steps topic: its indented lines."
  []
  (for [l (str/split-lines (slurp (io/resource "knowledge/world/manual/concepts/first-steps.md")))
        :let [[_ tool args] (re-find #"^    ([a-z-]+) (\{.*\})$" l)]
        :when tool]
    [tool (j/read-value args)]))

(deftest the-first-steps-work-as-written
  (fx/with-world [{:keys [world root witness]}]
    (let [dir (project!)
          p (:principal root)
          calls (steps)
          fix (fn [[tool a]]
                ;; the topic's harness is this test's project, the only thing that changes
                [tool (cond-> a
                        (get a "name") (update "name" str/replace "harness/" "proj/")
                        (= "grep" tool) (assoc "path" "proj"))])
          _ (mounted! root "proj" dir)
          wake (fn [] {:db (act/db world) :p p :ids {}})
          results (vec (for [c (map fix calls)]
                         (first (lops/execute! world p nil (apply parsed (wake) c)))))]
      (is (= ["outline" "read" "grep" "read" "set-field" "run" "history"] (map first calls)))
      (testing "each is taken at its word"
        (doseq [[c r] (map vector calls results)
                :when (not= "run" (first c))]
          (is (:ok r) (pr-str c r))))
      (testing "the command waits for a person"
        (let [r (nth results 5)]
          (is (= :approval-pending (some-> (get-in r [:err :code]) name keyword)) (pr-str r))))
      (testing "a line of a file is one read away, as a citation asks"
        (let [r (first (lops/execute! world p nil (parsed (wake) "read" {"name" "proj/README.md" "range" [2 2]})))]
          (is (:ok r) (pr-str r))
          (is (str/includes? (pr-str r) "read {object here"))))
      (testing "what was done is in the record"
        (is (str/includes? (pr-str (last results)) "memory/notes"))))))
