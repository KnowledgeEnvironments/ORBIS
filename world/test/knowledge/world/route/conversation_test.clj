(ns knowledge.world.route.conversation-test
  "A person types lines, not threads: a plain line continues the open conversation on the Route, the
   model sees it whole, and a pause or an explicit reply starts or keeps what it should."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.thread :as thread]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))

(defn- until [pred ms]
  (let [deadline (+ (System/currentTimeMillis) ms)]
    (loop []
      (or (pred)
          (when (< (System/currentTimeMillis) deadline) (Thread/sleep 50) (recur))))))

(defn- setup [root witness world]
  (let [route (resolve-as root "root.route")
        w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{(:id witness) route} #{:read :transmit})
    {:route route :w w}))

(defn- say! [w root-id route text]
  (get-in (ws/request!! w :send {:to #{root-id} :route route :text text}) [:result :message]))

(defn- answered? [world root-id m] (thread/answered-by? (act/db world) m root-id))

(deftest five-lines-are-one-conversation
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          lines (vec (for [i (range 5)]
                       (let [m (say! w root-id route (str "line number " i " of the talk"))]
                         (is (until #(answered? world root-id m) 8000) (str "line " i " was answered"))
                         m)))
          db (act/db world)
          threads (map #(thread/thread-of db %) lines)]
      (testing "one thread, rooted at the first line"
        (is (= #{(first lines)} (set threads))))
      (testing "each later line replies to what was last said to the person"
        (doseq [m (rest lines)]
          (let [parent (get-in (objects/fields db m) [:message/in-reply-to :field/value])]
            (is (some? parent))
            (is (= root-id (get-in (objects/fields db parent) [:message/from :field/value]))))))
      (testing "the fifth wake's Serialization holds the first line"
        (let [body (:body (ser/render db {:principal root-id :working-set #{(last lines) (first lines)}
                                          :route route :message (last lines)}))]
          (is (str/includes? body "line number 0 of the talk"))
          (is (str/includes? body "line number 1 of the talk") "the thread block lists the middle of it")))
      (testing "the asker of the thread is the person"
        (is (= (:id witness) (thread/asked-by db (last lines))))))))

(deftest a-pause-or-a-loop-starts-its-own
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          a (say! w root-id route "first talk")]
      (is (until #(answered? world root-id a) 8000))
      (testing "a recipient may set the idle limit to nothing: the next line is a new conversation"
        (is (:ok (ws/request!! root :set-field {:object root-id :field :serialize/conversation-idle-s :type :number :value 0})))
        (let [b (say! w root-id route "after a long pause")]
          (is (= b (thread/thread-of (act/db world) b)))
          (is (until #(answered? world root-id b) 8000))))
      (testing "an explicit in-reply-to is never second-guessed"
        (is (:ok (ws/request!! root :set-field {:object root-id :field :serialize/conversation-idle-s :type :number :value 1800})))
        (let [c (get-in (ws/request!! w :send {:to #{root-id} :route route :text "back to the first" :in-reply-to a}) [:result :message])]
          (is (= a (thread/thread-of (act/db world) c))))))))

(deftest a-new-line-is-a-new-exchange
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          {:keys [route w]} (setup root witness world)
          a (say! w root-id route "run the tests")
          _ (is (until #(answered? world root-id a) 8000))
          b (say! w root-id route "run them again")
          _ (is (until #(answered? world root-id b) 8000))
          db (act/db world)]
      (is (= (thread/thread-of db a) (thread/thread-of db b)))
      (testing "what the thread already did is what this exchange did"
        (is (= b (first (thread/current-exchange db (thread/thread-of db b)))))
        (is (not (some #{a} (thread/current-exchange db (thread/thread-of db b)))))))))
