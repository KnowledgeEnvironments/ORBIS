(ns knowledge.world.changes-test
  "A Principal hears what someone else changed, once, and can ask for the detail."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.changes :as changes]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.ws-client :as ws]))

(defn- put! [c obj k v] (ws/request!! c :set-field {:object obj :field k :type :string :value v :reader :both}))
(defn- nick [_] "someone")

(deftest line-diffs
  (is (= ["- b" "+ B" "+ c"] (changes/line-diff "a\nb" "a\nB\nc")))
  (is (= [] (changes/line-diff "a" "a")))
  (is (= ["+ a"] (changes/line-diff "" "a"))))

(deftest a-change-is-told-once
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root)
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{p} #{:read :write})
      (testing "a first wake sets the mark and says nothing"
        (is (nil? (changes/see! world p nick))))
      (is (:ok (put! w p :instructions/charter "new charter")))
      (is (:ok (put! root p :notes/kept "root's own")))
      (testing "someone else's edit shows as pending, its own does not"
        (is (= [:instructions/charter] (mapv :field (changes/pending (act/db world) p)))))
      (let [line (changes/see! world p nick)]
        (is (str/includes? line "instructions/charter"))
        (is (str/includes? line "by someone"))
        (is (str/includes? line "field.diff")))
      (testing "once"
        (is (empty? (changes/pending (act/db world) p)))
        (is (nil? (changes/see! world p nick))))
      (testing "a quiet wake writes no event"
        (let [head (knowledge.world.log.core/head (:log world))]
          (changes/see! world p nick)
          (is (= head (knowledge.world.log.core/head (:log world)))))))))

(deftest running-figures-are-not-news
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root)
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{p} #{:read :write})
      (changes/see! world p nick)
      (put! w p :cost/note "x")
      (is (empty? (changes/pending (act/db world) p))))))

(deftest field-diff
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root)
          act! (fn [c a] (ws/request!! c :act {:action :field.diff :args a}))]
      (put! root p :notes/x "one\ntwo")
      (put! root p :notes/x "one\nthree")
      (let [r (get-in (act! root {:object (str p) :field "notes/x"}) [:result :result])]
        (is (true? (:changed r)))
        (is (= ["- two" "+ three"] (:diff r)))
        (is (= p (:by r))))
      (testing "everything since a seq"
        (let [r (get-in (act! root {:object (str p) :field "notes/x" :since 1}) [:result :result])]
          (is (= 2 (:changes r)))
          (is (= ["+ one" "+ three"] (:diff r)))))
      (testing "it needs read on the Object"
        (is (= :unresolved (get-in (act! (fx/attach-as world (:token witness)) {:object (str p) :field "notes/x"})
                                   [:err :code]))))
      (testing "a Field never set says so"
        (is (false? (get-in (act! root {:object (str p) :field "notes/never"}) [:result :result :changed])))))))
