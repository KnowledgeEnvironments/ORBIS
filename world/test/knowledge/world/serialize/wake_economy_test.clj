(ns knowledge.world.serialize.wake-economy-test
  "A long wake stays affordable: the results of earlier iterations in the same
   wake render compactly, one line per op with at most 300 chars of a read, so a setup-style wake
   of many reads does not re-render every file it opened on every later iteration. The stub plays
   the model: the Message carries one ops block per iteration, three reads each, fifteen reads in
   five acting iterations, then a sixth iteration that is the reply."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [c id k] (get-in (ws/request!! c :read {:object id}) [:result :object/fields k :field/value]))
(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))
(defn- read-line* [nm] (str "{\"op\": \"read\", \"args\": {\"name\": \"" nm "\"}}"))

(defn wake-sizes
  "Run a wake of n-blocks acting iterations of reads-per-block reads of ~5 KB objects as root.
   Returns [{:iteration :bytes :tokens-est} ...] in iteration order."
  [{:keys [world root witness]} n-blocks reads-per-block]
  (let [root-id (:principal root)
        route (resolve-as root "root.route")
        w (fx/attach-as world (:token witness))
        big (apply str (repeat 5000 "f"))
        files (doall (for [i (range (* n-blocks reads-per-block))]
                       (let [id (get-in (ws/request!! root :act {:class :object :action :create
                                                                 :args {:fields {:file/path (str "docs/file-" i ".md") :file/text big}}})
                                        [:result :object])
                             nm (str "docs/file-" i)]
                         ;; read by name, as a model reads a mounted file: the Message spells out no ids
                         (is (:ok (ws/request!! root :bind {:name nm :object id})))
                         nm)))
        blocks (map (fn [group] (apply ops-block (map read-line* group))) (partition reads-per-block files))]
    (fx/grant! root (:id witness) #{(:id witness)} #{:read})
    (fx/grant! root (:id witness) #{route} #{:transmit})
    (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
    (let [head (:seq (ws/request!! root :read {:object root-id}))
          sent (ws/request!! w :send {:to #{root-id} :route route :text (str "set up the project:\n" (str/join "\n" blocks))})
          reply (some-> (ws/await-push w "inbox" 90000) :events first :event/body :message)
          evals (->> (get-in (ws/request!! root :history {:stream root-id :after head :limit 1000}) [:result :events])
                     (filter #(= :evaluated (:event/kind %)))
                     (map #(get-in % [:event/body :evaluation]))
                     distinct)
          rows (vec (for [e evals]
                      (let [ctx (until #(field root e :evaluation/context) 10000)]
                        {:iteration (field root e :evaluation/iteration) :bytes (:serialization-bytes ctx)
                         :tokens-est (quot (long (or (:serialization-bytes ctx) 0)) 4)})))]
      (is (:ok sent) (pr-str sent))
      (is (some? reply) "root replied within 90 s")
      (ws/close! w)
      rows)))

(deftest a-long-wake-stays-under-eight-thousand-tokens-per-iteration
  (fx/with-world [ctx]
    (let [rows (wake-sizes ctx 5 3)]
      (println "wake economy, bytes per iteration:" (pr-str rows))
      (is (= 6 (count rows)) (pr-str rows))
      (testing "every iteration's Serialization is under 32 KiB (8k tokens at 4 chars per token)"
        (doseq [{:keys [iteration bytes]} rows]
          (is (< bytes 32768) (str "iteration " iteration ": " bytes " bytes"))))
      (testing "the prompt does not keep growing with the reads of earlier iterations"
        (let [b (mapv :bytes rows)]
          (is (< (- (nth b 5) (nth b 2)) 6000) (str "iterations 3 to 6 grew by " (- (nth b 5) (nth b 2)) " bytes: " (pr-str b))))))))
