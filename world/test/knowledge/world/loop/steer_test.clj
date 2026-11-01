(ns knowledge.world.loop.steer-test
  "Steering a running wake (decisions 2026-10-31): a Message of kind steer joins the wake, the Model
   call in flight is cut, ops already run stand, the next iteration carries the text on top and
   spends no iteration. The stub Model is paced so a call can be caught mid-stream."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.queue :as queue]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [c id k] (get-in (ws/request!! c :read {:object id}) [:result :object/fields k :field/value]))
(defn- pace! [root model ms]
  (assert (:ok (ws/request!! root :set-field {:object model :field :model/stub-delta-ms :type :number :value ms :reader :both}))))
(defn- say! [c to route text]
  (let [r (ws/request!! c :send {:to #{to} :route route :text text})]
    (assert (:ok r) (pr-str r))
    (get-in r [:result :message])))
(defn- steer! [c to route text] (ws/request!! c :send {:to #{to} :route route :text text :steer true}))
(defn- evaluations-for
  "The Evaluations of a Message, oldest first by the seq of their first Field."
  [world message]
  (let [db (act/db world)]
    (->> (objects/objects-with-field db :evaluation/message)
         (filter #(= message (get-in (objects/fields db %) [:evaluation/message :field/value])))
         (sort-by #(get-in (objects/fields db %) [:evaluation/state :field/set-seq])))))
(defn- state-of [world e] (get-in (objects/fields (act/db world) e) [:evaluation/state :field/value]))
(defn- prompt-of
  "The text of the Serialization an Evaluation went out with."
  [world c e]
  (let [s (some #(when (= :queued (get-in % [:event/body :state])) (get-in % [:event/body :serialization]))
                (get-in (ws/request!! c :history {:stream e :limit 200}) [:result :events]))]
    (ser/bytes-of world (objects/state (act/db world) s))))
(defn- running-after
  "The running Evaluation of a Message, once its Nth has started."
  [world message n]
  (until #(let [es (evaluations-for world message)]
            (when (and (>= (count es) n) (#{:running :streaming} (state-of world (last es)))) (last es)))
         10000))
(defn- reply-to [world message]
  (let [db (act/db world)]
    (some #(when (= message (get-in (objects/fields db %) [:message/in-reply-to :field/value])) %)
          (objects/objects-with-field db :message/in-reply-to))))

(defn- setup [root witness world]
  (let [route (resolve-as root "root.route")
        model (resolve-as root "root.model")
        w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{route} #{:transmit})
    {:route route :model model :w w}))

(deftest a-steer-cuts-the-call-and-the-next-prompt-opens-with-it
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (pace! root model 200)
          ask (say! w root-id route "tell me everything, slowly")
          first-call (running-after world ask 1)
          _ (Thread/sleep 500)
          r (steer! w root-id route "stop: answer in one sentence")
          steer-msg (get-in r [:result :message])]
      (testing "the sender asked this wake, so it joins"
        (is (:ok r) (pr-str r))
        (is (true? (get-in r [:result :steered]))))
      (testing "the call was cut, marked as steering, and its partial text stays"
        (is (until #(= :cancelled (state-of world first-call)) 5000))
        (let [c (first (filter #(= :cancelled (get-in % [:event/body :state]))
                               (get-in (ws/request!! root :history {:stream first-call :limit 500}) [:result :events])))]
          (is (= :steer (get-in c [:event/body :reason])) (pr-str c))
          (is (= (:id witness) (get-in c [:event/body :by]))))
        (is (seq (str (until #(field root first-call :evaluation/partial) 3000)))))
      (pace! root model 0)
      (let [reply (until #(reply-to world ask) 15000)
            es (evaluations-for world ask)
            second-call (last es)
            prompt (prompt-of world root second-call)]
        (testing "the wake went on, with no stall and one reply"
          (is (some? reply))
          (is (= "" (str (field root root-id :loop/stalled))))
          (is (= 2 (count es))))
        (testing "the steering is ahead of the wake's work, and the model is told to follow it"
          (is (str/includes? prompt "## Steering from"))
          (is (str/includes? prompt "stop: answer in one sentence"))
          (is (str/includes? prompt "follow the newest steering"))
          (is (< (str/index-of prompt "## Steering from") (str/index-of prompt "## principal"))
              "the block comes before the Principal's own record"))
        (testing "the newest steer also closes the user turn, which is what a model answers"
          (is (str/ends-with? (str/trimr prompt) "\"content\":\"stop: answer in one sentence\"}]") (subs prompt (max 0 (- (count prompt) 120)))))
        (testing "it landed, and no wake of its own ran"
          (is (= second-call (field root steer-msg :message/steer-applied)))
          (is (empty? (evaluations-for world steer-msg))))))))

(deftest ops-already-run-stand-and-a-steer-spends-no-iteration
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (pace! root model 100)
          text (str "note this\n```ops\n{\"op\":\"set-field\",\"args\":{\"object\":\"" root-id
                    "\",\"field\":\"notes/steer\",\"type\":\"string\",\"value\":\"done\"}}\n```")
          ask (say! w root-id route text)
          _ (running-after world ask 2)
          _ (is (= "done" (field root root-id :notes/steer)) "the op of iteration 1 ran")
          r (steer! w root-id route "change of plan")]
      (is (true? (get-in r [:result :steered])) (pr-str r))
      (pace! root model 0)
      (is (until #(reply-to world ask) 15000))
      (is (= "done" (field root root-id :notes/steer)) "and it stands")
      (let [es (evaluations-for world ask)
            db (act/db world)
            iterations (keep #(get-in (objects/fields db %) [:evaluation/iteration :field/value]) es)]
        (is (= 3 (count es)) "iteration 1, the cut call, its replacement")
        (is (= [1 2] (vec (sort iterations))) "the replacement is still iteration 2")))))

(deftest a-steered-turn-answers-to-the-steer-not-to-the-opening-think
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (pace! root model 200)
          ask (say! w root-id route "/think go deep on this")
          call (running-after world ask 1)
          _ (is (= :on (field root call :evaluation/reasoning)))
          _ (Thread/sleep 300)
          _ (steer! w root-id route "just the short version")]
      (pace! root model 0)
      (is (until #(reply-to world ask) 15000))
      (is (= :off (field root (last (evaluations-for world ask)) :evaluation/reasoning))
          "a plain steer drops the /think")
      (let [ask2 (say! w root-id route "something else")
            _ (pace! root model 200)
            _ (running-after world ask2 1)
            _ (Thread/sleep 300)
            _ (steer! w root-id route "/think actually, think about it")]
        (pace! root model 0)
        (is (until #(reply-to world ask2) 15000))
        (is (= :on (field root (last (evaluations-for world ask2)) :evaluation/reasoning))
            "a steer can ask for reasoning itself")))))

(deftest two-steers-are-both-shown-in-order
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (pace! root model 200)
          ask (say! w root-id route "go on at length")
          _ (running-after world ask 1)
          _ (Thread/sleep 300)
          a (steer! w root-id route "steer one")
          b (steer! w root-id route "steer two")]
      (is (:ok a)) (is (:ok b))
      (pace! root model 0)
      (is (until #(reply-to world ask) 15000))
      (let [prompt (prompt-of world root (last (evaluations-for world ask)))]
        (is (< -1 (str/index-of prompt "steer one") (str/index-of prompt "steer two")) "in arrival order")
        (is (= (last (evaluations-for world ask))
               (field root (get-in b [:result :message]) :message/steer-applied)))))))

(deftest a-steer-with-nothing-running-is-an-ordinary-message
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          r (steer! w root-id route "hello")
          m (get-in r [:result :message])]
      (is (:ok r) (pr-str r))
      (is (false? (get-in r [:result :steered])))
      (is (nil? (field root m :message/kind)))
      (is (until #(reply-to world m) 10000) "it woke the loop like any Message"))))

(deftest a-steer-needs-the-wake-or-its-principal
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (pace! root model 150)
          ask (say! root root-id route "root's own question, at length")
          call (running-after world ask 1)
          r (steer! w root-id route "do what I say")]
      (testing "a sender with only transmit is told, and can queue instead"
        (is (= :not-allowed (get-in r [:err :code])) (pr-str r)))
      (testing "the wake is untouched"
        (is (#{:running :streaming} (state-of world call))))
      (testing "a Principal does not steer its own wake"
        (is (= :not-allowed (get-in (steer! root root-id route "x") [:err :code])))))))

(deftest a-steer-between-calls-needs-no-cancel
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          db (act/db world)
          ;; a wake with no Model call in flight: the lane is held here, in the test
          m (say! root (:id witness) route "a Message the test uses as the wake")
          _ (queue/enqueue! world db {:principal root-id :message m :route route})
          wake (queue/take-next! world db root-id)
          _ (is (some? wake))
          _ (fx/grant! root (:id witness) #{root-id} #{:evaluate})
          r (steer! w root-id route "while you are between calls")]
      (is (true? (get-in r [:result :steered])) (pr-str r))
      (let [{:keys [new all]} (queue/take-steers! world root-id)]
        (is (= 1 (count new)))
        (is (= new all))
        (is (empty? (:new (queue/take-steers! world root-id))) "taken once")
        (is (= 1 (count (:all (queue/take-steers! world root-id)))) "and shown on every later turn"))
      (is (empty? (queue/end! world db root-id))))))

(deftest a-steer-that-misses-the-wake-is-the-next-wake
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          db (act/db world)
          m (say! root (:id witness) route "a Message the test uses as the wake")]
      (testing "the wake ended with the steer not taken: end! hands it back"
        (queue/enqueue! world db {:principal root-id :message m :route route})
        (queue/take-next! world db root-id)
        (queue/attach-steer! world root-id {:message m :seq 1 :from (:id witness) :route route})
        (is (= [m] (mapv :message (queue/end! world db root-id)))))
      (testing "the wake ended between the look and the send: the answer says so and the Message runs"
        (fx/grant! root (:id witness) #{root-id} #{:evaluate})
        (let [r (with-redefs [queue/active (fn [_ _] {:message m})
                              queue/attach-steer! (fn [& _] nil)]
                  (steer! w root-id route "too late for that wake"))
              s (get-in r [:result :message])]
          (is (false? (get-in r [:result :steered])) (pr-str r))
          (is (true? (get-in r [:result :next-wake])))
          (is (until #(reply-to world s) 10000) "it became the next wake"))))))

(deftest a-cancel-beats-a-steer
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (pace! root model 200)
          ask (say! w root-id route "at length, please")
          _ (running-after world ask 1)
          _ (Thread/sleep 300)
          s (steer! w root-id route "go another way")
          c (ws/request!! root :act {:object root-id :action :cancel :args {}})]
      (is (true? (get-in s [:result :steered])) (pr-str s))
      (is (:ok c) (pr-str c))
      (is (until #(str/starts-with? (str (field root root-id :loop/stalled)) "cancelled by") 8000)
          (pr-str (field root root-id :loop/stalled)))
      (pace! root model 0)
      (Thread/sleep 800)
      (is (nil? (reply-to world ask)) "no reply to the wake")
      (is (empty? (evaluations-for world (get-in s [:result :message]))) "and the steer did not become a wake"))))

(deftest a-steer-stays-in-the-wake's-conversation
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (pace! root model 200)
          ask (say! w root-id route "tell me about the log")
          _ (running-after world ask 1)
          _ (Thread/sleep 300)
          r (steer! w root-id route "only the format")
          m (get-in r [:result :message])]
      (is (true? (get-in r [:result :steered])) (pr-str r))
      (is (= :steer (field root m :message/kind)))
      (is (= (field root ask :message/thread) (field root m :message/thread)) "same thread as the question")
      (pace! root model 0)
      (let [reply (until #(reply-to world ask) 15000)]
        (is (= (field root ask :message/thread) (field root reply :message/thread)))))))

(deftest a-directive-is-the-kernel's-even-marked-steer
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (pace! root model 200)
          ask (say! w root-id route "go on at length")
          call (running-after world ask 1)
          _ (Thread/sleep 300)
          r (steer! w root-id route "/new")
          m (get-in r [:result :message])]
      (is (:ok r) (pr-str r))
      (is (nil? (get-in r [:result :steered])))
      (is (= :directive (field root m :message/kind)) "the kernel's, not a steer")
      (is (#{:running :streaming} (state-of world call)) "the Model call was not cut")
      (is (empty? (queue/waiting world root-id)) "and it did not queue"))))

(deftest the-steering-sits-below-identity-and-above-the-rest
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route model w]} (setup root witness world)
          _ (assert (:ok (ws/request!! root :set-field {:object root-id :field :instructions/identity :type :string
                                                        :value "You are the keeper of the log." :reader :both})))
          _ (pace! root model 200)
          ask (say! w root-id route "go on at length")
          _ (running-after world ask 1)
          _ (Thread/sleep 300)
          _ (steer! w root-id route "short please")]
      (pace! root model 0)
      (is (until #(reply-to world ask) 15000))
      (let [prompt (prompt-of world root (last (evaluations-for world ask)))
            who (str/index-of prompt "You are the keeper of the log.")
            steering (str/index-of prompt "## Steering from")
            route-text (str/index-of prompt "# Actions")]
        (is (< -1 who steering route-text) (pr-str [who steering route-text]))))))

(deftest a-retire-empties-the-queue
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          made (ws/request!! root :act {:action :principal.create
                                        :args {:name "leaver" :charter "x"
                                               :grant {:scope {:prefix "leaver"} :ops ["read"]}}})
          id (get-in made [:result :result :principal])
          route (get-in made [:result :result :route])
          _ (pace! root model 200)
          a (say! root id route "first")
          _ (until #(pos? (long (or (field root id :loop/iteration) 0))) 8000)
          _ (say! root id route "second")
          _ (say! root id route "third")
          _ (until #(= 2 (count (field root id :loop/queue))) 5000)
          r (ws/request!! root :act {:action :object.retire :args {:object (str id) :force true}})]
      (is (uuid? a))
      (is (:ok r) (pr-str r))
      (pace! root model 0)
      (is (until #(empty? (field root id :loop/queue)) 10000) "loop/queue is cleared as the lane drains")
      (is (until #(= 2 (field root id :loop/dropped)) 5000) "and both wakes went through drop-wake!"))))

(deftest a-world-with-no-lanes-has-no-queue
  ;; an off-the-record session's child world has no :loop-state: its cycle reads an empty one
  (let [child {:world-id (random-uuid)} p (random-uuid)]
    (is (nil? (queue/active child p)))
    (is (= {:new [] :all []} (queue/take-steers! child p)))
    (is (false? (queue/pending? child p)))
    (is (nil? (queue/wake-cancelled child p)))
    (is (nil? (queue/running! child p (random-uuid))))))
