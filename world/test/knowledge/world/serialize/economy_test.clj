(ns knowledge.world.serialize.economy-test
  "Prompt economy (decisions 2026-10-30): serializer policy Fields with defaults, clipping of other
   Objects' Fields at 300 chars, the newest Messages and Evaluations verbatim and the older ones as
   one line each, a budget that drops what does not fit, and the count of it all in evaluation/context."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- create! [c class args] (get-in (ws/request!! c :act {:class class :action :create :args args}) [:result :object]))
(defn- f [state k] (get-in state [:object/fields k :field/value]))
(defn- ser-text
  "The system message of a Serialization, inline or from the blob store."
  [world c id]
  (let [s (:result (ws/request!! c :read {:object id}))
        bytes (ser/bytes-of world s)]
    (fx/full-ids (:content (first (j/read-value bytes j/keyword-keys-object-mapper))) (ser/ids-of world s))))
(defn- events [c stream] (get-in (ws/request!! c :history {:stream stream :limit 2000}) [:result :events]))

(deftest a-work-wake-past-its-first-turn-resends-less
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))
          _ (fx/grant! root (:id witness) #{(:id witness)} #{:read})
          _ (fx/grant! root (:id witness) #{route} #{:transmit})
          send! (fn [t] (get-in (ws/request!! w :send {:to #{root-id} :route route :text t}) [:result :message]))
          other (send! "an earlier approval text")
          msg (send! "the waking text")
          _ (ws/request!! root :set-field {:object root-id :field :loop/turn-limit :type :number :value 8 :reader :both})
          args {:principal root-id :working-set #{root-id msg other} :route route :message msg :wake-note "# Work so far"}
          full (ser/render (act/db world) args)
          lean (ser/render (act/db world) (assoc args :lean? true))]
      (is (< (count (:body lean)) (count (:body full))))
      (is (str/includes? (:body full) "an earlier approval text"))
      (is (not (str/includes? (:body lean) "an earlier approval text")) "the note holds what they said")
      (is (str/includes? (:acting full) "# Actions"))
      (is (not (str/includes? (:acting lean) "# Actions")))
      (is (str/includes? (:acting lean) "# Work so far") "the note stays")
      (is (not (str/includes? (:body lean) "loop/turn-limit")))
      (is (= "the waking text" (str/trim (str (:message-text lean)))) "the wake's own Message still reaches the model"))))

(deftest policy-comes-from-fields-with-defaults
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          db (act/db world)]
      (is (= ser/policy-defaults (ser/policy db root-id)))
      (is (= 2000 (:serialize/self-field-cap ser/policy-defaults)) "the Principal's own bulky Fields are clipped at 2000 chars by default")
      (ws/request!! root :set-field {:object root-id :field :serialize/budget-tokens :type :number :value 1200})
      (is (= 1200 (:serialize/budget-tokens (ser/policy (act/db world) root-id))))
      (is (= 4 (:serialize/messages-verbatim (ser/policy (act/db world) root-id))) "the others keep their defaults (4 since )"))))

(deftest other-objects-are-clipped-and-the-principal-is-not
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          long-text (apply str (repeat 1000 "y"))
          other (create! root :object {:fields {:blurb long-text :instructions/keep long-text :memory/keep long-text}})
          _ (ws/request!! root :set-field {:object root-id :field :notes/mine :type :string :value long-text})
          r (ws/request!! root :serialize {:working-set #{other root-id} :model model})
          text (ser-text world root (get-in r [:result :serialization]))]
      (is (:ok r) (pr-str r))
      (is (str/includes? text (str (apply str (repeat 300 "y")) " [... 700 more chars; read {\"object\": \"" other "\"} for all of blurb]")) "blurb of another Object clipped at 300, with the way to the rest")
      (is (= 3 (count (re-seq (re-pattern long-text) text))) "instructions/*, memory/* of the other and the Principal's own Field render whole")
      (testing "serialize/self-field-cap clips the Principal's own bulky Fields, never its instructions"
        (ws/request!! root :set-field {:object root-id :field :serialize/self-field-cap :type :number :value 100})
        (let [r2 (ws/request!! root :serialize {:working-set #{other root-id} :model model})
              t2 (ser-text world root (get-in r2 [:result :serialization]))]
          (is (str/includes? t2 (str (apply str (repeat 100 "y")) " [... 900 more chars; read {\"object\": \"" root-id "\"} for all of notes/mine]")))
          (is (= 2 (count (re-seq (re-pattern long-text) t2))) "the other's instructions/* and memory/* still whole")
          (is (str/includes? t2 "# Charter of the root Principal") "root's own instructions whole"))))))

(deftest older-messages-and-evaluations-become-lines-within-budget
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route")
          w (fx/attach-as world (:token witness))
          db #(act/db world)
          ;; root's replies only: a person's next line in a conversation also carries in-reply-to
          replies #(count (filter (fn [m] (= root-id (get-in (objects/fields (db) m) [:message/from :field/value])))
                                  (objects/objects-with-field (db) :message/in-reply-to)))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! root :set-field {:object root-id :field :serialize/messages-verbatim :type :number :value 2})
      (ws/request!! root :set-field {:object root-id :field :serialize/evaluations-verbatim :type :number :value 1})
      ;; four Messages on the Route; the loop (stub Model) answers each, one Evaluation per answer
      (let [sent (doall (for [i (range 4)]
                          (let [before (replies)
                                s (ws/request!! w :send {:to #{root-id} :route route :text (str "message number " i " please")})]
                            (is (:ok s) (pr-str s))
                            (is (until #(> (replies) before) 10000) "root answered")
                            (get-in s [:result :message]))))
            evals (vec (objects/objects-with-field (db) :evaluation/context))
            waking (last sent)
            ws-set (into #{root-id} (concat sent evals))
            r (ser/render (db) {:principal root-id :working-set ws-set :route route :message waking})
            text (:body r)]
        (is (= 4 (count sent)))
        (is (>= (count evals) 4) (pr-str evals))
        (testing "the waking Message and one more are verbatim, the two older are lines"
          (is (str/includes? text "message number 3 please"))
          (is (str/includes? text "message number 2 please"))
          (is (= 2 (count (re-seq #"(?m)^- message [0-9a-f-]+ from" text))) text)
          (is (= 2 (count (re-seq #"(?m)^## message " text)))))
        (testing "one Evaluation compact, the rest one line each"
          (is (= 1 (count (re-seq #"(?m)^## evaluation " text))))
          (is (= (dec (count evals)) (count (re-seq #"(?m)^- evaluation [0-9a-f-]+ iteration" text)))))
        (testing "dropped counts the summarised items"
          (is (= (+ 2 (dec (count evals))) (:dropped r))))
        (testing "a small budget drops everything but the Principal, the waking Message and this wake's Evaluation"
          ;; the Evaluations of the wake being answered are kept whatever the budget,
          ;; a loop blind to its own results would act again; here that is the one that answered message 3
          (ws/request!! root :set-field {:object root-id :field :serialize/budget-tokens :type :number :value 1})
          (let [r2 (ser/render (db) {:principal root-id :working-set ws-set :route route :message waking})
                t2 (:body r2)]
            (is (str/includes? t2 (str "## principal " root-id)))
            (is (str/includes? t2 "message number 3 please"))
            (is (not (str/includes? t2 "message number 2 please")))
            (is (= (+ (count sent) (count evals) -2) (:dropped r2)) "every other item is dropped")
            (is (= 3 (count (:objects r2))) "the Principal, the waking Message and its Evaluation rendered"))))
      (ws/close! w))))

(deftest dropped-reaches-evaluation-context
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          others (doall (for [i (range 5)] (create! root :object {:fields {:n i :pad (apply str (repeat 250 "p"))}})))]
      (ws/request!! root :set-field {:object root-id :field :serialize/budget-tokens :type :number :value 400})
      (let [r (ws/request!! root :evaluate {:working-set (into #{root-id} others) :model model :policy :one-shot})
            ev (get-in r [:result :evaluation])
            ctx (until #(get-in (ws/request!! root :read {:object ev}) [:result :object/fields :evaluation/context :field/value]) 10000)]
        (is (:ok r) (pr-str r))
        (is (pos? (:dropped ctx)) (pr-str ctx))
        (is (= (- 6 (:dropped ctx)) (:objects ctx)) "objects counts what rendered")))))

(deftest the-principals-own-bulky-fields-are-clipped-by-default
  ;; the prompt floor. A long Field of its own clips at 2000 chars with the way to
  ;; the rest; instructions/* and memory/* never clip; 0 turns the cap off.
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          big (apply str (repeat 3000 "i"))
          _ (ws/request!! root :set-field {:object root-id :field :notes/index :type :string :value big})
          _ (ws/request!! root :set-field {:object root-id :field :memory/today :type :string :value big})
          r (ws/request!! root :serialize {:working-set #{root-id} :model model})
          text (ser-text world root (get-in r [:result :serialization]))]
      (is (:ok r) (pr-str r))
      (is (str/includes? text (str (apply str (repeat 2000 "i")) " [... 1000 more chars; read {\"object\": \"" root-id "\"} for all of notes/index]")))
      (is (= 1 (count (re-seq (re-pattern big) text))) "memory/today whole, notes/index clipped")
      (is (str/includes? text "# Charter of the root Principal"))
      (ws/request!! root :set-field {:object root-id :field :serialize/self-field-cap :type :number :value 0})
      (let [t2 (ser-text world root (get-in (ws/request!! root :serialize {:working-set #{root-id} :model model}) [:result :serialization]))]
        (is (= 2 (count (re-seq (re-pattern big) t2))) "0 renders the Principal's own Fields whole")))))


(deftest long-standing-fields-are-pointed-at-not-rendered
  (fx/with-world [{:keys [world root]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          text (fn [] (ser-text world root (get-in (ws/request!! root :serialize {:working-set #{root-id} :model model})
                                                   [:result :serialization])))
          t (text)]
      (testing "named once, one read away"
        (is (not (str/includes? t "# The protocol, for agents")))
        (is (not (str/includes? t "# The harness, for root")))
        (is (str/includes? t "On demand"))
        (is (str/includes? t "instructions/protocol"))
        (is (str/includes? t "# Charter of the root Principal") "the charter stays")
        (is (str/includes? t "# Ethos of this world") "the ethos stays"))
      (testing "the running books are not in the prompt"
        (is (not (str/includes? t "principal/token-hash")))
        (is (not (str/includes? t "cost/tokens-in"))))
      (testing "a Principal's serialize/on-demand replaces the list"
        (ws/request!! root :set-field {:object root-id :field :serialize/on-demand :type [:list :string]
                                       :value ["instructions/ethos"]})
        (let [t2 (text)]
          (is (str/includes? t2 "# The protocol, for agents"))
          (is (not (str/includes? t2 "# Ethos of this world"))))))))

(deftest the-budget-covers-standing-fields
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          db (fn [] (act/db world))
          rendered (fn [] (ser/render (db) {:principal root-id :working-set #{root-id}}))
          r (rendered)]
      (testing "a prompt at rest says what it stands on and stays inside the budget"
        (is (pos? (:standing-tokens r)))
        (is (false? (:over-budget r))))
      (testing "standing Fields past the budget are recorded, not silently spent"
        (ws/request!! root :set-field {:object root-id :field :serialize/budget-tokens :type :number :value 200})
        (is (true? (:over-budget (rendered)))))
      (testing "serialize/standing-cap puts what the world wrote on demand, largest first"
        (ws/request!! root :set-field {:object root-id :field :serialize/standing-cap :type :number :value 300})
        (let [{:keys [body standing-tokens] :as r2} (rendered)]
          (is (< standing-tokens (:standing-tokens r)) (pr-str standing-tokens))
          (is (not (str/includes? body "# Charter of the root Principal")))
          (is (str/includes? body "instructions/charter"))))
      (testing "never what a person wrote"
        (log/append!! (:log world)
                      (log/pending :field-set root-id (:id witness)
                                   {:name :instructions/ethos :type :string :value "kept whole, a person wrote this" :reader :both}))
        (is (str/includes? (:body (rendered)) "kept whole, a person wrote this"))))))

(deftest the-sender-and-what-they-tagged-outlast-the-budget
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          witness-id (:id witness)
          route (resolve-as root "root.route")
          attachment (create! root :object {:fields {:note "the tagged thing"}})
          w (fx/attach-as world (:token witness))]
      (fx/grant! root witness-id #{route attachment} #{:read :reference :transmit})
      (let [sent (ws/request!! w :send {:to #{root-id} :route route :text "who am i" :tags [attachment]})
            message (get-in sent [:result :message])
            db (act/db world)
            items (ser/plan db root-id #{message witness-id root-id attachment} [] message (ser/policy db root-id))
            of (fn [o] (first (filter #(= o (:object %)) items)))]
        (is (:ok sent) (pr-str sent))
        (is (= {:mode :about :keep? true} (select-keys (of witness-id) [:mode :keep?])) "the sender stays, as what is said about them")
        (is (:keep? (of attachment)) "what the Message tags stays")
        (is (:keep? (of message)))))))

(deftest a-read-comes-back-as-something-a-model-can-use
  (let [render #'ser/render-evaluation
        padding (apply str (repeat 3000 "x"))
        result {:object/id "abc" :object/class :principal
                :object/fields {:principal/token-hash {:field/value padding}
                                :loop/stalled {:field/value padding}
                                :real_name {:field/value "Moth"}}}
        state {:object/fields {:evaluation/iteration {:field/value 1}
                               :evaluation/ops {:field/value [{:op "read" :args {:object "abc"}}]}
                               :evaluation/results {:field/value [{:op "read" :ok true :result result}]}}}
        lines (:lines (render nil "p" "e" state {:lines [] :reads [] :eval-cap 256}))
        text (str/join "\n" lines)]
    (is (str/includes? text "real_name: Moth") "a Field about the object survives the smallest cap")
    (is (not (str/includes? text padding)) "bookkeeping is named, not pasted")
    (is (str/includes? text "bookkeeping Fields"))))
