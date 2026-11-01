(ns knowledge.world.adapters.fs-test
  "Mounts (spec 6.5, decisions 2026-10-30): `<mount>/<path>` resolves under a Grant on the prefix,
   the file becomes an Object in the world namespace on first resolution, large files go to blobs
   and are paged with range, a changed file is refreshed on the next resolution, and a Principal
   without a Grant on the prefix gets nothing."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [r] (get-in r [:result :object/fields]))
(defn- v [r k] (get-in (fields r) [k :field/value]))

(deftest the-harness-is-mounted-for-root
  (fx/with-world [{:keys [world root witness]}]
    (testing "a small text file: resolved, materialized, read inline"
      (let [r (ws/request!! root :read {:name "harness/docs/ground-rules.md"})]
        (is (:ok r) (pr-str r))
        (is (= :object (get-in r [:result :object/class])))
        (is (= "docs/ground-rules.md" (v r :file/path)))
        (is (= :file (v r :file/kind)))
        (is (= "text/markdown" (v r :file/mime)))
        (is (= 64 (count (v r :file/sha256))))
        (is (str/starts-with? (v r :file/text) "# Ground rules"))
        (is (pos? (v r :file/lines)))
        (testing "the Object is bound under its full name in the world namespace: the second resolution finds it"
          (is (= (get-in r [:result :object/id]) (resolve-as root "harness/docs/ground-rules.md" :read))))))
    (testing "a file above the blob threshold arrives as a receipt and is paged with range"
      (let [whole (ws/request!! root :read {:name "harness/docs/protocol.md"})
            page (ws/request!! root :read {:name "harness/docs/protocol.md" :range [1 5]})]
        (is (:ok whole) (pr-str whole))
        (is (nil? (v whole :file/text)))
        (is (map? (v whole :file/bytes)) "a blob receipt")
        (is (> (v whole :file/size) fs/text-threshold))
        (is (:ok page) (pr-str page))
        (is (= 5 (count (str/split-lines (v page :file/text)))))
        (is (= [1 5] (v page :file/range)))
        (is (= (v whole :file/lines) (v page :file/lines)))
        (is (nil? (v page :file/bytes)))))
    (testing "a directory lists its entries"
      (let [r (ws/request!! root :read {:name "harness/docs"})]
        (is (= :directory (v r :file/kind)))
        (is (some #{"protocol.md"} (v r :file/entries)))))
    (testing "a missing path and an escaping path are nothing"
      (is (= :unresolved (get-in (ws/request!! root :read {:name "harness/docs/not-here.md"}) [:err :code])))
      (is (= :unresolved (get-in (ws/request!! root :read {:name "harness/../world"}) [:err :code]))))
    (testing "a Principal without a Grant on the prefix gets nothing; with one it reads"
      (let [w (fx/attach-as world (:token witness))]
        (is (= :unresolved (get-in (ws/request!! w :read {:name "harness/docs/ground-rules.md"}) [:err :code])))
        (is (nil? (resolve-as w "harness/docs/ethos.md" :read)))
        (fx/grant! root (:id witness) {:prefix "harness"} #{:read})
        (is (:ok (ws/request!! w :read {:name "harness/docs/ground-rules.md"})))
        (is (:ok (ws/request!! w :read {:name "harness/docs/ethos.md" :range [1 3]})) "materialized by the witness under its prefix Grant")
        (is (= :unresolved (get-in (ws/request!! w :set-field {:object (resolve-as w "harness/docs/ethos.md" :read) :field :x :type :string :value "y"}) [:err :code]))
            "read on the prefix is not write")
        (ws/close! w)))))

(deftest a-changed-file-is-refreshed-on-resolution
  (fx/with-world [{:keys [world root]}]
    (let [dir (fx/temp-dir)
          f (io/file dir "note.txt")
          _ (spit f "one\n")
          adapter (get-in (ws/request!! root :act {:class :adapter :action :create :args {:binds :filesystem :locator dir :sync :pull}}) [:result :object])
          _ (is (:ok (ws/request!! root :bind {:namespace (:world-id world) :name "scratch" :object adapter})))
          r1 (ws/request!! root :read {:name "scratch/note.txt"})
          id (get-in r1 [:result :object/id])]
      (is (= "one\n" (v r1 :file/text)))
      (Thread/sleep 1100) ; mtime resolution
      (spit f "one\ntwo\n")
      (let [r2 (ws/request!! root :read {:name "scratch/note.txt"})
            sets (filter #(and (= :field-set (:event/kind %)) (= :file/text (get-in % [:event/body :name])))
                         (get-in (ws/request!! root :history {:stream id}) [:result :events]))]
        (is (= id (get-in r2 [:result :object/id])) "same Object")
        (is (= "one\ntwo\n" (v r2 :file/text)))
        (is (not= (v r1 :file/sha256) (v r2 :file/sha256)))
        (is (= 1 (count sets)) "the change is a field-set on the file's stream")
        (is (= (:principal root) (:event/actor (first sets)))))
      (fx/delete-tree! dir))))

(deftest describe-and-range-are-pure
  (let [dir (fx/temp-dir)]
    (spit (io/file dir "a.md") (str/join "\n" (map #(str "line " %) (range 1 201))))
    (spit (io/file dir "big.txt") (apply str (repeat 5000 "x")))
    (.mkdirs (io/file dir "sub"))
    (let [a (fs/describe dir "a.md")
          big (fs/describe dir "big.txt")
          d (fs/describe dir ".")]
      (is (string? (:file/text a)))
      (is (= 200 (:file/lines a)))
      (is (bytes? (:file/bytes big)))
      (is (nil? (:file/text big)))
      (is (= :directory (:file/kind d)))
      (is (= ["a.md" "big.txt" "sub/"] (:file/entries d)))
      (is (nil? (fs/describe dir "../etc/passwd")))
      (is (nil? (fs/safe-file dir "../../x")))
      (let [{:keys [text from to of]} (fs/lines-range (:file/text a) [10 12])]
        (is (= "line 10\nline 11\nline 12" text))
        (is (= [10 12 200] [from to of])))
      (is (= "" (:text (fs/lines-range (:file/text a) [500 600])))))
    (fx/delete-tree! dir)))
