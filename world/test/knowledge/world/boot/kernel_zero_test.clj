(ns knowledge.world.boot.kernel-zero-test
  "Kernel-zero (spec 17.3) as the standing integration test (spec 18.5 item 8). Six steps plus step 7
   (decisions 2026-10-30, root knows the harness), 8 (memory), 9 (approval policy); step 4 is the
   heartbeat Trigger. Observed
   only through the protocol as the named principal test/witness (and root's own client where the
   witness holds no Grant). Each failure names its step and the op or step that is not built."
  (:require [clojure.set :as set]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing use-fixtures]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]))

(def ^:private world-state (atom nil))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))

(defn- setup!
  "One world for the suite: witness may read root, root's stream and the schema objects, transmit
   on root.route and evaluate on root.model (every Grant through the grant op, as root)."
  []
  (or @world-state
      (let [dir (fx/temp-dir)
            world (fx/start-world dir 0)
            root (fx/attach-root world)
            witness (fx/create-witness! root)
            route (resolve-as root "root.route")
            model (resolve-as root "root.model")]
        (fx/grant! root (:id witness) #{(:principal root) (:id witness)} #{:read})
        (fx/grant! root (:id witness) {:prefix "schema"} #{:read})
        (when route (fx/grant! root (:id witness) #{route} #{:read :transmit}))
        (when model (fx/grant! root (:id witness) #{model} #{:read :evaluate}))
        (reset! world-state {:dir dir :world world :root root :witness witness :root-id (:principal root)
                             :route route :model model}))))

(defn- teardown! []
  (when-let [{:keys [world root dir]} @world-state]
    (ws/close! root)
    (system/stop world)
    (fx/delete-tree! dir)
    (reset! world-state nil)))

(use-fixtures :once (fn [run] (setup!) (try (run) (finally (teardown!)))))

(defn- as-witness [f]
  (let [{:keys [world witness]} (setup!)
        c (fx/attach-as world (:token witness))]
    (try (f c) (finally (ws/close! c)))))

(defn- step-msg [n title detail] (str "kernel-zero step " n " (" title "): " detail))

(defn- root-state [c root-id] (:result (ws/request!! c :read {:object root-id})))
(defn- root-history [c root-id] (get-in (ws/request!! c :history {:stream root-id :limit 1000}) [:result :events]))

(defn- bound-in
  "name -> object for the names bound in root's namespace, from the bound events on root's stream."
  [c root-id]
  (into {} (for [e (root-history c root-id)
                 :when (and (= :bound (:event/kind e)) (= root-id (get-in e [:event/body :namespace])))]
             [(get-in e [:event/body :name]) (get-in e [:event/body :object])])))

(deftest step-1-names-itself
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            names (set (keys (bound-in c root-id)))
            expected #{"root" "root.workspace" "root.model" "root.route"}]
        (is (= expected (set/intersection expected names))
            (step-msg 1 "name itself"
                      (str "root has bound " (pr-str names) " in its own namespace, expected " (pr-str expected)
                           "; step! :name-itself is not implemented")))))))

(deftest shipped-harness-explains-context-layers
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            harness (get-in (root-state c root-id)
                            [:object/fields :instructions/harness :field/value])]
        (doseq [clause ["Context: three layers" "**Wake:**" "**Compression:**" "**Fields:**"
                        "not compression or deletion" "Prompt-budget clipping is separate"
                        "open questions and Task ids" "cite a Task id, not copied output"]]
          (is (str/includes? harness clause) (str "missing context guidance: " clause)))))))

(deftest step-2-reads-shipped-instructions-and-writes-own
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            fields (:object/fields (root-state c root-id))
            shipped (filter #(= "instructions" (namespace (key %))) fields)
            own (filter (fn [[_ f]] (and (= root-id (:field/author f)) (= :agent (:field/reader f)))) fields)
            reads (filter #(and (= :field-read (:event/kind %)) (= root-id (:event/actor %))) (root-history c root-id))]
        (is (seq shipped) (step-msg 2 "read shipped instructions" "no instruction Fields are shipped on root"))
        (is (seq reads) (step-msg 2 "read shipped instructions" "root has not read any instruction Field (no field-read by root on its stream); step! :read-and-write-instructions is not implemented"))
        (is (seq own) (step-msg 2 "write own instructions" "root has written no reader :agent Field about what it is for; step! :read-and-write-instructions is not implemented"))))))

(deftest shipped-harness-builds-client-capabilities-without-remote-login
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            harness (get-in (root-state c root-id)
                            [:object/fields :instructions/harness :field/value])]
        (doseq [clause ["Capabilities needed from a client belong in the client"
                        "remote-login or pull-script workarounds"
                        "verify release provenance, digest"
                        "signing identity before replacement"
                        "preserve client continuity"
                        "restore the previous app and report rollback"
                        "person's explicit authorization"]]
          (is (str/includes? harness clause) (str "missing client tenet: " clause)))))))

(deftest shipped-harness-keeps-work-visible-and-mainlines-lessons
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            harness (get-in (root-state c root-id)
                            [:object/fields :instructions/harness :field/value])]
        (doseq [clause ["Person-facing work updates"
                        "what, why and the rough duration"
                        "one-line informational"
                        "roughly ten minutes"
                        "urgent `notify`"
                        "`task.progress`"
                        "`Changed:`, `Verified:`, `Next:`"
                        "End only when waiting"
                        "completion Message wake you"
                        "Mainline instruction improvements"
                        "`instructions/own`, `memory/notes`"
                        "`memory/lessons`"
                        "regression coverage"
                        "separate commit"
                        "private Fields"]]
          (is (str/includes? (or harness "") clause)
              (str "shipped harness instruction missing: " clause)))))))

(deftest step-3-binds-model-and-evaluates-via-stub
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            model (get (bound-in c root-id) "root.model")
            evaluated (filter #(= :evaluated (:event/kind %)) (root-history c root-id))
            r (when model (ws/request!! c :evaluate {:working-set #{root-id} :model model}))]
        (is (some? model) (step-msg 3 "bind model" "root.model is not bound; step! :bind-model-and-evaluate is not implemented"))
        (is (seq evaluated) (step-msg 3 "evaluate own instructions" "no evaluated event on root's stream"))
        (is (:ok r) (step-msg 3 "evaluate" (str "op evaluate answered " (name (get-in r [:err :code] :none)))))))))

(deftest step-4-arms-heartbeat-and-it-fires
  ;;: root.heartbeat is an interval Trigger root owns, bound at boot, disarmed while
  ;; root's policy/heartbeat-s is 0. The test turns it to 1 s and arms it through the arm op (as root:
  ;; the witness holds no :write on it), sees it fire within 2 s, sees root's loop answer the firing
  ;; on the stub Model, then disarms it so the rest of the suite is quiet.
  (let [{:keys [root root-id]} @world-state
        hb (resolve-as root "root.heartbeat")
        f (fn [k] (get-in (root-state root root-id) [:object/fields k :field/value]))
        tf (fn [k] (get-in (ws/request!! root :read {:object hb}) [:result :object/fields k :field/value]))]
    (is (some? hb) (step-msg 4 "arm heartbeat" "root.heartbeat is not bound; step! :arm-heartbeat is not implemented"))
    (is (= 0 (f :policy/heartbeat-s)) (step-msg 4 "policy" (str "policy/heartbeat-s is " (pr-str (f :policy/heartbeat-s)) ", expected the shipped 0 (off)")))
    (is (= :interval (tf :trigger/kind)) (step-msg 4 "trigger" (pr-str (tf :trigger/kind))))
    (is (false? (tf :trigger/armed)) (step-msg 4 "trigger" "root.heartbeat should be disarmed while policy/heartbeat-s is 0"))
    (let [_ (ws/request!! root :set-field {:object hb :field :trigger/every-s :type :number :value 1})
          sub (ws/request!! root :subscribe {:sub "hb" :stream root-id :kinds #{:fired}})
          arm (ws/request!! root :arm {:trigger hb})
          fired (when (:ok sub) (some->> (ws/await-push root "hb" 2000) :events (filter #(= :fired (:event/kind %))) first))]
      (is (:ok arm) (step-msg 4 "arm heartbeat" (str "op arm answered " (name (get-in arm [:err :code] :none)))))
      (is (some? fired) (step-msg 4 "heartbeat fires within 2 s" "no fired event on root's stream within 2 s; the scheduler is not running"))
      (is (= hb (get-in fired [:event/body :trigger])))
      (testing "root's loop answers the heartbeat (the stub speaks in prose, so the reply is sent)"
        (let [m (get-in fired [:event/body :message])
              answered (fn [] (some (fn [e] (= m (get-in (ws/request!! root :read {:object (get-in e [:event/body :message])}) [:result :object/fields :message/in-reply-to :field/value])))
                                    (filter #(= :sent (:event/kind %)) (root-history root root-id))))]
          (is (loop [n 0] (cond (answered) true (> n 200) false :else (do (Thread/sleep 50) (recur (inc n)))))
              (step-msg 4 "loop answers the heartbeat" "root sent nothing in reply to the heartbeat Message within 10 s"))))
      (is (:ok (ws/request!! root :disarm {:trigger hb})) (step-msg 4 "disarm" "op disarm failed")))))

(deftest step-5-answers-listener-citing-fields-read
  (as-witness
    (fn [c]
      (let [{:keys [root-id root]} @world-state
            witness-id (:id (:witness @world-state))
            route (get (bound-in c root-id) "root.route")
            _ (ws/request!! c :subscribe {:sub "inbox" :stream witness-id :kinds #{:delivered}})
            r (ws/request!! c :send {:to #{root-id} :route (or route root-id) :text "what do you know, and why?"})
            push (when (:ok r) (ws/await-push c "inbox" 30000))
            reply (some-> push :events first :event/body :message)
            ;; the witness holds no Grant on the reply Message (see CONTRACT-NEEDS.md); root reads it
            text (when reply (get-in (ws/request!! root :read {:object reply}) [:result :object/fields :message/text :field/value]))
            read-names (into #{} (for [e (root-history c root-id)
                                       :when (and (= :field-read (:event/kind e)) (= root-id (:event/actor e)))
                                       n (get-in e [:event/body :names])]
                                   (subs (str n) 1)))
            answered (filter #(= :sent (:event/kind %)) (root-history c root-id))]
        (is (:ok r) (step-msg 5 "answer a listener message" (str "op send answered " (name (get-in r [:err :code] :none)))))
        (is (seq answered) (step-msg 5 "answer" (str "root sent no Message; the resident loop did not run")))
        (is (string? text) (step-msg 5 "reply delivered" (str "no reply Message was delivered to " witness-id " within 30 s")))
        (is (some #(str/includes? (str text) %) read-names)
            (step-msg 5 "answer citing fields read" (str "root's reply cites none of the Fields it read " (pr-str read-names) ": " (pr-str text))))))))

(deftest step-6-first-person-with-narrower-grant
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            grants (filter #(and (= :granted (:event/kind %)) (= root-id (:event/actor %))
                                 (not= :everything (get-in % [:event/body :scope])))
                           (root-history c root-id))
            person-grant (first grants)
            r-read (ws/request!! c :resolve {:name "root" :op :read})
            r-delegate (ws/request!! c :resolve {:name "root" :op :delegate})]
        (is (some? person-grant) (step-msg 6 "first person" "root has issued no Grant narrower than its own to a person Principal; step! :first-person is not implemented"))
        (is (:ok r-read) (step-msg 6 "resolves? holds for :read" (str "op resolve answered " (name (get-in r-read [:err :code] :none)))))
        (is (and (:ok r-delegate) (nil? (:result r-delegate)))
            (step-msg 6 "resolves? is nil for :delegate (the caller's ':grant')" (str "op resolve answered " (name (get-in r-delegate [:err :code] :none)))))))))

(deftest step-7-knows-the-harness
  ;; decisions 2026-10-30: root is the bootstrap developer; the repository is mounted as `harness`,
  ;; indexed onto root, read through the mount, and root's first command ran as a Task. Observed
  ;; through root's own client (the witness holds no Grant on the mount or the Task).
  (let [{:keys [root root-id]} @world-state
        state (root-state root root-id)
        f (fn [k] (get-in state [:object/fields k :field/value]))
        ethos (get-in (ws/request!! root :resolve {:name "harness/docs/ethos.md" :op :read}) [:result :object])
        ethos-state (when ethos (:result (ws/request!! root :read {:object ethos})))
        ev (fn [k] (get-in ethos-state [:object/fields k :field/value]))
        done (filter #(and (= :task-moved (:event/kind %)) (= :done (get-in % [:event/body :to]))) (root-history root root-id))
        task (some-> done first :event/body :task)
        task-state (when task (:result (ws/request!! root :read {:object task})))
        tv (fn [k] (get-in task-state [:object/fields k :field/value]))]
    (is (string? (f :harness/index)) (step-msg 7 "index" "root has no harness/index Field; step! :know-the-harness is not implemented"))
    (is (str/includes? (str (f :harness/index)) "world/src/knowledge/world/loop/core.clj") (step-msg 7 "index" "harness/index does not list the loop's source"))
    (is (re-find #"^[0-9a-f]{7,} (clean|dirty)$" (str (f :harness/head))) (step-msg 7 "head" (str "harness/head is " (pr-str (f :harness/head)))))
    (is (every? #(string? (f %)) [:instructions/harness :instructions/ethos :instructions/charter]) (step-msg 7 "instructions" "instructions/harness, instructions/ethos and instructions/charter are shipped on root"))
    (is (some? ethos) (step-msg 7 "mount" "harness/docs/ethos.md does not resolve for root"))
    (is (= 64 (count (str (ev :file/sha256)))) (step-msg 7 "mount" "the ethos Object carries no file/sha256"))
    (is (or (string? (ev :file/text)) (map? (ev :file/bytes))) (step-msg 7 "mount" "the ethos Object carries neither file/text nor a file/bytes receipt"))
    (is (some? task) (step-msg 7 "task" "no Task of root reached done; shell.local run did not happen"))
    (is (= "git log --oneline -1" (tv :task/command)) (step-msg 7 "task" (pr-str (tv :task/command))))
    (is (= 0 (tv :task/exit)) (step-msg 7 "task" (str "exit " (pr-str (tv :task/exit)) " stderr " (pr-str (tv :task/stderr)))))
    (is (not (str/blank? (str (tv :task/stdout)))) (step-msg 7 "task" "the run captured no stdout"))
    (testing "root read its index, its harness instructions and the ethos through the mount: field-read events by root"
      (let [reads (into #{} (for [e (root-history root root-id) :when (and (= :field-read (:event/kind e)) (= root-id (:event/actor e)))
                                  n (get-in e [:event/body :names])]
                              n))]
        (is (every? reads [:harness/index :instructions/harness :instructions/ethos]) (pr-str reads))))))

(deftest step-8-memory-defaults
  ;; every process Principal carries its memory defaults; root gets them at boot
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            f (fn [k] (get-in (root-state c root-id) [:object/fields k :field/value]))]
        (is (string? (f :memory/policy)) (step-msg 8 "memory" "root has no memory/policy Field; step! :memory-defaults is not implemented"))
        (is (= 4000 (f :memory/budget)) (step-msg 8 "memory" (str "memory/budget is " (pr-str (f :memory/budget)))))
        (is (= 30 (f :memory/retention-days)) (step-msg 8 "memory" "memory/retention-days missing"))
        (is (string? (f :memory/notes)) (step-msg 8 "memory" "memory/notes missing"))))))

(deftest step-9-approval-defaults
  ;; W4 (decisions 2026-10-30 'approval gates'): every process Principal carries its approval policy; root gets it at boot
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            f (fn [k] (get-in (root-state c root-id) [:object/fields k :field/value]))]
        (is (= ["task.run" "shell" "grant" "principal.create"] (f :policy/approval)) (step-msg 9 "approval" "root has no policy/approval Field with the shipped default; step! :approval-defaults is not implemented"))
        (is (= 86400 (f :policy/approval-ttl-s)) (step-msg 9 "approval" (str "policy/approval-ttl-s is " (pr-str (f :policy/approval-ttl-s)))))))))

(deftest shipped-developer-habits
  (as-witness
    (fn [c]
      (let [{:keys [root-id]} @world-state
            fields (:object/fields (root-state c root-id))]
        (doseq [key [:instructions/harness :instructions/charter-template/developer]
                clause ["Test-only problems: change approach each try"
                        "Product code: stop and ask after the second failure"
                        "Running tests never needs approval, including the release gate."
                        "Ask once before publishing, covering the push and the release together, with the gate results and privacy review in the question."
                        "Ask before a restart or anything on a person's device."
                        "Ask once for all foreseeable steps needing approval"
                        "While blocked, continue approved independent work."
                        "repeat it once after an hour without an answer."
                        "On a heartbeat, resume work that ended in an error"
                        "Red means stop: never commit or publish on a red or unrun gate. Before a commit, one full run on the exact tree. Twice in a row only for the parallel suite and the release gate."
                        "delegated developers never merge or push"
                        "Secrets appear only as environment-variable names"]]
          (is (str/includes? (str (get-in fields [key :field/value])) clause)
              (str key " missing developer habit: " clause)))))))
