(ns knowledge.world.window-test
  "The Model's window is the limit, and a cited path is a fact about the disk."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.chatgpt :as chatgpt]
            [knowledge.world.code :as code]
            [knowledge.world.conversation :as conversation]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.loop.core]
            [knowledge.world.loop.worklog :as worklog]
            [knowledge.world.models :as models]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(deftest the-window-is-the-limit
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          p (:principal root)]
      (with-redefs [models/window-of (fn [_ _ _] 200000)]
        (let [pol (ser/policy db p)]
          (testing "the prompt may use what the Model has"
            (is (= 200000 (:serialize/budget-tokens pol)))
            (is (> (:serialize/messages-verbatim pol) 1000) "history is not held to four Messages")
            (is (> (:serialize/open-chars pol) 100000) "nor open files to 5000 chars"))
          (testing "compression is of the whole window"
            (is (< 100000 (conversation/compress-at db p) 140001)))))
      (testing "a Model that has not said keeps the safe defaults, and they are the only place 8000 lives"
        (with-redefs [models/window-of (fn [_ _ _] nil)]
          (is (= 8000 (:serialize/budget-tokens (ser/policy db p)))))))))

(deftest a-window-comes-from-the-provider-or-a-person
  (testing "the plan's catalogue says"
    (is (= [{:slug "gpt-x" :name "X" :context-window 272000}]
           (chatgpt/catalogue {:models [{:slug "gpt-x" :display_name "X" :visibility "list" :context_window 272000}]}))))
  (is (= [{:slug "y" :name "y"}] (chatgpt/catalogue {:models [{:slug "y" :visibility "list"}]}))
      "and one that does not say has none"))

(deftest a-server-may-report-its-window-in-its-metrics
  (let [text (str "# HELP tensorfold_health:context_length Tokens a request's prompt and reply may hold.\n"
                  "# TYPE tensorfold_health:context_length gauge\n"
                  "tensorfold_health:context_length 1048576\n"
                  "tensorfold_health:streams_max 8\n")]
    (is (= 1048576 (models/reported-window text)))
    (is (= 200000 (models/reported-window "vllm:context_length{model=\"x\"} 200000.0\n")))
    (is (nil? (models/reported-window "# HELP context_length 5\nother 7\n")))))

(deftest a-wake-has-room-and-a-principal-can-set-it
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          room #'knowledge.world.loop.core/room-of]
      (testing "no window, the old limits"
        (with-redefs [models/window-of (fn [_ _ _] nil)]
          (is (= 7 (:force-after (room world p nil))))))
      (with-redefs [models/window-of (fn [_ _ _] 272000)]
        (testing "a window gives room"
          (is (= 30 (:force-after (room world p nil))))
          (is (= 20 (:act-after (room world p nil)))))
        (testing "a Field on the Principal wins"
          (ws/request!! root :set-field {:object p :field :loop/force-after :type :number :value 12 :reader :both})
          (is (= 12 (:force-after (room world p nil)))))))))

(deftest reading-closes-when-the-log-says
  (is (not (worklog/hold-reading? {:quiet 10 :force-after 30})))
  (is (worklog/hold-reading? {:quiet 30 :force-after 30}))
  (is (worklog/hold-reading? {:quiet 7}) "and at the old limit with none set"))

(deftest the-first-turn-of-a-thread-may-carry-its-room
  (let [log (worklog/add {:open-chars 400000} [{:op :read :args {:name "a"} :ok false}])]
    (is (= 1 (:n log)) "a log that only knows its room is an empty log")
    (is (= 400000 (:open-chars log)))))

(deftest open-files-have-the-room-the-model-has
  (let [text (apply str (repeat 3000 "line of text\n"))
        read-of {:op :read :args {:name "a.txt"} :ok true
                 :result {:object/fields {:file/text {:field/value text} :file/lines {:field/value 3000}}}}]
    (is (not (:open (get-in (worklog/settle (worklog/add nil [read-of])) [:files "a.txt"]))))
    (is (:open (get-in (worklog/settle (worklog/add {:open-chars 400000 :files {} :order [] :n 0} [read-of])) [:files "a.txt"])))))

(deftest a-citation-is-a-file-that-is-there
  (fx/with-world [{:keys [world root]}]
    (let [dir (doto (java.io.File/createTempFile "orbis-cite" "") .delete .mkdirs)
          _ (spit (io/file dir "README.md") "# here\n")
          _ (.mkdirs (io/file dir "docs"))
          _ (spit (io/file dir "docs" "a.md") "a\n")
          r (ws/request!! root :act {:action :workspace.mount :args {:principal "root" :path (.getCanonicalPath dir) :name "cite" :shell false}})
          db (act/db world)
          p (:principal root)
          spans (fn [text] (code/cited-paths db p text))
          shown (fn [text] (mapv (fn [{:keys [start end]}] (subs text start end)) (spans text)))]
      (is (:ok r) (pr-str r))
      (testing "a path relative to where the Principal works, with its line"
        (let [text "see `README.md:7-15` and docs/a.md:3."
              [a b] (spans text)]
          (is (= ["README.md:7-15" "docs/a.md:3"] (shown text)))
          (is (= ["README.md" 7] [(:path a) (:line a)]))
          (is (= ["docs/a.md" 3] [(:path b) (:line b)]))
          (is (= :path (:kind a)))
          (is (uuid? (:object a)))))
      (testing "a sentence's full stop stays out, and no line is no line"
        (is (= ["README.md"] (shown "It is in README.md."))))
      (testing "what is not a file there is not a link, whatever it looks like"
        (doseq [text ["example.com:443" "modelhost.local:8000" "192.0.2.10:7777" "nothere.md:12" "docs/nothere.md"
                      "https://example.com:443/README.md" "http://x.org/docs/a.md:3" "ratio 3:4 and 1.2.3:4" "e.g. and/or"]]
          (is (empty? (spans text)) text)))
      (testing "fenced code is left alone"
        (is (empty? (spans "```\nREADME.md:1\n```"))))
      (testing "an offset counts code points"
        (is (= 2 (:start (first (spans "é README.md:1")))))))))

(deftest a-wake-on-a-real-window-is-the-whole-conversation
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root)
          route (get-in (ws/request!! root :resolve {:name "root.route" :op :read}) [:result :object])
          w (fx/attach-as world (:token witness))
          _ (fx/grant! root (:id witness) #{route} #{:transmit})
          ops (str "```ops\n" (str/join "\n" (for [i (range 12)] (str "{\"op\": \"read\", \"args\": {\"object\": \"" p "\"}}"))) "\n```")]
      (is (some? ops) "the shape of the test: twelve reads")
      (with-redefs [models/window-of (fn [_ _ _] 1000000)]
        (let [db (act/db world)
              pol (ser/policy db p)]
          (is (> (:serialize/evaluations-verbatim pol) 1000) "every turn of a wake stays whole")
          (is (> (:serialize/run-result-cap pol) 100000) "and a run's output is not cut to 2 KiB"))))))

(deftest a-conversation-that-only-reads-is-told-to-write
  (let [nudge #'knowledge.world.loop.core/still-reading]
    (is (nil? (nudge {} {:i 10})) "early on it is left alone")
    (is (re-find #"25 turns" (nudge {} {:i 25})))
    (is (nil? (nudge {} {:i 26})) "and it is not said every turn")
    (is (nil? (nudge {} {:i 30 :changed-at 28})) "a change resets it")
    (is (nil? (nudge {:work? true} {:i 25})) "an 8k wake has its own note")))

