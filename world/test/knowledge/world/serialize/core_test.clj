(ns knowledge.world.serialize.core-test
  "The serializer (spec 8.1 to 8.3): headers, Fields by reader, tags, depth, as-of, field-reads,
   resolution filtering, the :openai-chat bytes, blobs above 4 KiB, deterministic hash."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- create! [c class args] (get-in (ws/request!! c :act {:class class :action :create :args args}) [:result :object]))
(defn- ser-state [c id] (:result (ws/request!! c :read {:object id})))
(defn- f [state k] (get-in state [:object/fields k :field/value]))
(def ^:private w (atom nil)) ; the world under test, for fetching Serialization bytes past the blob threshold
(defn- system-text [state] (fx/full-ids (:content (first (j/read-value (ser/bytes-of @w state) j/keyword-keys-object-mapper))) (ser/ids-of @w state)))
(defn- events [c stream] (get-in (ws/request!! c :history {:stream stream :limit 1000}) [:result :events]))

(deftest serialize-a-working-set
  (fx/with-world [{:keys [world root witness]}]
    (reset! w world)
    (let [root-id (:principal root)
          model (resolve-as root "root.model")
          other (create! root :object {:fields {:title "other" :note "a note"}})
          doc (create! root :object {:fields {:title "plan" :link other}})
          w (fx/attach-as world (:token witness))
          _ (fx/grant! root (:id witness) #{doc model} #{:read :write :evaluate})
          _ (ws/request!! root :set-field {:object doc :field :secret :type :string :value "for people" :reader :human})
          _ (ws/request!! root :set-field {:object doc :field :howto :type :string :value "for agents" :reader :agent})
          _ (ws/request!! w :set-field {:object doc :field :memo :type :string :value "a memory by the witness" :reader :both})
          _ (ws/request!! root :bind {:name "docs.plan" :object doc})
          r (ws/request!! root :serialize {:working-set #{other doc} :model model})
          id (get-in r [:result :serialization])
          s (ser-state root id)
          text (system-text s)]
      (testing "the result and the Serialization Object"
        (is (:ok r) (pr-str r))
        (is (= 64 (count (get-in r [:result :hash]))))
        (is (pos? (get-in r [:result :size :bytes])))
        (is (= :serialization (:object/class s)))
        (is (= :openai-chat (f s :serialization/format)))
        (is (= #{other doc} (f s :serialization/working-set)))
        (is (= model (f s :serialization/model)))
        (is (= (get-in r [:result :hash]) (f s :serialization/hash))))
      (testing "render rules"
        (is (str/includes? text (str "## object " other)) "header: class and id")
        (is (str/includes? text (str "## object " doc " names: docs.plan")) "names the principal knows it by")
        (is (str/includes? text "=== field title (:string)\nplan") "reader :both renders")
        (is (str/includes? text "=== field howto") "reader :agent renders")
        (is (not (str/includes? text "for people")) "reader :human is omitted")
        (is (str/includes? text (str "[[" other "||object]]")) "a reference is a tag")
        (is (str/includes? text (str "=== field memo (:string) by [[" (:id witness) "|test/witness|principal]] at 20")) "a Field by another author shows author and time")
        (is (not (str/includes? text "=== field title (:string) by")) "the owner's own Fields show no author"))
      (testing "as-of covers every stream rendered, and every rendered Field was read"
        (is (= #{other doc} (set (keys (f s :serialization/as-of)))))
        (is (every? pos? (vals (f s :serialization/as-of))))
        (is (some #(and (= :field-read (:event/kind %)) (some #{:title} (get-in % [:event/body :names])) (= root-id (:event/actor %)))
                  (events root doc))))
      (testing "depth 0 keeps the reference a tag; depth 1 renders the referenced object inline"
        (let [r0 (ws/request!! root :serialize {:working-set #{doc} :model model})
              t0 (system-text (ser-state root (get-in r0 [:result :serialization])))
              r1 (ws/request!! root :serialize {:working-set #{doc} :model model :depth {doc 1}})
              t1 (system-text (ser-state root (get-in r1 [:result :serialization])))]
          (is (not (str/includes? t0 "a note")) (pr-str t0))
          (is (str/includes? t1 "a note") (pr-str t1))
          (is (= {doc 1} (f (ser-state root (get-in r1 [:result :serialization])) :serialization/depth)))))
      (testing "same working set, same bytes, same hash"
        (let [r3 (ws/request!! root :serialize {:working-set #{other doc} :model model})]
          (is (= text (system-text (ser-state root (get-in r3 [:result :serialization])))))
          (is (= (get-in r [:result :hash]) (get-in r3 [:result :hash])))))
      (testing "what fails to resolve costs nothing"
        (let [rw (ws/request!! w :serialize {:working-set #{other doc} :model model})
              sw (ser-state root (get-in rw [:result :serialization]))
              tw (system-text sw)]
          (is (:ok rw) (pr-str rw))
          (is (str/includes? tw (str "## object " doc)))
          (is (not (str/includes? tw (str "## object " other))) "other does not resolve for the witness")
          (is (= #{doc} (set (keys (f sw :serialization/as-of)))))
          (is (not= (get-in r [:result :hash]) (get-in rw [:result :hash])) "two principals, two Serializations")))
      (ws/close! w)
      (testing "bytes above 4 KiB go to the blob store"
        ;; instructions/* renders whole; any other Field of another Object is clipped at 300 chars (economy)
        (let [big (create! root :object {:fields {:instructions/manual (apply str (repeat 6000 "x"))}})
              rb (ws/request!! root :serialize {:working-set #{big} :model model})
              sb (ser-state root (get-in rb [:result :serialization]))]
          (is (map? (f sb :serialization/bytes)))
          (is (= 64 (count (:blob/sha256 (f sb :serialization/bytes)))))
          (is (> (get-in rb [:result :size :bytes]) 4096)))))))

(deftest the-acting-section-states-the-principals-caps
  ;; 
  (is (clojure.string/includes? ser/acting "At most 16 ops per turn and 8 turns per Message"))
  (is (clojure.string/includes? (ser/acting-text 3 2) "At most 3 ops per turn and 2 turns per Message")))


(deftest op-results-read-as-text-not-clipped-json
  (let [lines #'ser/op-result-lines
        fit #'ser/fit-lines]
    (testing "a run: its exit, then its output"
      (let [t (str/join "\n" (lines {:exit 2 :duration-ms 1500 :stdout "one\ntwo" :stderr "boom" :task "t-1"}))]
        (is (str/starts-with? t "exit 2, 1.5 s, task t-1\none\ntwo\nstderr:\nboom"))))
    (testing "search hits, one line each, keys as they come back from a Field"
      (let [t (str/join "\n" (lines {"total" 3 "hits" [{"object" "o-1" "field" "notes/a" "seq" 9 "snippet" "the\nreal   name"}]}))]
        (is (= "3 hits\n- o-1 notes/a seq 9: the real name" t))))
    (testing "history, one line per event"
      (let [t (str/join "\n" (lines {:events [{:event/seq 4 :event/kind :field-set :event/actor "a-1" :event/body {:name :notes/x :value 1}}]}))]
        (is (str/starts-with? t "4 field-set by a-1: name notes/x, value 1"))))
    (testing "a list and a map with a list in it"
      (is (= ["- a 1, b two"] (vec (lines [{:a 1 :b "two"}]))))
      (is (= ["rows (1):" "- x 1" "count: 1"] (vec (lines {:rows [{:x 1}] :count 1})))))
    (testing "the cap cuts between lines and says what is left"
      (let [t (fit (map #(str "line " %) (range 100)) 40)]
        (is (str/ends-with? t "more lines]"))
        (is (not (re-find #"line \d+ \[" t)) "no line was cut")
        (is (not (str/includes? t "{")))))))
