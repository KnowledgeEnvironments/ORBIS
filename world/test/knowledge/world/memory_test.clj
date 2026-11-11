(ns knowledge.world.memory-test
  "Memory for every Principal (sprint item W5): defaults on root at boot and on every
   process Principal at creation; a note a Principal writes to memory/notes from inside its loop is in
   its next Serialization whole."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.memory :as memory]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))
(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))
(defn- reply-to [w sub] (some-> (ws/await-push w sub 15000) :events first :event/body :message))
(defn- serializations-after
  "Serialization ids of the Evaluations a Principal ran after seq, oldest first (from its `evaluated` events)."
  [c stream after]
  (->> (get-in (ws/request!! c :history {:stream stream :after after :limit 1000}) [:result :events])
       (filter #(= :evaluated (:event/kind %)))
       (keep #(get-in % [:event/body :serialization]))
       distinct))
(defn- ser-text [world c id]
  (let [s (:result (ws/request!! c :read {:object id}))]
    (:content (first (j/read-value (ser/bytes-of world s) j/keyword-keys-object-mapper)))))

(deftest every-process-principal-has-memory-defaults
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          fs (fields root root-id)]
      (testing "root, from kernel-zero step 8"
        (is (= memory/policy (v fs :memory/policy)))
        (is (= 4000 (v fs :memory/budget)))
        (is (= 30 (v fs :memory/retention-days)))
        (is (= "" (v fs :memory/notes)))
        (is (= :both (get-in fs [:memory/policy :field/reader]))))
      (testing "a process Principal is born with them; a person is not"
        (let [p (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "agents.m"}}) [:result :object])
              q (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :person :name "people.q"}}) [:result :object])]
          (is (= memory/policy (v (fields root p) :memory/policy)))
          (is (= 4000 (v (fields root p) :memory/budget)))
          (is (nil? (v (fields root q) :memory/policy))))))))

(deftest a-note-written-in-a-wake-is-in-the-next-serialization
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (let [sent (ws/request!! w :send {:to #{root-id} :route route
                                        :text (str "remember this:\n"
                                                   (ops-block (str "{\"op\": \"set-field\", \"args\": {\"object\": \"" root-id
                                                                   "\", \"field\": \"memory/notes\", \"type\": \"string\", \"value\": \"vantage is the user's desktop; it runs the second world\"}}")))})
            reply (reply-to w "inbox")]
        (is (:ok sent) (pr-str sent))
        (is (some? reply) "root replied within 15 s")
        (is (= "vantage is the user's desktop; it runs the second world" (v (fields root root-id) :memory/notes)))
        (testing "the next wake's Serialization carries memory/notes whole, authored by root"
          (let [head (:seq (ws/request!! root :read {:object root-id}))
                sent2 (ws/request!! w :send {:to #{root-id} :route route :text "what is vantage?"})
                reply2 (reply-to w "inbox")
                ser-id (last (serializations-after root root-id head))
                text (when ser-id (ser-text world root ser-id))]
            (is (:ok sent2))
            (is (some? reply2))
            (is (some? ser-id) "a Serialization was made for the second wake")
            (is (str/includes? (str text) (str ser/field-marker " memory/notes (:string)")) (pr-str (take 300 text)))
            (is (str/includes? (str text) "vantage is the user's desktop; it runs the second world"))
            (is (str/includes? (str text) "memory/policy")))))
      (ws/close! w))))
