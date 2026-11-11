(ns knowledge.world.loop.answer-test
  "The wake ends where the kernel knows it ends (decisions 2026-10-31). A gated op waiting for a person
   ends it with no further model call, and a send to the asker is the answer, not a Message of its own."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.model-api :as model]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.core]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" (map j/write-value-as-string lines)) "\n```"))
(defn- v [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- evaluations [world message]
  (filter #(= message (v world % :evaluation/message)) (objects/objects-with-field (act/db world) :evaluation/message)))
(defn- replies [world message]
  (filter #(= message (v world % :message/in-reply-to)) (objects/objects-with-field (act/db world) :message/in-reply-to)))
(defn- plain [world ids] (remove #(#{:approval :status} (some-> (v world % :message/kind) name keyword)) ids))
(defn- idle? [world p] (zero? (long (or (v world p :loop/iteration) 0))))

(defn- setup [root witness world]
  (let [route (resolve-as root "root.route")
        w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{(:id witness)} #{:read})
    (fx/grant! root (:id witness) #{route} #{:transmit})
    {:route route :w w :send-as (fn [text] (assoc {:op "send"} :args {:to [(str (:id witness))] :route (str route) :text text}))}))

(defn- ask! [w to route text]
  (let [r (ws/request!! w :send {:to #{to} :route route :text text})]
    (assert (:ok r) (pr-str r))
    (get-in r [:result :message])))

(deftest a-send-to-the-asker-alone-is-the-reply
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w send-as]} (setup root witness world)
          ask (ask! w root-id route (str "answer me:\n" (ops-block (send-as "the answer is 42"))))]
      (is (until #(seq (plain world (replies world ask))) 15000))
      (is (until #(idle? world root-id) 10000))
      (let [[reply :as all] (plain world (replies world ask))]
        (is (= 1 (count all)) "one Message")
        (is (str/starts-with? (v world reply :message/text) "the answer is 42"))
        (is (= 1 (count (evaluations world ask))) "the wake ended with it")))))

(deftest two-sends-of-one-answer-are-one-reply
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w send-as]} (setup root witness world)
          ask (ask! w root-id route (str "answer me:\n" (ops-block (send-as "once") (send-as "once"))))]
      (is (until #(seq (plain world (replies world ask))) 15000))
      (is (until #(idle? world root-id) 10000))
      (is (= 1 (count (plain world (replies world ask)))))
      (is (= 1 (count (evaluations world ask)))))))

(deftest a-send-beside-other-ops-waits-for-their-results
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w send-as]} (setup root witness world)
          ask (ask! w root-id route (str "read then answer:\n"
                                         (ops-block {:op "read" :args {:object (str root-id)}}
                                                    (send-as "guessed before reading"))))]
      (is (until #(seq (plain world (replies world ask))) 15000))
      (is (until #(idle? world root-id) 10000))
      (is (not-any? #(str/includes? (str (v world % :message/text)) "guessed before reading") (replies world ask)))
      (let [results (mapcat #(v world % :evaluation/results) (evaluations world ask))]
        (is (some #(= :answer-held (some-> (get-in % [:err :code]) name keyword)) results) (pr-str results)))
      (is (= 2 (count (evaluations world ask))) "the read's results, then the answer"))))

(deftest a-gated-op-ends-the-wake-until-a-person-decides
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          shell (resolve-as root "shell.local")
          ask (ask! w root-id route (str "run it:\n" (ops-block {:op "act" :args {:object (str shell) :action "run" :args {:cmd "echo gated-hello"}}})))
          status #(first (filter (fn [m] (= :status (some-> (v world m :message/kind) name keyword))) (replies world ask)))]
      (is (until status 15000))
      (is (until #(idle? world root-id) 10000))
      (is (= 1 (count (evaluations world ask))) "no model call after the gate")
      (is (str/starts-with? (v world (status) :message/text) "Waiting for approval: "))
      (is (str/includes? (v world (status) :message/text) "gated-hello") "the kernel names what waits"))))

(deftest a-send-to-someone-else-is-still-a-send
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          ask (ask! w root-id route (str "tell them:\n" (ops-block {:op "send" :args {:to [(str root-id)] :route (str route) :text "note to self"}})))]
      (is (until #(seq (plain world (replies world ask))) 15000))
      (is (until #(idle? world root-id) 10000))
      (is (>= (count (evaluations world ask)) 2) "the wake went on after it"))))

(deftest the-word-ops-then-a-json-fence-is-an-ops-block
  (let [line "{\"op\": \"read\", \"args\": {\"name\": \"x\"}}"]
    (testing "the shape seen live"
      (let [{:keys [ops reply]} (lops/parse {:text (str "ops\n```json\n" line "\n```")})]
        (is (= [:read] (map :op ops)))
        (is (str/blank? reply))))
    (testing "JSON shown in a reply stays text"
      (let [{:keys [ops reply]} (lops/parse {:text (str "It looks like this:\n```json\n" line "\n```")})]
        (is (empty? ops))
        (is (str/includes? reply line))))))

(deftest a-reply-of-nothing-but-op-lines-is-ops
  (let [{:keys [ops reply]} (lops/parse {:text "read {\"name\": \"x\"} ||| read {\"name\": \"y\"}"})]
    (is (= [:read :read] (map :op ops)))
    (is (str/blank? reply)))
  (testing "a sentence around it stays a reply"
    (let [{:keys [ops]} (lops/parse {:text "You could run read {\"name\": \"y\"} next."})]
      (is (empty? ops)))))

(deftest resolve-means-read-unless-it-says
  (is (= :read (some-> (get-in (first (:ops (lops/parse {:text (ops-block {:op "resolve" :args {:name "x"}})}))) [:args :op]) name keyword))))

(deftest a-wake-at-its-cap-answers-from-what-it-has
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          _ (assert (:ok (ws/request!! root :set-field {:object root-id :field :loop/max-iterations :type :number :value 1 :reader :both})))
          ask (ask! w root-id route (str "go:\n" (ops-block {:op "read" :args {:object (str root-id)}})))]
      (is (until #(seq (plain world (replies world ask))) 15000))
      (is (until #(idle? world root-id) 10000))
      (is (= 2 (count (evaluations world ask))) "one turn to act, one with no tools to answer")
      (is (not (str/blank? (v world (first (plain world (replies world ask))) :message/text)))))))

(deftest with-nothing-to-say-the-kernel-says-where-it-got-to
  (let [f @#'knowledge.world.loop.core/looked-at]
    (is (= " It last looked at: g/a.c; make; boot." 
           (f [{:op "read" :args {:name "g/a.c"}} {:op "read" :args {:object "x"}}
               {:op "act" :args {:args {:cmd "make"}}} {:op :act :args {:args {:pattern "boot"}}}])))
    (is (nil? (f [])))))

(deftest the-forms-a-model-really-writes
  (testing "the Adapter as the tag, by name"
    (fx/with-world [{:keys [world root]}]
      (let [db (act/db world) p (:principal root)
            {:keys [ops reply]} (lops/parse {:text "Let me look.<tool_call>~shell.local{\"cmd\":\"ls\"}<tool_call>~harness{\"name\":\"x\"}"}
                                            {:db db :p p :ids {}})]
        (is (= [:act :read] (map :op ops)) (pr-str ops))
        (is (= "run" (some-> (get-in (first ops) [:args :action]) name)))
        (is (uuid? (get-in (first ops) [:args :object])) "a name where an id goes")
        (is (= "Let me look." reply)))))
  (testing "op lines in another wrapper"
    (let [{:keys [ops reply]} (lops/parse {:text "<ops>\n{\"op\":\"read\",\"args\":{\"name\":\"x\"}}\n</ops>"})]
      (is (= [:read] (map :op ops)))
      (is (str/blank? reply))))
  (testing "bare op JSON as the whole reply"
    (is (= [:read] (map :op (:ops (lops/parse {:text "{\"op\":\"read\",\"args\":{\"name\":\"x\"}}"})))))))

(deftest placeholders-do-not-reach-the-asker
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          _ (assert (:ok (ws/request!! root :set-field {:object root-id :field :loop/max-iterations :type :number :value 2 :reader :both})))
          ask (ask! w root-id route "say exactly: *Preparing reply...*")]
      ;; the stub does not obey; the predicate is what is under test
      (is (until #(seq (replies world ask)) 15000))))
  (let [f @#'knowledge.world.loop.core/not-an-answer]
    (doseq [t ["*Preparing reply...*" "... (truncated)" "..." "<tool_call>~x{}" "{\"op\":\"read\"}" "Understood — I'll wait for the next wake to continue." "I'll look at the source first."]]
      (is (string? (f t)) t))
    (doseq [t ["The answer is 42." "Working set is three files, all read." "" "I'll wait for your answer on which file you mean, since both match the name you gave."]]
      (is (nil? (f t)) t))))

(deftest forms-of-a-reference-where-an-id-goes
  (fx/with-world [{:keys [world root]}]
    (let [wake {:db (act/db world) :p (:principal root) :ids {"52" (str (:principal root))}}
          [{:keys [args err]}] (:ops (lops/parse {:text (ops-block {:op "read" :args {:object "[[52|root]]"}})} wake))]
      (is (nil? err) (pr-str err))
      (is (= (:principal root) (:object args))))
    (testing "a resolve for a wire op means read"
      (is (= :read (some-> (get-in (first (:ops (lops/parse {:text (ops-block {:op "resolve" :args {:name "x" :op "act"}})}))) [:args :op]) name keyword))))))

(deftest a-directory-reads-with-its-slash
  (is (= "atlas" (get-in (first (:ops (lops/parse {:text (ops-block {:op "read" :args {:name "atlas/"}})}))) [:args :name]))))

(deftest a-second-identical-read-is-not-read-again
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          op {:op :read :args {:object p}}
          [first-read] (lops/execute! world p nil 1 [op] {})
          [second-read] (lops/execute! world p nil 1 [op] {:done [first-read]})
          [after-write] (lops/execute! world p nil 1 [op] {:done [first-read {:op "act" :ok true}]})]
      (is (:ok first-read))
      (is (some? (get-in first-read [:result :object/id])))
      (is (:repeated second-read))
      (is (nil? (get-in second-read [:result :object/id])))
      (is (some? (get-in after-write [:result :object/id])) "a write in between: read it again"))))

(deftest a-claim-with-nothing-behind-it-is-asked-once
  (let [f @#'knowledge.world.loop.core/unbacked-claim]
    (fx/with-world [{:keys [world]}]
      (let [ctx {:m-state nil}
            reads {:done [{:op "read" :ok true}]}]
        (is (string? (f world ctx reads "The edit is done and unstaged.")))
        (is (string? (f world ctx reads "Runs this wake:\n- `x` -> exit 0")))
        (is (nil? (f world ctx (assoc reads :challenged? true) "The edit is done.")) "once")
        (is (nil? (f world ctx {:done [{:op "act" :ok true}]} "The edit is done.")) "backed by an op")
        (is (nil? (f world ctx reads "The file says the edit was postponed.")) "no claim")
        (is (string? (f world ctx reads "The build is not configured. Let me configure first, then build.")))
        (is (string? (f world ctx reads "I have what I need.\nAnswer them on the Route.")))
        (is (nil? (f world ctx reads "The build is not configured. Tell me which preset you want.")) "a question to the asker")))))

(deftest a-card-stops-its-block
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          shell (resolve-as root "shell.local")
          run (fn [cmd] {:op :act :args {:object shell :action :run :args {:cmd cmd}}})
          results (lops/execute! world p nil 1 [(run "echo one") (run "echo two") {:op :read :args {:object p}}] {})
          code (fn [r] (some-> (get-in r [:err :code]) name keyword))]
      (is (= [:approval-pending :held :held] (map code results)) (pr-str results))
      (is (= 1 (count (filter #(= "approval" (v world % :task/kind)) (objects/objects-with-field (act/db world) :task/kind))))
          "one card"))))

(deftest a-tool-the-wake-cannot-use-is-not-offered
  (let [names (fn [withheld]
                (set (map #(get-in % [:function :name])
                          (:tools (model/request-body (assoc (model/create {:mode :real :url "http://x" :model "m"})
                                                             :tools? true :withheld withheld) [] :off)))))]
    (is (contains? (names []) "text_back"))
    (is (not (contains? (names ["text_back" "ask"]) "text_back")))
    (is (contains? (names ["text_back"]) "read"))))

(deftest a-turn-of-a-loop-offers-the-ops-as-tools
  (let [body #(model/request-body (assoc (model/create {:mode :real :url "http://x" :model "m"}) :tools? %) [] :off)]
    (is (= ["read" "grep" "outline" "projects" "add_project" "delegate" "progress" "ask" "text_back" "edit" "test" "run" "act" "send" "set-field" "resolve" "history" "bind" "grant"]
           (map #(get-in % [:function :name]) (:tools (body true)))))
    (is (nil? (:tools (body false))) "a compression only writes")))

(deftest a-tool-speaking-model-sees-its-calls-as-turns
  (let [msgs (ser/chat-messages {:acting "a" :body "b" :message-text "q"
                                 :turns [{:calls [{:name "read" :arguments "{}"} {:name "act" :arguments "{}"}] :text "r1"}
                                         {:calls [] :raw "bad" :text "not sent"}]})]
    (is (= ["system" "user" "assistant" "tool" "tool" "assistant" "user"] (map :role msgs)))
    (is (= ["t0-0" "t0-1"] (map :id (:tool_calls (nth msgs 2)))))
    (is (= "t0-0" (:tool_call_id (nth msgs 3))))
    (is (= "r1" (:content (nth msgs 3))))
    (is (= "bad" (:content (nth msgs 5))))))

(deftest a-plan-sent-to-the-asker-is-still-a-plan
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w send-as]} (setup root witness world)
          ask (ask! w root-id route (str "go:\n" (ops-block (send-as "There are two projects. Next I'll create them."))))]
      (is (until #(seq (plain world (replies world ask))) 15000))
      (is (until #(idle? world root-id) 10000))
      (is (not-any? #(str/includes? (str (v world % :message/text)) "Next I'll") (replies world ask)))
      (is (= 2 (count (evaluations world ask))) "it was asked again"))))

(deftest a-reply-that-offers-the-rest-is-asked-to-do-it
  (let [re @#'knowledge.world.loop.core/offers-next-re]
    (doseq [t ["Configure passed. No compile or test run has happened yet."
               "Configured. Next step: build. Would you like me to run it?"
               "If you want, I can run the tests."
               "I can now build it."]]
      (is (re-find re t) t))
    (doseq [t ["3 of 3 tests pass." "The boot sector is parsed in src/boot.cpp, line 12."]]
      (is (nil? (re-find re t)) t))))
