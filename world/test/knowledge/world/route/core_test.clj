(ns knowledge.world.route.core-test
  "send: a Message Object, sent on the sender's and the Route's streams, delivered on each
   recipient's (spec 5.2, 9.1), under :transmit on the Route."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- events [c stream] (get-in (ws/request!! c :history {:stream stream :limit 1000}) [:result :events]))
(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))

(deftest send-on-a-route
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))]
      (is (uuid? route) "kernel-zero bound root.route")
      (testing "root sends to the witness"
        (let [r (ws/request!! root :send {:to #{(:id witness)} :route route :text "hello" :tags [root-id]})
              m (get-in r [:result :message])
              msg (:result (ws/request!! root :read {:object m}))
              f #(get-in msg [:object/fields % :field/value])]
          (is (:ok r) (pr-str r))
          (is (= :message (:object/class msg)))
          (is (= root-id (f :message/from)))
          (is (= #{(:id witness)} (f :message/to)))
          (is (= route (f :message/route)))
          (is (= "hello" (f :message/text)))
          (is (= [root-id] (f :message/tags)))
          (is (some #(and (= :sent (:event/kind %)) (= m (get-in % [:event/body :message]))) (events root root-id)) "sent on the sender's stream")
          (is (some #(and (= :sent (:event/kind %)) (= m (get-in % [:event/body :message]))) (events root route)) "sent on the Route's stream")
          (is (some #(and (= :delivered (:event/kind %)) (= m (get-in % [:event/body :message]))
                          (= (:id witness) (get-in % [:event/body :recipient])))
                    (events root (:id witness))) "delivered on the recipient's stream")))
      (testing "the witness needs :transmit on the Route"
        (is (= :unresolved (get-in (ws/request!! w :send {:to #{root-id} :route route :text "hi"}) [:err :code])))
        (fx/grant! root (:id witness) #{route} #{:transmit})
        (let [r (ws/request!! w :send {:to #{root-id} :route route :text "hi"})]
          (is (:ok r) (pr-str r))))
      (testing "not a Route, or an unknown recipient"
        (is (= :unresolved (get-in (ws/request!! root :send {:to #{root-id} :route root-id :text "x"}) [:err :code])))
        (is (= :unresolved (get-in (ws/request!! root :send {:to #{(random-uuid)} :route route :text "x"}) [:err :code]))))
      (ws/close! w))))
