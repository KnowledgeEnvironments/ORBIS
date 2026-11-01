(ns knowledge.world.developer-principals-test
  "A developer made from the template gets its subtree mounted by principal.create
   itself; claims about other Principals' runs are checked against their Tasks; a reply to the
   creator after unresolved ops under the Principal's own name is a request carrying what it lacks;
   history {thread} lists a thread's Messages."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [datascript.core :as d]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.core :as lcore]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))
(defn- field [c m k] (v (fields c m) k))
(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- json-line [op args] (str "{\"op\": \"" op "\", \"args\": " args "}"))

(defn- poll
  "f over the live db until it answers, or nil after timeout-ms."
  [world f timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop []
      (or (f (act/db world))
          (when (< (System/currentTimeMillis) deadline) (Thread/sleep 250) (recur))))))

(defn- tasks-of [db q]
  (->> (d/q '[:find ?id ?s :in $ ?q :where [?o :object/class :task] [?o :object/created-by ?q] [?o :object/id ?id] [?o :object/created-seq ?s]] db q)
       (sort-by second) (mapv first)))

(defn- messages-from [db q]
  (->> (d/q '[:find ?id ?s :in $ ?q :where [?f :field/name :message/from] [?f :field/value ?q]
              [?o :object/fields ?f] [?o :object/class :message] [?o :object/id ?id] [?o :object/created-seq ?s]] db q)
       (sort-by second) (mapv first)))

(deftest a-developer-is-made-with-its-subtree-mounted
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          db (act/db world)]
      (testing "the summary a person decides on names the subtree"
        (is (str/includes? (gates/summary db :act {:action "principal.create"
                                                   :args {:name "ldev" :subtree "listener" :grant {:scope {:prefix "ldev"} :ops ["read"]}}})
                           "developer of listener, mounted as ldev")))
      (testing "a developer needs its subtree; anyone else needs a charter"
        (is (= :invalid-args (get-in (act! root "principal.create" {:name "nodev" :template "developer" :grant {:scope {:prefix "nodev"} :ops ["read"]}}) [:err :code])))
        (is (= :invalid-args (get-in (act! root "principal.create" {:name "nochart" :grant {:scope {:prefix "nochart"} :ops ["read"]}}) [:err :code])))
        (is (= :invalid-args (get-in (act! root "principal.create" {:name "nodir" :template "developer" :subtree "no-such-dir" :grant {:scope {:prefix "nodir"} :ops ["read"]}}) [:err :code]))))
      (let [created (act! root "principal.create" {:name "ldev" :template "developer" :subtree "listener" :check "ctest --preset macos-debug"
                                                   :grant {:scope {:prefix "ldev"} :ops ["read" "write" "transmit"]}})
            res (get-in created [:result :result])
            ldev (:principal res)]
        (is (:ok created) (pr-str created))
        (testing "the charter is the shipped template, filled by the kernel"
          (let [charter (str (field root ldev :instructions/charter))]
            (is (str/starts-with? charter "You are `ldev`") charter)
            (is (str/includes? charter "You own `listener`"))
            (is (str/includes? charter "ctest --preset macos-debug"))
            (is (not (str/includes? charter "<name>")))))
        (testing "the subtree is mounted for the new Principal, as its own name, with a shell at the repository root"
          (is (= "listener" (:subtree res)) (pr-str res))
          (is (map? (:mount res)))
          (is (uuid? (resolve-as root "ldev.fs" :read)))
          (is (uuid? (resolve-as root "ldev.shell" :read)))
          (is (str/ends-with? (str (get-in res [:mount :path])) "/listener"))
          (is (string? (get-in res [:mount :shell-cwd])) "the shell runs at the repository root")
          (let [r (act! root "task.run" {:adapter "ldev.shell" :cmd "ls listener/CMakeLists.txt docs/protocol.md"})]
            (is (= 0 (get-in r [:result :result :exit])) (pr-str r))))
        (testing "ldev itself reads its subtree by its own name and nothing of the world's"
          (let [c (fx/attach-as world (:token res))]
            (is (uuid? (get-in (ws/request!! c :read {:name "ldev/CMakeLists.txt"}) [:result :object/id])))
            (is (= :unresolved (get-in (ws/request!! c :read {:name "harness/world/deps.edn"}) [:err :code])))))))))

(deftest claims-about-another-principals-runs-are-checked
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          ghost (act! root "principal.create" {:name "ghost" :charter "you haunt" :grant {:scope {:prefix "ghost"} :ops ["read"]}})
          _ (is (:ok ghost) (pr-str ghost))
          db (act/db world)]
      (testing "a claim about a Principal that has run nothing"
        (is (str/includes? (str (lcore/other-run-contradiction db root-id "ghost reported 14 tests, 0 failures earlier.")) "has run nothing"))
        (is (nil? (lcore/other-run-contradiction db root-id "ghost is idle and said hello.")) "no run claim, no contradiction")
        (is (nil? (lcore/other-run-contradiction db root-id "nobody here reported 14 tests")) "no Principal named"))
      (testing "a claim against a Task the other Principal really ran"
        (let [h (act! root "principal.create" {:name "helper" :charter "you help" :grant {:scope {:prefix "helper"} :ops ["read" "write" "transmit"]}})
              helper (get-in h [:result :result :principal])
              route (get-in h [:result :result :route])
              _ (is (:ok h) (pr-str h))
              m (act! root "workspace.mount" {:principal (str helper) :path "harness/world" :name "helper"})
              _ (is (:ok m) (pr-str m))
              _ (fx/policy! world root {:object helper :field :policy/approval :type [:list :string] :value [] :reader :both})
              sent (ws/request!! root :send {:to #{helper} :route route
                                             :text (str "run this:\n" (ops-block (json-line "act" "{\"action\": \"task.run\", \"args\": {\"adapter\": \"helper.shell\", \"cmd\": \"echo 3 tests, 1 failures; exit 2\"}}")))})
              _ (is (:ok sent) (pr-str sent))
              task (poll world (fn [db] (some (fn [t] (when (some? (get-in (objects/fields db t) [:task/exit :field/value])) t)) (tasks-of db helper))) 40000)
              db (act/db world)]
          (is (uuid? task) "helper ran the command through its own shell")
          (is (= 2 (get-in (objects/fields db task) [:task/exit :field/value])))
          (is (str/includes? (str (lcore/other-run-contradiction db root-id "helper's checks all passed, 0 failures.")) "exited 2"))
          (is (str/includes? (str (lcore/other-run-contradiction db root-id "helper reported exit 0 on its run.")) "exited 2"))
          (is (str/includes? (str (lcore/other-run-contradiction db root-id "helper counted 14 tests in its run.")) "no such count"))
          (is (nil? (lcore/other-run-contradiction db root-id "helper ran its check and it exited 2 (3 tests, 1 failures).")) "consistent with the Task"))))))

(deftest a-request-to-the-creator-carries-what-it-lacks
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          h (act! root "principal.create" {:name "needy" :charter "you need things" :grant {:scope {:prefix "needy"} :ops ["read" "write" "transmit"]}})
          needy (get-in h [:result :result :principal])
          route (get-in h [:result :result :route])
          _ (is (:ok h) (pr-str h))
          _ (fx/policy! world root {:object needy :field :policy/approval :type [:list :string] :value [] :reader :both})
          sent (ws/request!! root :send {:to #{needy} :route route
                                         :text (str "read your README:\n" (ops-block (json-line "read" "{\"name\": \"needy/README.md\"}")
                                                                                    (json-line "act" "{\"action\": \"task.run\", \"args\": {\"adapter\": \"needy.shell\", \"cmd\": \"ls\"}}")))})
          _ (is (:ok sent) (pr-str sent))
          reply (poll world (fn [db] (some (fn [m] (when (seq (get-in (objects/fields db m) [:message/lacking :field/value])) m)) (messages-from db needy))) 40000)
          db (act/db world)]
      (is (uuid? reply) (pr-str (map #(objects/fields db %) (messages-from db needy))))
      (let [f (objects/fields db reply)
            lacking (get-in f [:message/lacking :field/value])]
        (testing "the reply to the creator is a request carrying the ops that failed under its own name"
          (is (= :request (some-> (get-in f [:message/kind :field/value]) name keyword)))
          (is (contains? (set (get-in f [:message/to :field/value])) root-id))
          (is (= #{"needy/README.md" "needy.shell"} (set (map :target lacking))) (pr-str lacking))
          (is (every? #(not (str/blank? (str (:err %)))) lacking) "each carries the world's reason")))
      (testing "the creator's serializer renders them as a block to act on"
        (let [out (ser/render-object db root-id reply 0 #{} {:lines [] :reads [] :seen #{}})
              lines (:lines out)]
          (is (some #(str/starts-with? % "They lack") lines) (pr-str lines))
          (is (some #(str/includes? % "read needy/README.md") lines))
          (is (not-any? #(str/includes? % "message/lacking") lines) "not as raw JSON"))))))

(deftest history-thread-lists-a-threads-messages
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          w (fx/attach-as world (:token witness))
          _ (fx/grant! root (:id witness) #{(:id witness)} #{:read})
          _ (fx/grant! root (:id witness) #{route} #{:transmit})
          sent (ws/request!! w :send {:to #{root-id} :route route :text "hello root, one question"})
          question (get-in sent [:result :message])
          reply (poll world (fn [db] (some (fn [m] (when (= question (get-in (objects/fields db m) [:message/in-reply-to :field/value])) m)) (messages-from db root-id))) 40000)]
      (is (:ok sent) (pr-str sent))
      (is (uuid? reply) "root answered")
      (testing "as the asker: both Messages, oldest first, with the asker"
        (let [r (ws/request!! w :history {:thread question})
              res (:result r)]
          (is (:ok r) (pr-str r))
          (is (= question (:thread res)))
          (is (= (:id witness) (:asked-by res)))
          (is (= 2 (:count res)) (pr-str res))
          (is (= [question reply] (mapv :object/id (:messages res))))))
      (testing "as root the same; an id that is no Message is unresolved"
        (is (= 2 (get-in (ws/request!! root :history {:thread question}) [:result :count])))
        (is (= :unresolved (get-in (ws/request!! root :history {:thread root-id}) [:err :code])))))))

(defn- deliveries [c sub pred timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop [acc []]
      (if (or (pred acc) (> (System/currentTimeMillis) deadline))
        acc
        (let [push (ws/await-push c sub 2000)
              ids (keep #(when (= :delivered (:event/kind %)) (get-in % [:event/body :message])) (:events push))]
          (recur (into acc ids)))))))

(deftest an-op-already-done-in-the-thread-is-answered-not-gated-again
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          w (fx/attach-as world (:token witness))
          _ (fx/grant! root (:id witness) #{(:id witness)} #{:read})
          _ (fx/grant! root (:id witness) #{route} #{:transmit})
          _ (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
          _ (fx/policy! world root {:object root-id :field :policy/approval :type [:list :string] :value ["task.run"] :reader :both})
          line (json-line "act" "{\"action\": \"task.run\", \"args\": {\"adapter\": \"shell.local\", \"cmd\": \"echo once-only; exit 0\"}}")
          sent (ws/request!! w :send {:to #{root-id} :route route :text (str "please run:\n" (ops-block line))})
          question (get-in sent [:result :message])
          got (deliveries w "inbox" #(>= (count %) 2) 30000)
          card (first (filter #(= :approval (some-> (field root % :message/kind) name keyword)) got))
          task (field root card :message/task)]
      (is (uuid? task) (pr-str got))
      (let [r (act! w "task.approve" {:task (str task)})
            _ (is (:ok r) (pr-str r))
            more (deliveries w "inbox" #(some (fn [m] (= question (field root m :message/in-reply-to))) %) 30000)
            ;; the decision Message goes to the requester (root), not to the asker: find it on the log
            result-msg (poll world (fn [db] (some (fn [m] (let [f (objects/fields db m)]
                                                            (when (and (= :approval-result (some-> (get-in f [:message/kind :field/value]) name keyword))
                                                                       (= task (get-in f [:message/task :field/value])))
                                                              m)))
                                                  (messages-from db (:id witness)))) 10000)
            tasks-before (count (tasks-of (act/db world) root-id))]
        (is (uuid? result-msg) "the decision Message about this Task exists")
        (testing "the result wake opens with the kernel's record of the run"
          (let [lines (ser/result-lines (act/db world) root-id result-msg)]
            (is (some #(str/starts-with? % "## result of what you asked for") lines) (pr-str lines))
            (is (some #(str/includes? % "exit 0") lines))
            (is (some #(str/includes? % "once-only") lines))
            (is (some #(str/includes? % "Do not run it again") lines))))
        (testing "the wake that delivers the result holds the run in its record, so the op is answered, not gated"
          (let [record (lops/thread-record (act/db world) root-id result-msg)
                rep (first (filter #(= "act" (str (:op %))) record))]
            (is (some? rep) (pr-str record))
            (is (= 0 (get-in rep [:result :exit])) "the earlier run's exit rides on the answer")
            (is (str/includes? (str (get-in rep [:result :stdout-tail])) "once-only"))))
        (testing "a person asking again has asked again: a new exchange, a new card"
          (let [again (ws/request!! w :send {:to #{root-id} :route route :in-reply-to question :text (str "again please:\n" (ops-block line))})
                q2 (get-in again [:result :message])
                after (deliveries w "inbox" #(some (fn [m] (= :approval (some-> (field root m :message/kind) name keyword))) %) 30000)]
            (is (:ok again) (pr-str again))
            (is (empty? (lops/thread-record (act/db world) root-id q2)))
            (is (some #(= :approval (some-> (field root % :message/kind) name keyword)) after) "gated again")))))))

