(ns knowledge.world.loop.concurrency-test
  "One lane per Principal: two process Principals evaluate at the same time, one
   Principal's wakes run strictly in order, and loop/concurrency on the world Principal is the bound."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.loop.core :as rloop]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [c id k] (get-in (ws/request!! c :read {:object id}) [:result :object/fields k :field/value]))

(defn- process-principal!
  "A process Principal with its own Route on the listener transport and root's stub Model; the
   witness may transmit on the Route. Returns {:id :route}."
  [root witness-id nm]
  (let [model (resolve-as root "root.model")
        transport (resolve-as root "transport.listener")
        p (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name nm}}) [:result :object])
        r (get-in (ws/request!! root :act {:class :route :action :create :args {:transport transport :address nm :template (str "You are " nm ".")}}) [:result :object])]
    (fx/grant! root p #{p r} #{:read :write :transmit})
    (fx/grant! root p #{model} #{:evaluate})
    (fx/grant! root p #{witness-id} #{:read})
    (fx/grant! root witness-id #{r} #{:transmit})
    (is (:ok (ws/request!! root :set-field {:object p :field :principal/routes :type [:list [:ref :route]] :value [r]})))
    (is (:ok (ws/request!! root :set-field {:object p :field :principal/models :type [:list [:ref :model]] :value [model]})))
    {:id p :route r}))

(defn- replies-to
  "Message ids delivered to the witness in reply to the given Messages, as they arrive, within ms."
  [w sub wanted ms]
  (let [deadline (+ (System/currentTimeMillis) ms)]
    (loop [got #{}]
      (if (or (= (count got) (count wanted)) (> (System/currentTimeMillis) deadline))
        got
        (if-let [push (ws/await-push w sub (max 1 (- deadline (System/currentTimeMillis))))]
          (recur (into got (keep #(get-in % [:event/body :message]) (:events push))))
          got)))))

(deftest principals-evaluate-at-the-same-time
  (fx/with-world [{:keys [world root witness]}]
    (let [model (resolve-as root "root.model")
          w (fx/attach-as world (:token witness))
          a (process-principal! root (:id witness) "agents.a")
          b (process-principal! root (:id witness) "agents.b")
          ask (fn [ps] (let [t0 (System/currentTimeMillis)
                             sent (doall (for [{:keys [id route]} ps] (ws/request!! w :send {:to #{id} :route route :text "status?"})))
                             got (replies-to w "inbox" sent 30000)]
                         (doseq [s sent] (is (:ok s) (pr-str s)))
                         (is (= (count ps) (count got)) (str "every Principal answered: " (pr-str got)))
                         (- (System/currentTimeMillis) t0)))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      ;; the stub streams its reply in 24-char pieces 300 ms apart: a few seconds per Evaluation
      (is (:ok (ws/request!! root :set-field {:object model :field :model/stub-delta-ms :type :number :value 300 :reader :both})))
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (let [t-one (ask [a])
            t-both (ask [a b])]
        (println "concurrency: one principal" t-one "ms, two principals together" t-both "ms")
        (is (> t-one 500) "the paced stub takes a measurable time")
        (is (< t-both (* 1.5 t-one)) (str "two Principals answered together, not in sequence: " t-both " vs " t-one " ms"))
        (is (= "" (field root (:id a) :loop/stalled)))
        (is (= "" (field root (:id b) :loop/stalled))))
      (testing "one Principal's wakes run in order: the second Evaluation starts after the first is done"
        (let [head (:seq (ws/request!! root :read {:object (:id a)}))
              sent (doall (for [i (range 2)] (ws/request!! w :send {:to #{(:id a)} :route (:route a) :text (str "in order " i)})))
              got (replies-to w "inbox" sent 30000)
              evs (->> (get-in (ws/request!! root :history {:stream (:id a) :after head :limit 1000}) [:result :events])
                       (filter #(= :evaluated (:event/kind %))))
              by-state (fn [st] (map :event/seq (filter #(= st (get-in % [:event/body :state])) evs)))
              running (sort (by-state :running))
              done (sort (by-state :done))]
          (is (= 2 (count got)) (pr-str got))
          (is (= 2 (count running)) (pr-str running))
          (is (= 2 (count done)) (pr-str done))
          (is (< (first done) (second running)) (str "second running " (second running) " after first done " (first done)))))
      (ws/close! w))))

(deftest concurrency-is-a-field-on-the-world
  (fx/with-world [{:keys [world root]}]
    (let [world-id (:world-id world)]
      (is (= rloop/default-concurrency (rloop/concurrency world)))
      (is (:ok (ws/request!! root :set-field {:object world-id :field :loop/concurrency :type :number :value 2 :reader :both})))
      (is (= 2 (rloop/concurrency world)) "read from the world Principal's Field (takes effect at the next start)"))))
