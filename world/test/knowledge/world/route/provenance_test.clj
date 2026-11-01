(ns knowledge.world.route.provenance-test
  "Which client sent a Message (decisions 2026-10-31): message/from is the Principal, so a person
   holding root's token could not be told from root's loop. A session's Message names its token's
   label and its session, a legacy token names only the session, a loop's Message names neither,
   and the model reads the sender as `from <name> on <device>`."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.log.core :as log]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))

(defn- deliveries [c sub pred timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop [acc []]
      (if (or (pred acc) (> (System/currentTimeMillis) deadline))
        acc
        (let [push (ws/await-push c sub 2000)
              ids (keep #(when (= :delivered (:event/kind %)) (get-in % [:event/body :message])) (:events push))]
          (recur (into acc ids)))))))

(deftest a-sessions-message-names-its-client
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          _ (fx/grant! root (:id witness) #{(:id witness)} #{:read})
          _ (fx/grant! root (:id witness) #{route} #{:transmit})
          issued (ws/request!! root :act {:action :token.issue :args {:principal (str (:id witness)) :label "hosttwo"}} 20000)
          secret (get-in issued [:result :result :secret])
          w (fx/attach-as world secret)]
      (is (string? secret) (pr-str issued))
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (testing "a labelled token: device is the label, session the attach, and args cannot name another"
        (let [sent (ws/request!! w :send {:to #{root-id} :route route :text "who am I" :device "forged" :session "forged"})
              m (get-in sent [:result :message])
              fs (fields root m)]
          (is (:ok sent) (pr-str sent))
          (is (= "hosttwo" (v fs :message/device)))
          (is (= (:session w) (v fs :message/session)))
          (is (= (:id witness) (v fs :message/from)))
          (testing "the model reads the sender with the device"
            (let [text (:body (ser/render (act/db world) {:principal root-id :working-set #{m} :message m :depth {}}))]
              (is (re-find #"(?m)^from \[\[.*\]\] on hosttwo$" text) text)
              (is (not (str/includes? text (:session w))) "the session id is not for the model")))
          (testing "a loop's reply names no client, even though it runs through the same handler"
            (let [reply (first (remove #{m} (deliveries w "inbox" seq 20000)))
                  rf (fields root reply)]
              (is (some? reply))
              (is (= root-id (v rf :message/from)))
              (is (nil? (v rf :message/device)))
              (is (nil? (v rf :message/session)))))))
      (ws/close! w))))

(deftest a-legacy-token-names-only-the-session
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          _ (fx/grant! root (:id witness) #{route} #{:transmit})
          secret (session/new-token)
          _ (act/append! world (log/pending :field-set (:id witness) root-id
                                            {:name :principal/token-hash :type :string
                                             :value (session/token-hash secret) :reader :agent}))
          w (fx/attach-as world secret)
          sent (ws/request!! w :send {:to #{root-id} :route route :text "from an old client"})
          fs (fields root (get-in sent [:result :message]))]
      (is (:ok sent) (pr-str sent))
      (is (= (:session w) (v fs :message/session)))
      (is (nil? (v fs :message/device)))
      (testing "no device, no sender line"
        (let [text (:body (ser/render (act/db world) {:principal root-id :working-set #{(get-in sent [:result :message])} :message (get-in sent [:result :message]) :depth {}}))]
          (is (not (re-find #"(?m)^from \[\[" text)) text)))
      (ws/close! w))))
