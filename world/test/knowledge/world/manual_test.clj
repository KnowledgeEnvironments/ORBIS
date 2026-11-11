(ns knowledge.world.manual-test
  "The manual: seeded from what ships, versioned in the log, read by every Principal, in no prompt."
  (:require [clojure.edn :as edn]
            [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.manual :as manual]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- result [r] (get-in r [:result :result]))

(deftest headings-split-outside-fences-only
  (is (= [nil "One" "Two"]
         (map :heading (manual/split-at-headings ["lead" "## One" "```" "## not a heading" "```" "## Two" "x"] 2)))))

(deftest every-action-is-a-topic
  (let [names (set (map :name (:topics @manual/shipped)))]
    (doseq [{:keys [name]} actions/definitions]
      (is (names (str "action/" name)) name))
    (is (names "class/principal"))
    (is (names "field/serialize/compress-at"))
    (is (names "field/principal/driven-by"))
    (is (names "authority"))
    (is (names "authority/what-root-cannot-do-either"))
    (is (some #(= "latex" (:format %)) (:topics @manual/shipped)))))

(deftest every-registered-page-loads
  (let [{:keys [books]} (edn/read-string (slurp (io/resource "knowledge/world/manual/sources.edn")))
        shipped (into {} (map (juxt :name identity)) (:topics @manual/shipped))
        pages (mapcat :concepts books)]
    (doseq [{:keys [name file]} pages]
      (is (seq (:body (shipped name))) (str name " is registered and has no body: " file)))
    (doseq [{:keys [book file]} (filter :file books)]
      (is (shipped book) (str book " names " file)))
    (testing "a page written and not registered reaches nobody"
      (let [dir (io/file (.getParentFile (io/file (io/resource "knowledge/world/manual/sources.edn"))) "concepts")
            registered (set (map :file pages))]
        (doseq [^java.io.File f (.listFiles dir)]
          (is (registered (str "concepts/" (.getName f))) (str (.getName f) " is in no book")))))))

(deftest a-book-may-have-both-shapes
  (let [ts (manual/book-topics {:book "both" :title "Both" :file "docs/ethos.md"
                                :concepts [{:name "both/page" :file "concepts/models.md"}]})]
    (is (some #(= "both/page" (:name %)) ts) "the page registered on a document's book loads")
    (is (some #(= "both" (:name %)) ts) "and so does the document")))

(deftest a-page-that-is-not-there-stops-the-manual
  (is (thrown-with-msg? clojure.lang.ExceptionInfo #"does not exist"
                        (doall (manual/book-topics {:book "x" :concepts [{:name "x/gone" :file "concepts/gone.md"}]}))))
  (is (thrown-with-msg? clojure.lang.ExceptionInfo #"does not exist"
                        (doall (manual/book-topics {:book "x" :title "X" :file "docs/gone.md"})))))

(deftest a-world-has-its-manual-on-the-log
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          coll (manual/collection db)
          ship (:topics @manual/shipped)]
      (is (uuid? coll))
      (is (= (count ship) (count (manual/topics-in db))))
      (testing "the topics are on the collection's stream"
        (is (seq (log/read-stream (:log world) coll 0 10))))
      (testing "a second boot writes nothing"
        (let [head (log/head (:log world))]
          (is (= {:created 0 :changed 0 :retired 0} (manual/ensure! world)))
          (is (= head (log/head (:log world))))))
      (testing "a topic that changes ships, one that is gone retires, and history stays"
        (with-redefs [manual/shipped (let [books (:books @manual/shipped)]
                                       (delay {:books books
                                               :topics (-> (vec (rest ship))
                                                           (update-in [0 :body] str " changed")
                                                           (update-in [0 :hash] str "x"))}))]
          (let [r (manual/ensure! world)]
            (is (= 1 (:changed r)))
            (is (= 1 (:retired r)))
            (is (= {:found false} (select-keys (result (act! root "doc.show" {:topic (:name (first ship))})) [:found])))
            (is (str/ends-with? (:body (result (act! root "doc.show" {:topic (:name (second ship))}))) " changed"))))))))

(deftest every-principal-reads-it
  (fx/with-world [{:keys [world root witness]}]
    (let [narrow (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "narrow"}}) [:result])
          c (fx/attach-as world (get-in narrow [:result :token]))]
      (testing "with no Grant on the manual"
        (let [r (result (act! c "doc.show" {:topic "authority"}))]
          (is (= "authority" (:topic r)))
          (is (= "markdown" (:format r)))
          (is (str/includes? (:body r) "resolves?"))
          (is (:next r))))
      (testing "by an action's bare name"
        (is (= "action/field.clear" (:topic (result (act! c "doc.show" {:topic "field.clear"}))))))
      (testing "candidates, best first"
        (let [{:keys [candidates total]} (result (act! c "doc.candidates" {:text "restart a world"}))]
          (is (pos? total))
          (is (some #(= "action/world.restart" (:topic %)) (take 3 candidates)) (pr-str (map :topic candidates)))))
      (testing "by type"
        (is (every? #(= "action" (:type %)) (:candidates (result (act! c "doc.candidates" {:text "token" :type "action"}))))))
      (testing "overview and contents"
        (is (some #(= "authority" (:book %)) (:books (result (act! c "doc.overview" {})))))
        (is (seq (:overview (result (act! c "doc.overview" {:topic "start/what-this-is"})))))
        (is (some #(= "authority/open" (:topic %)) (:topics (result (act! c "doc.contents" {:book "authority"}))))))
      (testing "where it lives is public"
        (is (:ok (ws/request!! c :read {:object (:world-id world) :field :world/manual}))))
      (testing "a person can use it too"
        (is (:ok (act! (fx/attach-as world (:token witness)) "doc.show" {:topic "ethos"}))))
      (testing "an unknown topic offers what is close"
        (let [r (result (act! c "doc.show" {:topic "restart the world please"}))]
          (is (false? (:found r)))
          (is (seq (:candidates r))))))))

(deftest no-manual-text-in-any-prompt
  (fx/with-world [{:keys [world root]}]
    (let [text (:acting (ser/render (act/db world) {:principal (:principal root) :working-set #{(:principal root)}}))]
      (is (str/includes? text "doc.candidates") "one pointer")
      (is (not (str/includes? text "Show Documentation"))))))

(deftest search-stays-on-the-project
  (fx/with-world [{:keys [root]}]
    (let [hits (get-in (ws/request!! root :search {:text "Document Examiner resolves delegable"}) [:result :hits])]
      (is (not-any? #(str/starts-with? (str (:field %)) ":topic/") hits)))))

(deftest a-path-completes-from-the-listing
  (fx/with-world [{:keys [world root dir]}]
    (let [project (doto (java.io.File. (str dir "/project/sub")) .mkdirs)
          _ (spit (str dir "/project/sub/alpha.md") "a")
          _ (spit (str dir "/project/sub/alps.md") "b")
          _ (spit (str dir "/project/.hidden") "c")
          _ project
          mount (get-in (ws/request!! root :act {:class :adapter :action :create
                                                 :args {:adapter "fs" :locator (str dir "/project") :binds :filesystem :name "proj"}})
                        [:result :object])
          complete (fn [path] (act! root "path.complete" {:mount (str mount) :path path}))
          head (log/head (:log world))]
      (is (uuid? mount))
      (is (= ["sub/"] (:entries (result (complete "")))))
      (is (= ["alpha.md" "alps.md"] (:entries (result (complete "sub/")))))
      (is (= ["alpha.md"] (:entries (result (complete "sub/alph")))))
      (is (= [".hidden"] (:entries (result (complete ".h")))) "a dot asks for the hidden")
      (testing "it does not leave the mount"
        (is (empty? (:entries (result (complete "../"))))))
      (testing "and a keystroke writes nothing but its own resolution"
        (is (empty? (filter #(= :created (:event/kind %)) (log/read-from (:log world) head 100))))))))
