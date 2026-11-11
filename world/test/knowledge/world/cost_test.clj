(ns knowledge.world.cost-test
  "Cost accounting: a done Evaluation carries evaluation/cost at the Model's
   prices; the books (cost/*) grow on the initiator, the Model and the world; cost.report draws
   a table view of it for people."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.cost :as cost]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args} 20000))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))

(deftest amounts
  (is (= 0.0 (cost/amount {:in 0.0 :out 0.0} {:prompt-tokens 1000 :completion-tokens 10})))
  (is (= 3.0 (cost/amount {:in 1e6 :out 2e6} {:prompt-tokens 1 :completion-tokens 1})) "$1 per prompt token, $2 per completion token")
  (is (= 0.0015 (cost/amount {:in 0.5 :out 1.5} {:prompt-tokens 1000 :completion-tokens 666.6666666666666}))))

(deftest an-evaluation-is-charged-and-the-books-grow
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          wid (:world-id world)
          ;; a priced stub Model: $1 per prompt token, $2 per completion token (per million: 1e6, 2e6)
          r (ws/request!! root :act {:class :model :action :create :args {:kind :model :name "priced" :price-in 1e6 :price-out 2e6}})
          model (get-in r [:result :object])
          _ (is (:ok r) (pr-str r))
          books (fn [t] (let [fs (fields root t)] {:n (long (or (v fs :cost/evaluations) 0)) :in (long (or (v fs :cost/tokens-in) 0))
                                                   :out (long (or (v fs :cost/tokens-out) 0)) :total (double (or (v fs :cost/total) 0))}))
          before (into {} (for [t [root-id model wid]] [t (books t)]))
          {:keys [ok err]} (evaluate/evaluate! world {:principal root-id :working-set #{root-id} :model model :policy :one-shot :sync? true})
          _ (is (nil? err) (pr-str err))
          outcome @(:done ok)
          efs (fields root (:evaluation ok))
          ctx (v efs :evaluation/context)
          c (v efs :evaluation/cost)
          tin (:prompt-tokens ctx) tout (:completion-tokens ctx)]
      (is (= :done (:state outcome)))
      (testing "evaluation/cost: the tokens, the prices at the time, the money"
        (is (pos? tin) "the stub counts prompt tokens as chars/4")
        (is (= (double tin) (:tokens-in c)))
        (is (= (double tout) (:tokens-out c)))
        (is (= 1e6 (:price-in c)))
        (is (= (+ (* 1.0 tin) (* 2.0 tout)) (:amount c)) "at $1 and $2 per token"))
      (testing "the books on the initiator, the Model and the world, the world as actor"
        ;; root and the world were already charged for the kernel-zero Evaluation; the priced Model is new
        (doseq [t [root-id model wid]]
          (let [fs (fields root t) b (get before t)]
            (is (= (inc (:n b)) (v fs :cost/evaluations)) (str t))
            (is (= (+ (:in b) tin) (v fs :cost/tokens-in)))
            (is (= (+ (:out b) tout) (v fs :cost/tokens-out)))
            (is (< (Math/abs (- (+ (:total b) (:amount c)) (double (v fs :cost/total)))) 1e-9))
            (is (inst? (v fs :cost/since)))
            (is (= wid (get-in fs [:cost/total :field/author])) "bookkeeping is the world's")))
        (is (= 1 (v (fields root model) :cost/evaluations)) "the priced Model's first"))
      (testing "a second Evaluation adds up"
        (let [{:keys [ok]} (evaluate/evaluate! world {:principal root-id :working-set #{root-id} :model model :policy :one-shot :sync? true})
              _ @(:done ok)
              fs (fields root model)]
          (is (= 2 (v fs :cost/evaluations)))
          (is (> (v fs :cost/tokens-in) tin))))
      (testing "cost.report: a table view, one row per Principal, the money summed, sent to whom it names"
        (let [witness-id (:id witness)
              route (first (v (fields root root-id) :principal/routes))
              r (act! root "cost.report" {:by "principal" :to [(str witness-id)] :route (str route)})
              res (get-in r [:result :result])]
          (is (:ok r) (pr-str r))
          (is (= :table (:kind res)))
          (let [view (fields root (:serialization res))
                data (v view :view/data)
                row (first (filter #(= "root" (first %)) (:rows data)))]
            (is (= ["principal" "evaluations" "tokens in" "tokens out" "cost"] (mapv :label (:columns data))))
            (is (some? row) (pr-str (:rows data)))
            (is (>= (second row) 2) "root's Evaluations, the kernel-zero one included")
            (is (pos? (double (last row))) "money, since two of them were priced")
            (is (= :view (v (fields root (:message res)) :message/kind)))))
        (let [r (act! root "cost.report" {:by "model" :to [(str (:id witness))] :route (str (first (v (fields root root-id) :principal/routes)))})
              data (v (fields root (get-in r [:result :result :serialization])) :view/data)]
          (is (some #(= "priced" (first %)) (:rows data)) (pr-str (:rows data)))))
      (testing "a narrow Principal reports only what it reads"
        (let [c (fx/attach-as world (:token witness))
              rows (:rows (cost/report (act/db world) (:id witness) {}))]
          (is (empty? rows) "the witness reads none of root's Evaluations")
          (ws/close! c))))))
