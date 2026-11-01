(ns knowledge.world.world-surface-test
  "The world line every Principal sees, history's newest page (before/last), the
   repeat-of rule on required args, courtesy replies dropped at hop 3, and the approval card's
   expiry on the Message."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [datascript.core :as d]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.loop.core :as lcore]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.route.core :as route]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws])
  (:import [java.util Date]))

(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))

(deftest required-args-and-the-repeat-key
  (is (= #{:kind :title :data} (actions/required-args "view.present")))
  (is (= #{:adapter :cmd} (actions/required-args "task.run")))
  (is (nil? (actions/required-args "no.such.action")))
  (testing "an optional arg added on a retry does not make a new op"
    (let [a {:action "view.present" :args {:kind "table" :title "t" :data {:rows []}}}
          b {:action "view.present" :args {:kind "table" :title "t" :data {:rows []} :in-reply-to "0fe4"}}]
      (is (= (lops/repeat-key :act a) (lops/repeat-key :act b)))
      (is (some? (lops/repeat-of [{:op "act" :args a :ok true :seq 7}] :act b)))))
  (testing "a different required arg is a different op"
    (is (not= (lops/repeat-key :act {:action "task.run" :args {:adapter "x" :cmd "ls"}})
              (lops/repeat-key :act {:action "task.run" :args {:adapter "x" :cmd "ls -l"}})))))

(deftest courtesy-replies
  (is (lcore/courtesy-reply? "Thanks, noted."))
  (is (lcore/courtesy-reply? "Acknowledged — I have both statuses and nothing is pending."))
  (is (lcore/courtesy-reply? ""))
  (is (not (lcore/courtesy-reply? "Thanks. Could you also run the exit suite?")) "a question is not noise")
  (is (not (lcore/courtesy-reply? "The build failed at step 3: ld: symbol not found _foo (listener/src/main.cpp:12).")))
  (is (not (lcore/courtesy-reply? (apply str (repeat 45 "noted ")))) "over 200 chars is not a mere acknowledgement"))

(deftest the-world-line-and-the-newest-page
  (fx/with-world [{:keys [world root witness]}]
    (let [db (act/db world)
          wl (ser/world-line db)]
      (testing "the world line names the world, and opens every Serialization body"
        (is (string? wl))
        (is (str/starts-with? wl "world "))
        (let [r (ser/render db {:principal (:principal root) :working-set #{(:principal root)}})]
          (is (str/starts-with? (:body r) wl))))
      (testing "history {last n} is the newest n events, ascending, with prev for the page before"
        (let [{:keys [result ok]} (ws/request!! root :history {:last 5})
              seqs (mapv :event/seq (:events result))]
          (is ok)
          (is (= 5 (count seqs)))
          (is (= seqs (sort seqs)))
          (is (= (:prev result) (first seqs)))
          (let [{r2 :result} (ws/request!! root :history {:before (:prev result) :last 3})
                s2 (mapv :event/seq (:events r2))]
            (is (= 3 (count s2)))
            (is (< (peek s2) (first seqs)) "all before the earlier page"))))
      (testing "a narrow Principal's newest page holds only what it may read"
        (let [w (fx/attach-as world (:token witness))
              {:keys [result]} (ws/request!! w :history {:last 50})]
          (is (every? (fn [e] (or (= (:id witness) (:event/stream e)) (some #{(:id witness)} (:event/streams e))))
                      (:events result))))))))

(deftest an-approval-card-carries-its-expiry
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          expires (Date. (+ (System/currentTimeMillis) 60000))
          _ (fx/grant! root (:id witness) #{(:id witness)} #{:read})
          {:keys [ok]} (route/send! world root-id {:to #{(:id witness)} :route route :text "approval needed: echo hi"
                                                   :kind :approval :expires-at expires})
          w (fx/attach-as world (:token witness))
          fs (fields w (:message ok))]
      (is (some? ok))
      (is (= :approval (keyword (name (v fs :message/kind)))))
      (is (instance? Date (v fs :message/expires-at)))
      (is (= (.getTime expires) (.getTime ^Date (v fs :message/expires-at)))))))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))

(deftest a-gated-action-that-cannot-run-is-refused-before-the-gate
  (fx/with-world [{:keys [world root witness]}]
    (let [db (act/db world)
          root-id (:principal root)
          bad-delegate {:name "dev-x" :charter "c" :grant {:scope {:prefix "dev-x"} :ops ["read" "delegate"]}}
          no-grant {:name "dev-y" :charter "c"}]
      (testing "the loop's execute! answers the err at once and makes no Task"
        (let [tasks (fn [db] (or (d/q '[:find (count ?o) . :where [?o :object/class :task]] db) 0))
              before (tasks db)
              rs (lops/execute! world root-id nil 1
                                [{:op :act :args {:action "principal.create" :args bad-delegate}}
                                 {:op :act :args {:action "principal.create" :args no-grant}}]
                                {:max-ops 16})
              after (tasks (act/db world))]
          (is (= [:narrower-required :invalid-args] (mapv #(keyword (name (get-in % [:err :code]))) rs)) (pr-str rs))
          (is (= before after) "no approval Task for an op that could not run"))))))

(deftest a-relative-mount-path-means-under-the-harness-root
  (fx/with-world [{:keys [world root]}]
    (let [r (act! root "workspace.mount" {:principal "root" :path "docs" :name "docs-x"})]
      (is (:ok r) (pr-str r))
      (is (str/ends-with? (str (or (get-in r [:result :path]) (get-in r [:result :result :path]))) "/docs") (pr-str (:result r))))))
