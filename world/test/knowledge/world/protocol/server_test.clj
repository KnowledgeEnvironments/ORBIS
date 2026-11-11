(ns knowledge.world.protocol.server-test
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.schema.protocol :as p]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- events-on [client stream]
  (get-in (ws/request!! client :history {:stream stream :limit 1000}) [:result :events]))

(defn- kinds-on
  "Event kinds on a stream, without the resolved events our own history probes append."
  [client stream]
  (vec (remove #{:resolved} (map :event/kind (events-on client stream)))))

(deftest attach-and-detach-are-events
  (fx/with-world [{:keys [world root witness]}]
    (let [w (fx/attach-as world (:token witness) :json)
          _ (fx/grant! root (:id witness) #{(:id witness)} #{:read})
          kinds (kinds-on w (:id witness))]
      (is (some #{:attached} kinds) (pr-str kinds))
      (is (:ok (ws/request!! w :detach {})))
      (is (= :unauthenticated (get-in (ws/request!! w :read {:object (:id witness)}) [:err :code])))
      (ws/close! w)
      (let [kinds (kinds-on root (:id witness))]
        (is (= :detached (last kinds)) (pr-str kinds))))
    (testing "a dropped socket is detached with a reason"
      (let [w (fx/attach-as world (:token witness))]
        (ws/abort! w)
        (Thread/sleep 300)
        (let [ev (last (filter #(= :detached (:event/kind %)) (events-on root (:id witness))))]
          (is (some? ev))
          (is (= "socket-closed" (get-in ev [:event/body :reason]))))))))

(def sprint-1-ops
  "Built beside the session and read set: resolution, messages, serialization, evaluation, tasks,
   the Trigger ops arm and disarm, present, search,
   and the two a socket may send before attach, hello and pair.claim."
  #{:resolve :send :serialize :evaluate :move :arm :disarm :present :search :hello :pair.claim})

(deftest unbuilt-ops-answer-not-implemented
  (fx/with-world [{:keys [root]}]
    (doseq [op (remove (into p/sprint-0-ops sprint-1-ops) p/ops)]
      (let [r (ws/request!! root op (case op
                                      :resolve {:name "x" :op :read}
                                      :search {:text "x"}
                                      :send {:to #{(random-uuid)} :route (random-uuid) :text "hi"}
                                      :move {:task (random-uuid) :to :done}
                                      :evaluate {:working-set #{} :model (random-uuid)}
                                      (:cancel :release :fail) {:evaluation (random-uuid)}
                                      :arm {:predicate {:kind :schedule :every-ms 1000} :evaluation {}}
                                      :disarm {:trigger (random-uuid)}
                                      :serialize {:working-set #{} :model (random-uuid)}
                                      {}))]
        (is (= :not-implemented (get-in r [:err :code])) (str op " -> " (pr-str r)))
        (is (int? (:seq r)))))
    (testing "act with an unbuilt action"
      (is (= :not-implemented (get-in (ws/request!! root :act {:class :serialization :action :create :args {}}) [:err :code])))
      (is (= :not-implemented (get-in (ws/request!! root :act {:object (:principal root) :action :dance :args {}}) [:err :code]))))))

(deftest invalid-and-unauthenticated-frames
  (fx/with-world [{:keys [world root]}]
    (testing "invalid with id -> err invalid"
      (ws/send-raw! root "[\"^ \",\"~:id\",\"bad1\",\"~:op\",\"~:nope\",\"~:args\",[\"^ \"]]")
      (let [f (ws/await-frame root #(= "bad1" (:id %)) 2000)]
        (is (= :invalid (get-in f [:err :code])) (pr-str f))))
    (testing "invalid without id -> ignored"
      (ws/send-raw! root "not even transit")
      (is (nil? (ws/await-frame root (constantly true) 300))))
    (testing "ops before attach"
      (let [c (ws/connect (:port world) :json)]
        (is (= :unauthenticated (get-in (ws/request!! c :read {:object (:principal root)}) [:err :code])))
        (is (= :unauthenticated (get-in (ws/request!! c :attach {:token "wrong"}) [:err :code])))
        (ws/close! c)))
    (testing "every response carries the head"
      (let [r (ws/request!! root :set-field {:object (:principal root) :field :t :type :string :value "v"})]
        (is (= (get-in r [:result :seq]) (:seq r)))))))
