(ns knowledge.world.conversation-test
  "Directives never reach a model; compression keeps the recent lines and shrinks the Serialization,
   its summary carries via, the threshold works, /new and /resume steer which conversation is open."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.conversation :as conv]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.thread :as thread]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- until [pred ms]
  (let [deadline (+ (System/currentTimeMillis) ms)]
    (loop [] (or (pred) (when (< (System/currentTimeMillis) deadline) (Thread/sleep 50) (recur))))))

(defn- setup [root witness world]
  (let [route (resolve-as root "root.route") w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{(:id witness) route} #{:read :transmit})
    {:route route :w w}))
(defn- say! [w to route text] (get-in (ws/request!! w :send {:to #{to} :route route :text text}) [:result :message]))
(defn- answered? [world p m] (thread/answered-by? (act/db world) m p))
(defn- field [world o k] (get-in (objects/fields (act/db world) o) [k :field/value]))
(defn- status-to [world m]
  (let [db (act/db world)]
    (some (fn [x] (when (= m (get-in (objects/fields db x) [:message/in-reply-to :field/value]))
                    (get-in (objects/fields db x) [:message/text :field/value])))
          (objects/objects-of-class db :message))))
(defn- evaluations [world] (count (objects/objects-of-class (act/db world) :evaluation)))

(deftest directives-never-reach-a-model
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root) {:keys [route w]} (setup root witness world)
          before (evaluations world)]
      (testing "an unknown word answers the list"
        (let [m (say! w p route "/compresss")]
          (is (until #(status-to world m) 4000))
          (is (str/includes? (status-to world m) "/compresss is not a directive"))
          (is (str/includes? (status-to world m) "/resume"))))
      (testing "/new is answered too"
        (let [m (say! w p route "/new")] (is (until #(status-to world m) 4000))))
      (is (= before (evaluations world)) "no Evaluation ran")
      (testing "a path is not a directive, and /think rides on to the model"
        (let [m (say! w p route "/Users/someone/SOUL.md")]
          (is (nil? (field world m :message/directive)))
          (is (until #(answered? world p m) 8000)))
        (let [m (say! w p route "/think what is this")]
          (is (nil? (field world m :message/directive))))))))

(deftest new-and-resume
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root) {:keys [route w]} (setup root witness world)
          a (say! w p route "first talk")
          _ (is (until #(answered? world p a) 8000))
          _ (say! w p route "/new")
          b (say! w p route "second talk")
          _ (is (until #(answered? world p b) 8000))]
      (is (= b (thread/thread-of (act/db world) b)) "/new closed the first")
      (say! w p route (str "/resume " (subs (str a) 0 8)))
      (let [c (say! w p route "back again")]
        (is (= a (thread/thread-of (act/db world) c)) "/resume reopened it")))))

(deftest compress-keeps-the-recent-and-shrinks-the-prompt
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root) {:keys [route w]} (setup root witness world)
          lines (vec (for [i (range 8)]
                       (let [m (say! w p route (str "line " i " " (apply str (repeat 60 "words "))))]
                         (is (until #(answered? world p m) 8000)) m)))
          t (thread/thread-of (act/db world) (first lines))
          last-line (peek lines)
          before (:body (ser/render (act/db world) {:principal p :working-set (set lines) :route route :message last-line}))
          r (conv/compress! world p t {:note "keep the names"})
          db (act/db world)
          after (:body (ser/render db {:principal p :working-set (set lines) :route route :message last-line}))]
      (is (:ok r) (pr-str r))
      (testing "the summary carries via and covers a seq range; the Messages stay"
        (is (string? (field world t :conversation/summary)))
        (is (some #(and (= :field-set (:event/kind %)) (= :conversation/summary (get-in % [:event/body :name]))
                        (= (:evaluation (:ok r)) (:event/via %)))
                  (log/read-stream (:log world) t 0 1000)) "via the Evaluation")
        (is (>= (count (thread/messages db t)) 16) "the Messages stay"))
      (testing "the Serialization shrinks and marks the summary"
        (is (< (count after) (count before)))
        (is (str/includes? after "## summary of this conversation")))
      (testing "the newest lines stay verbatim"
        (is (str/includes? after "line 7 ")))
      (testing "a second compression covers the first"
        (let [more (vec (for [i (range 8 14)]
                          (let [m (say! w p route (str "line " i " " (apply str (repeat 60 "words "))))]
                            (is (until #(answered? world p m) 8000)) m)))
              through (field world t :conversation/through)
              r2 (conv/compress! world p t nil)]
          (is (:ok r2) (pr-str r2))
          (is (> (field world t :conversation/through) through))
          (is (= (:object/created-seq (objects/entity (act/db world) (first lines))) (long (field world t :conversation/from))) "still from the first line"))))))

(deftest a-summary-has-a-size
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root) {:keys [route w]} (setup root witness world)
          lines (vec (for [i (range 8)]
                       (let [m (say! w p route (str "line " i))]
                         (is (until #(answered? world p m) 8000)) m)))
          t (thread/thread-of (act/db world) (first lines))
          seen (atom nil)]
      (is (= conv/default-summary-tokens (conv/summary-tokens (act/db world) p)))
      (ws/request!! root :set-field {:object p :field :serialize/summary-tokens :type :number :value 150 :reader :both})
      (ws/request!! root :set-field {:object p :field :serialize/compressor :type :string :value "" :reader :both})
      (with-redefs [evaluate/evaluate! (fn [_ {:keys [rendered]}] (reset! seen rendered) {:err {:code :invalid :message "stop"}})]
        (conv/compress! world p t nil))
      (is (str/includes? (:acting @seen) "under 150 tokens"))
      (is (str/includes? (:template @seen) "conversation's memory") "an empty Field is the shipped prompt"))))

(deftest a-principal-gets-its-own-prompt-from-the-shipped-one
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)]
      (is (:ok (ws/request!! root :act {:action :conversation.compressor :args {:principal (str p)}})))
      (is (str/includes? (field world p :serialize/compressor) "conversation's memory")))))

(deftest a-long-conversation-compresses-itself
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root) {:keys [route w]} (setup root witness world)]
      (ws/request!! root :set-field {:object p :field :serialize/compress-at :type :number :value 200})
      (let [lines (vec (for [i (range 8)]
                         (let [m (say! w p route (str "line " i " " (apply str (repeat 60 "words "))))]
                           (is (until #(answered? world p m) 8000)) m)))
            t (thread/thread-of (act/db world) (first lines))]
        (is (string? (field world t :conversation/summary)) "the threshold compressed it")
        (is (some :compressed (keep #(field world % :evaluation/context) (objects/objects-of-class (act/db world) :evaluation))))
        (testing "0 is off"
          (is (zero? (do (ws/request!! root :set-field {:object p :field :serialize/compress-at :type :number :value 0})
                         (conv/compress-at (act/db world) p)))))))))

(deftest conversations-are-listed
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root) {:keys [route w]} (setup root witness world)
          a (say! w p route "what shall we call this\nsecond line")
          _ (is (until #(answered? world p a) 8000))
          r (ws/request!! root :history {:conversations true})
          row (first (filter #(= a (:thread %)) (get-in r [:result :conversations])))]
      (is (:ok r) (pr-str r))
      (is (= "what shall we call this" (:title row)) (pr-str row))
      (is (= route (:route row)))
      (is (>= (:messages row) 2))
      (is (false? (:compressed row)))
      (testing "the witness sees only what it may read"
        (is (seq (get-in (ws/request!! w :history {:conversations true}) [:result :conversations])))))))
