(ns knowledge.world.cost
  "Cost accounting (spec 5.4 cost on an Evaluation, principle 'the richness is
   affordable because context is allocated on purpose'). When an Evaluation is done, its
   evaluation/cost Field gains {tokens-in tokens-out price-in price-out amount}: the tokens the
   Model reported and the money at the Model's prices at that moment (per million tokens, 0 when
   nobody set them). The running books, cost/evaluations, cost/tokens-in, cost/tokens-out,
   cost/total and cost/since, are kept on the initiator, on the Model and on the world Principal,
   written by the world as actor like every other bookkeeping Field, under one lock so two lanes
   finishing at once do not lose a count. `report` folds the Evaluations a caller may read into
   the rows of a table view (the action cost.report)."
  (:require [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.view.core :as view])
  (:import [java.util Date]))

(def fields [:cost/evaluations :cost/tokens-in :cost/tokens-out :cost/total :cost/since])

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn prices
  "{:in :out :priced?} per million tokens; a Model with neither price Field is unpriced: the amount is 0 and evaluation/cost says priced false."
  [db model]
  (let [in (field db model :model/price-in) out (field db model :model/price-out)]
    {:in (double (or in 0)) :out (double (or out 0)) :priced? (boolean (or (number? in) (number? out)))}))

(defn amount
  "Money for a usage at prices per million tokens."
  [{:keys [in out]} {:keys [prompt-tokens completion-tokens]}]
  (/ (+ (* (double (or prompt-tokens 0)) in) (* (double (or completion-tokens 0)) out)) 1e6))

(defonce ^:private books (Object.))

(defn- bump!
  "The running books on one Object, the world as actor."
  [world target {:keys [tokens-in tokens-out amount]}]
  (let [db (act/db world)
        n (fn [k] (double (or (field db target k) 0)))
        ap (fn [k type v] (log/append!! (:log world) (log/pending :field-set target (:world-id world) {:name k :type type :value v :reader :both})))]
    (ap :cost/evaluations :number (inc (long (n :cost/evaluations))))
    (ap :cost/tokens-in :number (long (+ (n :cost/tokens-in) tokens-in)))
    (ap :cost/tokens-out :number (long (+ (n :cost/tokens-out) tokens-out)))
    (ap :cost/total :number (+ (n :cost/total) amount))
    (when (nil? (field db target :cost/since)) (ap :cost/since :instant (Date.)))))

(defn charge!
  "An Evaluation is done: evaluation/cost on it (as the initiator, via the Evaluation, under its
   Grant) and the books on the initiator, the Model and the world Principal. Returns the cost map."
  [world {:keys [evaluation principal model grant usage]}]
  (let [db (act/db world)
        p (prices db model)
        tin (long (or (:prompt-tokens usage) 0))
        tout (long (or (:completion-tokens usage) 0))
        amt (amount p usage)
        cost {:tokens-in (double tin) :tokens-out (double tout) :price-in (:in p) :price-out (:out p) :amount amt :priced (:priced? p)}]
    (log/append!! (:log world) (log/pending :field-set evaluation principal {:name :evaluation/cost :type :map :value cost :reader :both}
                                            :delegation grant :via evaluation))
    (locking books
      (doseq [t (distinct [principal model (:world-id world)])]
        (bump! world t {:tokens-in tin :tokens-out tout :amount amt})))
    cost))

(defn- day-of [^Date d] (subs (str (.toInstant d)) 0 10))

(defn report
  "The rows of cost.report for the caller: every Evaluation it may read that is done
   (evaluation/context set), grouped by :principal, :model or :day, after `since` (an instant) when
   given. Returns {:columns :rows :about :count}: rows [group evaluations tokens-in tokens-out cost],
   most expensive first; about = the Models involved."
  [db p {:keys [by since]}]
  (let [by (or (#{:principal :model :day} (some-> by name keyword)) :principal)
        evs (for [e (objects/objects-of-class db :evaluation)
                  :when (resolve/resolves? db p (act/id-name e) :read)
                  :let [fs (objects/fields db e)
                        ctx (get-in fs [:evaluation/context :field/value])
                        at (get-in fs [:evaluation/context :field/set-at])]
                  :when (map? ctx)
                  :when (or (nil? since) (and (inst? at) (not (.before ^Date at ^Date since))))]
              {:principal (get-in fs [:evaluation/initiator :field/value])
               :model (get-in fs [:evaluation/model :field/value])
               :day (if (inst? at) (day-of at) "unknown")
               :in (long (or (:prompt-tokens ctx) 0))
               :out (long (or (:completion-tokens ctx) 0))
               :amount (double (or (get-in fs [:evaluation/cost :field/value :amount]) 0))})
        groups (group-by by evs)
        rows (vec (for [[k xs] (sort-by (fn [[_ xs]] (- (reduce + 0.0 (map :amount xs)))) groups)]
                    [(if (= by :day) (str k) (if k (view/name-of db k) "?"))
                     (count xs)
                     (reduce + 0 (map :in xs))
                     (reduce + 0 (map :out xs))
                     (reduce + 0.0 (map :amount xs))]))]
    {:columns [{:key (name by) :label (name by)}
               {:key "evaluations" :label "evaluations" :align "right"}
               {:key "tokens-in" :label "tokens in" :align "right"}
               {:key "tokens-out" :label "tokens out" :align "right"}
               {:key "cost" :label "cost" :align "right"}]
     :rows rows
     :about (vec (distinct (keep :model evs)))
     :count (count evs)}))
