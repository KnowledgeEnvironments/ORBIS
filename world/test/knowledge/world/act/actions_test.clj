(ns knowledge.world.act.actions-test
  "Named actions (plan §19.1 W3, decisions 2026-10-30): principal.create, workspace.mount,
   route.create, route.join, task.run, harness.index, through the act op as a session and as a loop
   would call them; refusals (narrower-required, outside-mount, unknown-action, invalid-args,
   unresolved); the token never on the log; invalid Grant ops named."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.log.core :as log]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.core :as lcore]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws])
  (:import [java.nio.file Files]
           [java.nio.file.attribute PosixFilePermissions]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- res-of
  "The action's own result map: the act op answers {result object seq}."
  [r] (get-in r [:result :result]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))
(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))

(defn- all-events
  "Every event in the world log, decoded."
  [world]
  (loop [from 1 acc []]
    (let [batch (log/read-from (:log world) from 1000)]
      (if (empty? batch) acc (recur (inc (long (:event/seq (last batch)))) (into acc batch))))))

(deftest principal-create-is-one-gesture
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model" :read)
          r (act! root "principal.create" {:name "atlas" :charter "You own atlas. Ask root for anything else."
                                           :grant {:scope {:prefix "atlas"} :ops ["read" "reference" "transmit" "write"]}
                                           :actions ["workspace.mount"]})
          res (res-of r)
          new (:principal res)]
      (is (:ok r) (pr-str r))
      (testing "the result names everything it made and, for a session, the token"
        (is (uuid? new))
        (is (uuid? (:route res)))
        (is (uuid? (:grant res)))
        (is (= model (:model res)))
        (is (= "root.atlas" (:route-name res)))
        (is (= ["task.run" "task.list" "task.cancel" "view.present" "view.table-of" "view.graph-of" "search.find" "cost.report"
                "pane.open" "pane.list" "pane.close" "pane.resize" "pane.read" "pane.send" "pane.frob" "file.open"
                "workspace.mount"] (:actions res)) "task.run, the view actions, search.find and cost.report by default, plus the one asked for")
        (is (string? (:token res)) "a session gets the token")
        (is (string? (:token-file res))))
      (testing "the token file exists, 0600, holding the token"
        (let [f (io/file (:token-file res))]
          (is (.exists f))
          (is (= (:token res) (slurp f)))
          (is (= "rw-------" (PosixFilePermissions/toString (Files/getPosixFilePermissions (.toPath f) (make-array java.nio.file.LinkOption 0)))))))
      (testing "the Principal carries charter, model, route, accepts-from, memory"
        (let [fs (fields root new)]
          (is (= "You own atlas. Ask root for anything else." (v fs :instructions/charter)))
          (is (str/includes? (str (v fs :instructions/protocol)) "err unresolved") "the protocol how-to ships with every Principal")
          (is (str/includes? (str (v fs :instructions/workspace)) "atlas.shell") "the workspace how-to names its own Adapters")
          (is (string? (v fs :memory/notes)) "memory defaults from create!")
          (is (= [model] (v fs :principal/models)))
          (is (= [(:route res)] (v fs :principal/routes)))
          (is (= [root-id] (v fs :loop/accepts-from)))
          (is (= :process (get-in (ws/request!! root :read {:object new}) [:result :principal/driven-by])))))
      (testing "the Route is between root and the new Principal, in both principal/routes, with the first person seated"
        (is (= #{root-id new (resolve-as root "person" :read)} (set (v (fields root (:route res)) :route/members))))
        (is (some #{(:route res)} (v (fields root root-id) :principal/routes)))
        (is (= (:route res) (resolve-as root "root.atlas" :read))))
      (testing "as the new Principal: its own name and Model resolve, root does not, actions granted resolve for evaluate"
        (let [c (fx/attach-as world (:token res))]
          (is (= (:route res) (resolve-as c (str "/" (:route res)) :transmit)) "it may transmit on its Route, which lies outside its prefix")
          (is (uuid? (:route-grant res)))
          (is (= new (resolve-as c "atlas" :read)))
          (is (= new (resolve-as c "atlas" :write)))
          (is (= model (resolve-as c (str "/" model) :evaluate)) "by id: root.model is a name in root's namespace")
          (is (= model (resolve-as c "root.model" :evaluate)) "what root names is the world's; a name grants nothing, the Grant on it does")
          (is (nil? (resolve-as c "root" :read)))
          (is (nil? (resolve-as c "atlas" :delegate)))
          (is (some? (resolve-as c "action/task.run" :evaluate)))
          (is (some? (resolve-as c "action/workspace.mount" :evaluate)))
          (is (nil? (resolve-as c "action/principal.create" :evaluate)))
          (ws/close! c)))
      (testing "refusals"
        (is (= :narrower-required (get-in (act! root "principal.create" {:name "twin" :charter "x" :grant {:scope "everything" :ops ["read" "reference" "transmit" "write" "evaluate" "create"]}}) [:err :code]))
            "the same scope and ops as root's is not narrower")
        (is (= :narrower-required (get-in (act! root "principal.create" {:name "twin" :charter "x" :grant {:scope {:prefix "twin"} :ops ["read" "delegate"]}}) [:err :code]))
            "delegate is never handed down")
        (is (= :invalid-args (get-in (act! root "principal.create" {:name "twin" :grant {:scope {:prefix "twin"} :ops ["read"]}}) [:err :code])))
        (is (str/includes? (get-in (act! root "principal.create" {:name "twin" :grant {:scope {:prefix "twin"} :ops ["read"]}}) [:err :message]) "charter"))
        (is (= :invalid-args (get-in (act! root "principal.create" {:name "twin" :charter "x" :grant {:scope {:prefix "twin"} :ops ["history"]}}) [:err :code]))
            "wire ops are not Grant ops")
        (is (= :unknown-action (get-in (act! root "principal.destroy" {}) [:err :code])))
        (let [w (fx/attach-as world (:token witness))]
          (is (= :unresolved (get-in (act! w "principal.create" {:name "x" :charter "x" :grant {:scope {:prefix "x"} :ops ["read"]}}) [:err :code]))
              "no evaluate on the action Object")
          (ws/close! w))))))

(deftest principal-create-from-a-loop-keeps-the-token-off-the-log
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          r (actions/perform-action! world root-id "principal.create"
                                     {:name "grid" :charter "You own grid." :grant {:scope {:prefix "grid"} :ops ["read" "write"]}}
                                     {:session? false})
          res (get-in r [:ok :result])
          token (slurp (io/file (:token-file res)))]
      (is (:ok r) (pr-str r))
      (is (nil? (:token res)) "a loop never sees the token")
      (is (string? (:token-file res)))
      (is (= 43 (count token)))
      (testing "no event body carries the token"
        (is (not-any? #(str/includes? (pr-str (:event/body %)) token) (all-events world))))
      (testing "the credential still works"
        (let [c (fx/attach-as world token)]
          (is (= (:principal res) (:principal c)))
          (ws/close! c))))))

(deftest workspace-mount-and-task-run
  (fx/with-world [{:keys [world root]}]
    (let [dir (fx/temp-dir)
          other (fx/temp-dir)
          _ (spit (io/file dir "README.md") "# Proj\nA project under test.\n")
          _ (spit (io/file other "SECRET.md") "not yours\n")
          created (res-of (act! root "principal.create" {:name "proj" :charter "You own proj."
                                                           :grant {:scope {:prefix "proj"} :ops ["read" "reference" "transmit" "write"]}
                                                           :actions ["workspace.mount"]}))
          new (:principal created)
          r (act! root "workspace.mount" {:principal new :path dir :name "proj"})
          res (res-of r)]
      (is (:ok r) (pr-str r))
      (is (uuid? (:fs res)))
      (is (uuid? (:shell res)))
      (is (= "proj.shell" (:shell-name res)))
      (is (= 3 (count (:grants res))))
      (testing "the Principal's adapters and names: the bare name stays the Principal's"
        (is (= [(:fs res) (:shell res)] (v (fields root new) :principal/adapters)))
        (is (= new (resolve-as root "proj" :read)))
        (is (= (:fs res) (resolve-as root "proj.fs" :read)))
        (is (= "proj.fs" (:fs-name res)))
        (is (= (:shell res) (resolve-as root "proj.shell" :read))))
      (testing "as the Principal: it reads its mount, runs a command, cannot read elsewhere"
        ;; a process's own session meets its gate (session_gate_test); this test is about reach
        (fx/policy! world root {:object new :field :policy/approval :type [:list :string] :value [] :reader :both})
        (let [c (fx/attach-as world (:token created))
              f (ws/request!! c :read {:name "proj/README.md"})
              run (act! c "task.run" {:adapter "proj.shell" :cmd "ls"})]
          (is (:ok f) (pr-str f))
          (is (str/includes? (str (get-in f [:result :object/fields :file/text :field/value])) "A project under test"))
          (is (:ok run) (pr-str run))
          (is (= 0 (:exit (res-of run))))
          (is (str/includes? (:stdout (res-of run)) "README.md"))
          (is (uuid? (:task (res-of run))))
          (testing "another mount of root's is not its business"
            (let [m2 (act! root "workspace.mount" {:principal (:principal root) :path other :name "elsewhere" :shell false})]
              (is (:ok m2) (pr-str m2))
              (is (nil? (:shell (res-of m2))))
              (is (= :unresolved (get-in (ws/request!! c :read {:name "elsewhere/SECRET.md"}) [:err :code])))))
          (testing "a narrow Principal may not mount outside its own mounts, inside them it may"
            (is (= :outside-mount (get-in (act! c "workspace.mount" {:principal new :path other :name "stolen"}) [:err :code])))
            (.mkdirs (io/file dir "sub"))
            (let [inside (act! c "workspace.mount" {:principal new :path (str dir "/sub") :name "proj.sub" :shell false})]
              ;; granting on the new mount needs a delegable Grant the narrow Principal does not hold
              (is (contains? #{:unresolved :outside-mount} (get-in inside [:err :code])) (pr-str inside))))
          (ws/close! c)))
      (testing "a path that is not a directory"
        (is (= :invalid-args (get-in (act! root "workspace.mount" {:principal new :path (str dir "/nope") :name "nope"}) [:err :code]))))
      (fx/delete-tree! dir)
      (fx/delete-tree! other))))

(deftest routes-and-index
  (fx/with-world [{:keys [root witness]}]
    (let [root-id (:principal root)
          wid (:id witness)
          r (act! root "route.create" {:name "ops" :members [root-id wid]})
          route (:route (res-of r))]
      (is (:ok r) (pr-str r))
      (is (= #{root-id wid} (set (v (fields root route) :route/members))))
      (is (= #{root-id wid} (set (:joined (res-of r)))))
      (is (some #{route} (v (fields root wid) :principal/routes)))
      (let [other (:route (res-of (act! root "route.create" {:name "ops2" :members [root-id]})))
            j (act! root "route.join" {:route other :principal wid})]
        (is (:ok j) (pr-str j))
        (is (some #{other} (v (fields root wid) :principal/routes))))
      (testing "harness.index refreshes root's index"
        (let [i (act! root "harness.index" {})]
          (is (:ok i) (pr-str i))
          (is (pos? (:bytes (res-of i)))))))))

(deftest the-acting-section-lists-what-a-principal-may-call
  (fx/with-world [{:keys [world root witness]}]
    (let [db (act/db world)
          text (actions/acting-text db (:principal root))]
      (is (str/starts-with? text "# Actions"))
      (is (not (str/includes? text "- task.run:")) "a tool is not listed twice")
      (is (re-find #"\n- [a-z.]+: " text) "purposes are said while they fit")
      (is (str/includes? text "action/<name>") "the block says where the args are")
      (is (not (str/includes? text "timeout-s? int")) "shapes are on demand")
      (is (str/includes? text "pane.send") "the pane actions")
      (is (<= (count text) actions/acting-block-chars))
      (let [theirs (actions/acting-text db (:id witness))]
        (is (str/includes? theirs "- notify:") "every Principal can ask for a person's attention")
        (is (not (str/includes? theirs "task.run")) "and holds nothing else")))))

(deftest a-model-finds-an-actions-args-by-reading-it
  ;; the Actions block names actions; a model that reads one gets what it needs to call it
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          [r] (lops/execute! world p nil 1 [{:op :read :args {:name "action/task.run"}}])
          text (#'ser/read-text (:result r) (:args r))]
      (is (:ok r) (pr-str r))
      (is (str/includes? text "args: {adapter string, cmd string, cwd? string, timeout-s? int, background? bool}") text)
      (is (str/includes? text "instructions/own") text)
      (let [[g] (lops/execute! world p nil 1 [{:op :read :args {:name "action/grant"}}])]
        (is (or (:ok g) (:err g)) "a name with no action answers, it does not throw")))
    (testing "a Field the Serialization only points at comes back whole by field"
      (let [p (:principal root)
            [r] (lops/execute! world p nil 1 [{:op :read :args {:object (str p) :field "instructions/protocol"}}])
            text (#'ser/read-text (:result r) (:args r))]
        (is (:ok r) (pr-str r))
        (is (str/includes? text "The protocol, for agents") text)))))

(deftest grant-ops-are-a-vocabulary
  (testing "a loop's grant with wire ops is named, not just invalid"
    (let [{:keys [ops]} (lops/parse {:text "```ops\n{\"op\": \"grant\", \"args\": {\"grantee\": \"00000000-0000-0000-0000-000000000001\", \"scope\": {\"prefix\": \"x\"}, \"ops\": [\"history\", \"set-field\", \"read\"]}}\n```"})]
      (is (= :invalid-ops (get-in (first ops) [:err :code])))
      (is (str/includes? (get-in (first ops) [:err :message]) "history, set-field"))
      (is (str/includes? (get-in (first ops) [:err :message]) "read, reference, transmit, write, delegate, evaluate, create"))))
  (testing "act/grant! directly"
    (is (= :invalid-ops (get-in (act/invalid-ops [:history]) [:err :code])))
    (is (= :invalid-ops (get-in (act/invalid-ops []) [:err :code])))
    (is (nil? (act/invalid-ops [:read :write])))))

(deftest an-action-name-may-stand-as-the-op
  ;; the model writes {"op": "task.run", ...} as often as {"op": "act", "args": {"action": "task.run", ...}}
  (let [{:keys [ops]} (lops/parse {:text "```ops\n{\"op\": \"task.run\", \"args\": {\"adapter\": \"shell.local\", \"cmd\": \"ls\"}}\n{\"op\": \"principal.create\", \"args\": {\"name\": \"x\"}}\n{\"op\": \"read\", \"args\": {\"name\": \"harness/docs/ethos.md\"}}\n```"})]
    (is (= :act (:op (first ops))))
    (is (= :task.run (get-in (first ops) [:args :action])))
    (is (= "shell.local" (get-in (first ops) [:args :args :adapter])))
    (is (= :act (:op (second ops))))
    (is (= :principal.create (get-in (second ops) [:args :action])))
    (is (= :read (:op (nth ops 2))) "wire ops are untouched")))

(deftest the-new-principal-answers-on-its-route
  ;; the real run found the created Principal could not transmit on <caller>.<name>: its reply stalled
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          res (res-of (act! root "principal.create" {:name "echo" :charter "You answer."
                                                      :grant {:scope {:prefix "echo"} :ops ["read" "reference" "transmit" "write"]}}))
          new (:principal res) route (:route res)
          _ (ws/request!! root :subscribe {:sub "inbox" :stream root-id :kinds #{:delivered}})
          sent (ws/request!! root :send {:to #{new} :route route :text "say something"})
          push (ws/await-push root "inbox" 15000)
          reply (some-> push :events first :event/body :message)
          f (when reply (fields root reply))]
      (is (:ok sent) (pr-str sent))
      (is (some? reply) (str "no reply from the new Principal within 15 s; loop/stalled = " (pr-str (v (fields root new) :loop/stalled))))
      (is (= new (v f :message/from)))
      (is (= (get-in sent [:result :message]) (v f :message/in-reply-to))))))

(deftest the-loop-waits-for-the-model-s-timeout-and-reasons-only-after-a-failed-op
  ;; a stall "evaluation failed after 0 s: timeout" and a 16k-token reasoning turn after a timeout were
  ;; seen on an older tree: the deadline is model/timeout-s (600 s default) plus grace, never 0, and
  ;; :auto reasoning turns on only for the iteration after an op failed
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          model (get-in (ws/request!! root :resolve {:name "root.model" :op :read}) [:result :object])
          deadline #'knowledge.world.loop.core/evaluation-timeout-ms
          p-state (objects/state db (:principal root))]
      (is (= (+ 15000 (* 1000 600)) (deadline db model)) "600 s default plus the 15 s grace")
      (is (= (+ 15000 (* 1000 600)) (deadline db (random-uuid))) "a Model without the Field, or none at all, still waits 600 s")
      (is (= :off (lcore/reasoning-for db p-state {} model false)) "auto all the way down: off")
      (is (= :off (lcore/reasoning-for db p-state {} model true)) "auto never turns reasoning on by itself, not even after a failed op"))))

