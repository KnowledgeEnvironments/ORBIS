(ns knowledge.world.act.rename-test
  "Root is found by world/root, not by its name (decisions 2026-10-31), so object.rename can rename
   it: gates, a reboot on the same data, principal.create and a world.send default all keep working,
   the old name stops resolving and a taken name is refused. People and loops are bound where a
   recipient can name them, and a sender is named to its recipient from any namespace."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args} 20000))
(defn- res-of [r] (get-in r [:result :result]))
(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- field [c id k] (get-in (fields c id) [k :field/value]))
(defn- kind-of [c m] (some-> (field c m :message/kind) name keyword))
(defn- code [r] (get-in r [:err :code]))
(def run-line "{\"op\": \"act\", \"args\": {\"action\": \"task.run\", \"args\": {\"adapter\": \"shell.local\", \"cmd\": \"echo renamed-ok\"}}}")

(defn- deliveries [c sub pred timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop [acc []]
      (if (or (pred acc) (> (System/currentTimeMillis) deadline))
        acc
        (let [push (ws/await-push c sub 2000)
              ids (keep #(when (= :delivered (:event/kind %)) (get-in % [:event/body :message])) (:events push))]
          (recur (into acc ids)))))))

(deftest rename-root-and-everything-still-finds-it
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)]
      (testing "refused: a taken name, a non-writer"
        (is (= :conflict (code (act! root "object.rename" {:object "root" :name "person"}))))
        (let [w (fx/attach-as world (:token witness))]
          (is (not (:ok (act! w "object.rename" {:object "root" :name "mallory"}))))
          (ws/close! w))
        (is (= root-id (resolve-as root "root" :read))))
      (testing "the rename: one step, in every namespace that held the name"
        (let [r (act! root "object.rename" {:object "root" :name "ivy"})]
          (is (:ok r) (pr-str r))
          (is (= "root" (:was (res-of r))))
          (is (some #{"root.route"} (:related (res-of r))) "dependent names are listed, not touched")
          (is (seq (:prefix-grants (res-of r))) "the person's prefix Grant is reported, not moved"))
        (is (nil? (resolve-as root "root" :read)) "the old name no longer resolves")
        (is (= root-id (resolve-as root "ivy" :read)))
        (is (= route (resolve-as root "root.route" :read)))
        (is (= root-id (objects/root-id (act/db world))))
        (is (= root-id (kz/root-of world))))
      (testing "principal.create after the rename"
        (let [r (act! root "principal.create" {:name "later" :charter "After the rename."
                                               :grant {:scope {:prefix "later"} :ops ["read"]}})]
          (is (:ok r) (pr-str r))
          (is (= "ivy.later" (:route-name (res-of r))))))
      (testing "an approval gate after the rename"
        (fx/grant! root (:id witness) #{(:id witness)} #{:read})
        (fx/grant! root (:id witness) #{route} #{:transmit})
        (let [w (fx/attach-as world (:token witness))
              _ (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
              sent (ws/request!! w :send {:to #{root-id} :route route
                                          :text (str "please run this:\n```ops\n" run-line "\n```")})
              got (deliveries w "inbox" (fn [ms] (some #(= :approval (kind-of root %)) ms)) 20000)
              card (first (filter #(= :approval (kind-of root %)) got))
              task (when card (field root card :message/task))
              r (when task (act! w "task.approve" {:task (str task)}))]
          (is (:ok sent) (pr-str sent))
          (is (some? card) (pr-str got))
          (is (= "done" (:state (res-of r))) (pr-str r))
          (ws/close! w))))))

(deftest a-renamed-root-boots-on-the-same-data
  (let [dir (fx/temp-dir)
        world (fx/start-world dir 0)]
    (try
      (let [root (fx/attach-root world)
            root-id (:principal root)]
        (is (:ok (act! root "object.rename" {:object "root" :name "ivy"})))
        (ws/close! root)
        (system/stop world)
        (let [again (fx/start-world dir 0)]
          (try
            (let [c (fx/attach-root again)]
              (is (= root-id (:principal c)) "root.token still opens root")
              (is (= root-id (resolve-as c "ivy" :read)))
              (is (nil? (resolve-as c "root" :read)) "boot did not rebind the old name")
              (is (= root-id (kz/root-of again)))
              (is (.exists (io/file dir "root.token")))
              (ws/close! c))
            (finally (system/stop again)))))
      (finally (fx/delete-tree! dir)))))

(deftest a-world-send-with-no-target-finds-a-renamed-root
  (fx/with-world [{:keys [root]}]
    (let [dir-b (fx/temp-dir)
          world-b (fx/start-world dir-b 0)]
      (try
        (let [root-b (fx/attach-root world-b)
              url-b (str "ws://127.0.0.1:" (:port world-b) "/ws")
              _ (is (:ok (act! root-b "object.rename" {:object "root" :name "ivy"})))
              _ (is (= (:principal root-b) (get-in (ws/request!! root-b :hello {}) [:result :root])))
              link (act! root "world.link" {:name "b" :url url-b :token-file (str (io/file dir-b "root.token"))})
              s (act! root "world.send" {:world "b" :text "to whoever is root there" :timeout-s 30})
              deadline (+ (System/currentTimeMillis) 30000)
              texts (fn [] (let [h (get-in (ws/request!! root-b :history {:stream (:principal root-b) :limit 1000}) [:result :events])]
                             (keep #(when (= :delivered (:event/kind %))
                                      (field root-b (get-in % [:event/body :message]) :message/text)) h)))]
          (is (:ok link) (pr-str link))
          (is (:ok s) (pr-str s))
          (loop []
            (when (and (not-any? #{"to whoever is root there"} (texts)) (< (System/currentTimeMillis) deadline))
              (Thread/sleep 200)
              (recur)))
          (is (some #{"to whoever is root there"} (texts)))
          (ws/close! root-b))
        (finally (system/stop world-b) (fx/delete-tree! dir-b))))))

(deftest people-and-loops-are-named-where-a-recipient-can-see-them
  (fx/with-world [{:keys [world root witness]}]
    (let [ws-id (:world-id world)
          db (fn [] (act/db world))]
      (testing "a person made by act create is bound in the world namespace; a taken name is refused"
        (is (= (:id witness) (objects/lookup (db) ws-id "test/witness")))
        (is (= :conflict (code (ws/request!! root :act {:class :principal :action :create
                                                        :args {:driven-by :person :name "test/witness"}})))))
      (testing "principal.create binds in the world namespace and refuses a taken name"
        (let [r (act! root "principal.create" {:name "narrow" :charter "Narrow."
                                               :grant {:scope {:prefix "narrow"} :ops ["read"]}})
              narrow (:principal (res-of r))]
          (is (:ok r) (pr-str r))
          (is (= narrow (objects/lookup (db) ws-id "narrow")))
          (is (= :conflict (code (act! root "principal.create" {:name "narrow" :charter "Again."
                                                                :grant {:scope {:prefix "narrow"} :ops ["read"]}}))))
          (testing "a sender bound only in another namespace is still named to its recipient"
            (let [quiet (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "quiet"}})
                  q (get-in quiet [:result :object])
                  ;; root's names are the world's now; take this one back so only root's namespace holds it
                  _ (ws/request!! root :unbind {:namespace ws-id :name "quiet"})
                  route (:route (res-of r))
                  _ (fx/grant! root q #{route} #{:transmit})
                  c (fx/attach-as world (get-in quiet [:result :result :token]))
                  sent (ws/request!! c :send {:to #{narrow} :route route :text "who is this"})
                  m (get-in sent [:result :message])
                  body (:body (ser/render (db) {:principal narrow :working-set #{m} :message m :depth {}}))]
              (is (:ok sent) (pr-str sent))
              (is (nil? (objects/lookup (db) ws-id "quiet")) "only in root's namespace, the case at issue")
              (is (str/includes? body (str "[[" q "|quiet|principal]]")) body)
              (ws/close! c)
              (testing "upgrade! binds it, a name grants nothing"
                (is (some #(= q (first %)) (act/unbound-in-world (db))))
                (kz/upgrade! world)
                (is (= q (objects/lookup (db) ws-id "quiet")))))))))))
