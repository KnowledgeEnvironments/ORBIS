(ns knowledge.world.serialize.facets-test
  "Field hints and comments (sprint item W10): <field>/hint renders after the value
   as a trailing comment with its author, <field>/comment as a comments block, object/hint and
   object/comment about the whole Object; reader scope holds; writing one needs :write like any Field."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- ser-text [world c id]
  (let [s (:result (ws/request!! c :read {:object id}))]
    (fx/full-ids (:content (first (j/read-value (ser/bytes-of world s) j/keyword-keys-object-mapper))) (ser/ids-of world s))))
(defn- put! [c o k type v & {:keys [reader]}]
  (ws/request!! c :set-field (cond-> {:object o :field k :type type :value v} reader (assoc :reader reader))))

(deftest hints-and-comments-render-with-what-they-are-about
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          model (resolve-as root "root.model")]
      (is (:ok (put! root root-id :notes/today :string "timeout is 600")))
      (is (:ok (put! root root-id :notes/today/hint :string "  seconds, not  ms ")))
      (is (:ok (put! root root-id :object/hint :string "the first resident")))
      (is (:ok (put! root root-id :notes/today/comment [:list [:map :string]]
                     [{:by (str root-id) :at "2026-10-30T19:00:00Z" :text "first"}
                      {:by (str root-id) :at "2026-10-30T19:01:00Z" :text "second"}
                      {:by (str root-id) :at "2026-10-30T19:02:00Z" :text "third"}
                      {:by (str root-id) :at "2026-10-30T19:03:00Z" :text "fourth, the newest"}])))
      (is (:ok (put! root root-id :object/comment [:list [:map :string]] [{:text "about the whole object"}])))
      (is (:ok (put! root root-id :secret/hint :string "humans only" :reader :human)))
      (is (:ok (put! root root-id :secret :string "s")))
      (let [r (ws/request!! root :serialize {:working-set #{root-id} :model model})
            text (ser-text world root (get-in r [:result :serialization]))
            lines (str/split-lines text)
            field-line (first (filter #(str/starts-with? % (str ser/field-marker " notes/today (")) lines))]
        (is (:ok r) (pr-str r))
        (is (some? field-line) (pr-str (take 40 lines)))
        (is (re-find #" // seconds, not ms \(by \S+" field-line) field-line) ; a tag is per wake: ids can collide
        (is (not-any? #(str/starts-with? % (str ser/field-marker " notes/today/hint")) lines) "a hint is not a Field of its own")
        (is (some #(and (str/starts-with? % "## principal") (str/includes? % "// the first resident")) lines) "object/hint on the header")
        (testing "the comments block: the newest three verbatim, the rest as a count, field-level ones named"
          (is (some #(= % (str ser/field-marker " comments (5)")) lines) (pr-str (filter #(str/includes? % "comments") lines)))
          (is (some #(= % "- 2 earlier comments") lines))
          (is (some #(and (str/starts-with? % "- on notes/today by ") (str/ends-with? % ": fourth, the newest")) lines))
          (is (some #(str/includes? % "at 2026-10-30T19:03:00Z") lines))
          (is (some #(and (str/starts-with? % "- by ") (str/ends-with? % ": about the whole object")) lines)
              "the object-level comment, newest by its Field's set time, with the Field's author")
          (let [block (->> lines (drop-while #(not= % (str ser/field-marker " comments (5)"))) rest (take-while seq))]
            (is (= 4 (count block)) (pr-str block))
            (is (not-any? #(str/ends-with? % ": first") block) "the oldest is counted, not shown")
            (is (not-any? #(str/ends-with? % ": second") block))))
        (testing "a reader :human hint is for people: not rendered to the model"
          (is (not (str/includes? text "humans only")))
          (is (some #(= % (str ser/field-marker " secret (:string)")) lines))))
      (testing "writing a hint needs :write on the Object like any Field"
        (let [w (fx/attach-as world (:token witness))
              r (put! w root-id :notes/today/hint :string "pwned")]
          (is (false? (:ok r)))
          (is (= :unresolved (get-in r [:err :code])))
          (ws/close! w))))))

(deftest facet-detection
  (is (= [:notes/today :hint] (ser/facet-of :notes/today/hint {:notes/today {}})))
  (is (= [:notes/today :comment] (ser/facet-of :notes/today/comment {})))
  (is (= [:object :hint] (ser/facet-of :object/hint {})))
  (is (= [:blurb :hint] (ser/facet-of :blurb/hint {:blurb {}})) "a hint on an un-namespaced Field present here")
  (is (nil? (ser/facet-of :blurb/hint {})) "x/hint with no Field x is an ordinary Field")
  (is (nil? (ser/facet-of :notes/today {}))))
