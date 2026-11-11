(ns knowledge.world.adapters.world-test
  "Two worlds in one process: world.link makes an Adapter of kind world and a bridge
   Principal; world.send carries a line to root on the other world and the answer comes back as a
   Message from the bridge; a Message to the bridge on its Route is forwarded the same way; hello
   tells the worlds apart; a credential in the args is refused."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args} 30000))
(defn- res-of [r] (get-in r [:result :result]))
(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- await-delivered
  "Deliveries to `to` on the client's sub matching pred, within timeout-ms: the Message's fields."
  [c sub pred timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop []
      (when (< (System/currentTimeMillis) deadline)
        (if-let [push (ws/await-push c sub (max 1 (- deadline (System/currentTimeMillis))))]
          (do (ws/ack! c sub (:event/seq (last (:events push))))
              (or (some (fn [ev]
                          (when (= :delivered (:event/kind ev))
                            (let [m (get-in ev [:event/body :message])
                                  f (get-in (ws/request!! c :read {:object m}) [:result :object/fields])]
                              (when (pred f) f))))
                        (:events push))
                  (recur)))
          nil)))))

(deftest two-worlds-talk-through-a-bridge
  (fx/with-world [{:keys [world root]}]
    (let [dir-b (fx/temp-dir)
          world-b (fx/start-world dir-b 0)]
      (try
        (let [root-b (fx/attach-root world-b)
              url-b (str "ws://127.0.0.1:" (:port world-b) "/ws")
              token-file (str (io/file dir-b "root.token"))
              db (fn [] (act/db world))]
          (testing "hello tells the two worlds apart"
            (let [ha (ws/request!! root :hello {}) hb (ws/request!! root-b :hello {})]
              (is (:ok ha)) (is (:ok hb))
              (is (not= (get-in ha [:result :world]) (get-in hb [:result :world])))
              (is (str/includes? (get-in hb [:result :url]) (str (:port world-b))))))
          (testing "a credential in the args is refused"
            (is (= :secret-in-args (get-in (act! root "world.link" {:name "b" :url url-b :key "sk-123"}) [:err :code]))))
          (testing "world.link: Adapter, bridge Principal, Route between root and the bridge"
            (let [r (act! root "world.link" {:name "b" :url url-b :token-file token-file})
                  res (res-of r)]
              (is (:ok r) (pr-str r))
              (is (= (:adapter res) (objects/lookup (db) (:world-id world) "world.b")))
              (is (= (:bridge res) (objects/lookup (db) (:world-id world) "bridge.b")))
              (is (= :world (field (db) (:adapter res) :adapter/binds)))
              (is (= url-b (field (db) (:adapter res) :adapter/locator)))
              (is (= #{(:principal root) (:bridge res)} (set (field (db) (:route res) :route/members))))
              (is (= :conflict (get-in (act! root "world.link" {:name "b" :url url-b :token-file token-file}) [:err :code])) "linked once")
              (let [sub "me" route (:route res)]
                (ws/request!! root :subscribe {:sub sub :streams #{(:principal root)} :after 0 :window 256})
                (testing "world.send: the line reaches root on B and B's answer comes back from bridge.b"
                  (let [s (act! root "world.send" {:world "b" :to "root" :text "bridge hello from A" :timeout-s 60})]
                    (is (:ok s) (pr-str s))
                    (is (= (:bridge res) (:bridge (res-of s))))
                    (let [f (await-delivered root sub #(= (:bridge res) (get-in % [:message/from :field/value])) 60000)]
                      (is (some? f) "the bridge relayed an answer")
                      (is (= route (get-in f [:message/route :field/value])))
                      (is (not (str/starts-with? (str (get-in f [:message/text :field/value])) "[b] could not")) (pr-str (get-in f [:message/text :field/value])))
                      (is (seq (str (get-in f [:message/text :field/value]))))))
                  (testing "B's log carries A's line, sent by B's root-token principal, and B's root's reply"
                    (let [hist (get-in (ws/request!! root-b :history {:stream (:principal root-b) :limit 1000}) [:result :events])
                          msgs (for [ev hist :when (= :delivered (:event/kind ev))] (get-in ev [:event/body :message]))
                          texts (map #(get-in (ws/request!! root-b :read {:object %}) [:result :object/fields :message/text :field/value]) msgs)]
                      (is (some #(= "bridge hello from A" %) texts) (pr-str texts)))))
                (testing "a Message to the bridge on its Route is forwarded and answered the same way"
                  (let [sent (ws/request!! root :send {:to #{(:bridge res)} :route route :text "second line via the bridge"})]
                    (is (:ok sent) (pr-str sent))
                    (let [f (await-delivered root sub #(and (= (:bridge res) (get-in % [:message/from :field/value]))
                                                           (= (get-in sent [:result :message]) (get-in % [:message/in-reply-to :field/value])))
                                             60000)]
                      (is (some? f) "the bridge's loop forwarded and relayed")
                      (is (seq (str (get-in f [:message/text :field/value]))))))))))
          (ws/close! root-b))
        (finally
          (system/stop world-b)
          (fx/delete-tree! dir-b))))))
