(ns knowledge.world.loop.threads-test
  "a loop remembers what it sent in a wake (a repeated send is answered, not sent),
   Messages carry their thread and who asked, and a loop that asked others on behalf of a person
   relays their answers to that person once they are all in, in-reply-to the original."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))
(defn- field [c id k] (v (fields c id) k))

(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))
(defn- relay-block [& lines] (str "```ops-relay\n" (str/join "\n" lines) "\n```"))

(defn- send-line
  ([to route text] (send-line to route text nil))
  ([to route text in-reply-to]
   (str "{\"op\": \"send\", \"args\": {\"to\": [\"" to "\"], \"route\": \"" route "\", \"text\": \"" text "\""
        (when in-reply-to (str ", \"in-reply-to\": \"" in-reply-to "\"")) "}}")))

(defn- process-principal!
  "A process Principal root creates, with its own Route on the listener transport and root's stub
   Model; it may read and transmit on its own things, evaluate on the Model, read the witness."
  [root witness-id nm]
  (let [model (resolve-as root "root.model")
        transport (resolve-as root "transport.listener")
        p (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name nm}}) [:result :object])
        r (get-in (ws/request!! root :act {:class :route :action :create :args {:transport transport :address nm :template (str "You are " nm ".")}}) [:result :object])]
    (fx/grant! root p #{p r} #{:read :write :transmit})
    (fx/grant! root p #{model} #{:evaluate})
    (fx/grant! root p #{witness-id} #{:read})
    (is (:ok (ws/request!! root :set-field {:object p :field :principal/routes :type [:list [:ref :route]] :value [r]})))
    (is (:ok (ws/request!! root :set-field {:object p :field :principal/models :type [:list [:ref :model]] :value [model]})))
    {:id p :route r}))

(defn- deliveries
  "Message ids delivered to the witness on sub as they arrive, until pred holds of the vector or ms pass."
  [w sub pred ms]
  (let [deadline (+ (System/currentTimeMillis) ms)]
    (loop [got []]
      (if (or (pred got) (> (System/currentTimeMillis) deadline))
        got
        (if-let [push (ws/await-push w sub (max 1 (- deadline (System/currentTimeMillis))))]
          (recur (into got (keep #(get-in % [:event/body :message]) (:events push))))
          got)))))

(defn- messages-from
  "Messages a Principal sent, as {:id :text :to :thread :in-reply-to}, oldest first (its `sent` events)."
  [root p]
  (->> (get-in (ws/request!! root :history {:stream p :limit 1000}) [:result :events])
       (filter #(and (= :sent (:event/kind %)) (= p (:event/actor %))))
       (map #(get-in % [:event/body :message]))
       distinct
       (map (fn [m] (let [fs (fields root m)]
                      {:id m :text (v fs :message/text) :to (v fs :message/to) :thread (v fs :message/thread)
                       :in-reply-to (v fs :message/in-reply-to) :asked-by (v fs :message/asked-by)})))))

(deftest a-send-repeated-within-a-wake-is-not-sent-twice
  ;; block 1 sends X to the witness; block 2 (the next iteration) sends the same X again: the
  ;; second answers the first Message's id with duplicate true, and the witness gets X once
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      ;; asked by someone else: a send to the asker would be the answer itself (loop.answer-test)
      (let [line (send-line (:id witness) route "ping from root")
            head (:seq (ws/request!! root :read {:object root-id}))
            other (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :person :name "other"}}) [:result])
            _ (fx/grant! root (:object other) #{route} #{:transmit})
            o (fx/attach-as world (get-in other [:result :token]))
            sent (ws/request!! o :send {:to #{root-id} :route route
                                        :text (str "send them a ping, twice:\n" (ops-block line line) "\n" (ops-block line))})
            got (deliveries w "inbox" #(>= (count %) 1) 20000)
            _ (Thread/sleep 1500)
            got (into got (deliveries w "inbox" (constantly true) 300))
            texts (map #(field root % :message/text) got)
            pings (filter #(= "ping from root" %) texts)
            evals (->> (get-in (ws/request!! root :history {:stream root-id :after head :limit 2000}) [:result :events])
                       (filter #(= :evaluated (:event/kind %)))
                       (map #(get-in % [:event/body :evaluation])) distinct)
            results (fn [e] (field root e :evaluation/results))]
        (is (:ok sent) (pr-str sent))
        (Thread/sleep 500)
        (is (= 1 (count pings)) (str "one ping reached the witness: " (pr-str texts)))
        (let [r1 (results (first evals)) r2 (results (second evals))]
          (testing "within one block: the second line answers the first's id"
            (is (true? (:ok (first r1))) (pr-str r1))
            (is (true? (:ok (second r1))))
            (is (true? (get-in (second r1) [:result :duplicate])) (pr-str (second r1)))
            (is (= (get-in (first r1) [:result :message]) (get-in (second r1) [:result :message]))))
          (testing "across iterations: the log is the memory"
            (is (true? (get-in (first r2) [:result :duplicate])) (pr-str r2))
            (is (= (get-in (first r1) [:result :message]) (get-in (first r2) [:result :message])))))
        (is (= "" (field root root-id :loop/stalled))))
      (ws/close! w))))

(deftest messages-carry-their-thread-and-who-asked
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (let [sent (ws/request!! w :send {:to #{root-id} :route route :text "what do you know?"})
            orig (get-in sent [:result :message])
            reply (first (deliveries w "inbox" #(>= (count %) 1) 15000))
            ofs (fields root orig) rfs (fields root reply)]
        (is (:ok sent))
        (testing "a first Message is its own thread, asked by its sender"
          (is (= orig (v ofs :message/thread)))
          (is (= (:id witness) (v ofs :message/asked-by))))
        (testing "the reply inherits the thread and the asker"
          (is (some? reply))
          (is (= orig (v rfs :message/in-reply-to)))
          (is (= orig (v rfs :message/thread)))
          (is (= (:id witness) (v rfs :message/asked-by)))))
      (ws/close! w))))

(deftest root-relays-the-answers-to-the-person-who-asked
  ;; person -> root: ask A and B, relay. Root's first wake sends to A and B (once each); A and B
  ;; answer on their own Routes at hop 2 with root's thread; root wakes on each; on the second
  ;; answer nothing is awaiting and root relays to the person, in-reply-to the original.
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))
          a (process-principal! root (:id witness) "agents.a")
          b (process-principal! root (:id witness) "agents.b")]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (let [text (str "Ask A and B for a one-line status and relay both to me in one message.\n"
                      (ops-block (send-line (:id a) (:route a) "status?") (send-line (:id b) (:route b) "status?")) "\n"
                      (relay-block (send-line (:id witness) route "relay: A and B answered" "<thread>")))
            sent (ws/request!! w :send {:to #{root-id} :route route :text text})
            orig (get-in sent [:result :message])
            relay? (fn [m] (str/starts-with? (str (field root m :message/text)) "relay:"))
            got (deliveries w "inbox" #(some relay? %) 90000)
            relay (first (filter relay? got))]
        (is (:ok sent) (pr-str sent))
        (is (some? relay) (str "root relayed within 90 s; witness got: " (pr-str (map #(field root % :message/text) got))))
        (when relay
          (let [rfs (fields root relay)]
            (testing "the relay answers the original, in its thread, from root"
              (is (= orig (v rfs :message/in-reply-to)))
              (is (= orig (v rfs :message/thread)))
              (is (= (:id witness) (v rfs :message/asked-by)))
              (is (= root-id (v rfs :message/from))))))
        (testing "root asked each of A and B exactly once, in the person's thread"
          (let [asks (filter #(= "status?" (:text %)) (messages-from root root-id))]
            (is (= 2 (count asks)) (pr-str asks))
            (is (= #{#{(:id a)} #{(:id b)}} (set (map :to asks))))
            (is (every? #(= orig (:thread %)) asks))
            (is (every? #(= (:id witness) (:asked-by %)) asks))))
        (testing "A's answer carries the same thread and reached root"
          (let [answers (messages-from root (:id a))]
            (is (seq answers))
            (is (every? #(= orig (:thread %)) answers) (pr-str answers))))
        (is (until #(= "" (field root root-id :loop/stalled)) 20000)))
      (ws/close! w))))
