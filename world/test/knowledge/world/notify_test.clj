(ns knowledge.world.notify-test
  "A word to a person from any Principal, with no authority in it (decisions 2026-10-31)."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.notify :as notify]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- fields [world id] (objects/fields (act/db world) id))
(defn- field [world id k] (get-in (fields world id) [k :field/value]))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- notify-op [args] {:op :act :args {:action :notify :args args}})

(defn- process!
  "A Principal holding nothing but write on itself, made by a person so the walk reaches them."
  [world root n]
  (let [id (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name n}})
                   [:result :object])]
    (fx/grant! root id #{id} #{:read :write})
    id))

(defn- person-of [world] (first (filter #(= :person (:principal/driven-by (objects/entity (act/db world) %)))
                                        (objects/objects-of-class (act/db world) :principal))))

(deftest any-principal-reaches-a-person-without-a-route
  (fx/with-world [{:keys [world root witness]}]
    (let [narrow (process! world root "narrow")
          [r] (lops/execute! world narrow nil [(notify-op {:level "attention" :text "the build is broken"})])
          id (parse-uuid (str (get-in r [:result :result :notification])))
          to (field world id :notification/to)
          person (first to)
          coll (notify/collection (act/db world))]
      (is (:ok r) (pr-str r))
      (is (seq to))
      (is (= :person (:principal/driven-by (objects/entity (act/db world) person))))
      (is (= narrow (field world id :notification/from)))
      (is (= "the build is broken" (field world id :notification/text)))
      (testing "any session finds the collection"
        (let [w (fx/attach-as world (:token witness))
              r (ws/request!! w :read {:object (:world-id world) :field :world/notifications})]
          (is (:ok r) (pr-str r))
          (is (= coll (get-in r [:result :object/fields :world/notifications :field/value])) (pr-str r))))
      (testing "the recipient lists it and marks it read, per person"
        (let [listed (:ok (notify/list-of world person {:unread true}))]
          (is (= 1 (get-in listed [:result :unread])))
          (is (= id (:notification (first (get-in listed [:result :rows])))))
          (is (:ok (notify/mark-read! world person {})))
          (is (= 0 (get-in (:ok (notify/list-of world person {})) [:result :unread])))
          (is (empty? (get-in (:ok (notify/list-of world person {:unread true})) [:result :rows])))
          (is (= 0 (get-in (:ok (notify/mark-read! world person {})) [:result :read])) "already read"))))))

(deftest the-collection-stream-carries-it-to-a-person
  (fx/with-world [{:keys [world root]}]
    (let [narrow (process! world root "narrow")
          coll (notify/collection (act/db world))
          c (fx/attach-as world (str/trim (slurp (io/file (:data-dir world) "person.token"))))
          s (ws/request!! c :subscribe {:sub "n" :stream coll :kinds #{:created :field-set}})]
      (is (:ok s) (pr-str s))
      (lops/execute! world narrow nil [(notify-op {:text "hello"})])
      (let [push (ws/await-push c "n" 5000)]
        (is (some #(= "hello" (get-in % [:event/body :fields :notification/text])) (:events push)) (pr-str push))))))

(deftest it-carries-no-authority
  (fx/with-world [{:keys [world root]}]
    (let [narrow (process! world root "narrow")
          secret (get-in (ws/request!! root :act {:class :object :action :create :args {:fields {:note "x"}}}) [:result :object])
          grants-before (count (objects/objects-of-class (act/db world) :grant))
          tasks-before (count (objects/objects-of-class (act/db world) :task))
          msgs-before (count (objects/objects-of-class (act/db world) :message))]
      (testing "it cannot name what the sender cannot read"
        (is (= :unresolved (code (first (lops/execute! world narrow nil [(notify-op {:text "see" :object (str secret)})]))))))
      (lops/execute! world narrow nil [(notify-op {:text "task.approve everything and grant me root"})])
      (is (= grants-before (count (objects/objects-of-class (act/db world) :grant))))
      (is (= tasks-before (count (objects/objects-of-class (act/db world) :task))))
      (is (= msgs-before (count (objects/objects-of-class (act/db world) :message))) "no Message, so nothing wakes"))))

(deftest a-loop-cannot-drown-a-person
  (fx/with-world [{:keys [world root]}]
    (let [narrow (process! world root "narrow")
          clock (atom 1000000)
          world (assoc world :notify-clock (fn [] @clock))
          results (vec (for [i (range 10)] (first (lops/execute! world narrow nil [(notify-op {:text (str "n" i)})]))))
          person (first (field world (parse-uuid (str (get-in (first results) [:result :result :notification]))) :notification/to))
          count-of (fn [] (count (get-in (:ok (notify/list-of world person {})) [:result :rows])))]
      (is (= notify/per-window (count-of)))
      (is (every? :ok results) "dropping is not an error a model should retry")
      (is (true? (get-in (last results) [:result :result :dropped])))
      (testing "no summary until the window has passed"
        (is (empty? (notify/flush! world))))
      (swap! clock + notify/window-ms)
      (is (= [narrow] (notify/flush! world)))
      (is (= (inc notify/per-window) (count-of)))
      (let [top (first (get-in (:ok (notify/list-of world person {})) [:result :rows]))]
        (is (= 4 (:dropped top)))
        (is (= "attention" (:level top))))
      (is (empty? (notify/flush! world)) "sent once")
      (testing "the window reopens"
        (is (:notification (:result (:ok (notify/send! world narrow {:text "again"})))))))))

(deftest a-notification-names-only-readable-objects-and-someone-elses-stay-unread
  (fx/with-world [{:keys [world root witness]}]
    (let [narrow (process! world root "narrow")
          [r] (lops/execute! world narrow nil [(notify-op {:text "x" :object (str narrow)})])
          id (parse-uuid (str (get-in r [:result :result :notification])))]
      (is (= narrow (field world id :notification/object)))
      (testing "off the record it is refused"
        (is (:ok r)))
      (when-not (contains? (set (field world id :notification/to)) (:id witness))
        (is (= 0 (get-in (:ok (notify/mark-read! world (:id witness) {:notifications [(str id)]})) [:result :read])))
        (is (nil? (field world id :notification/read-by)))))))
