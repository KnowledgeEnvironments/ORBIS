(ns knowledge.world.log.fork-test
  "Forkable worlds at the log and tools level: a child reads its parent's events
   in place up to the fork seq and appends its own after it; the parent never sees the child; a
   parent event after the fork is invisible to the child; blobs fall back to the parent; export and
   import round-trip with hashes; promote appends to an unmoved parent and refuses a moved one."
  (:require [clojure.java.io :as io]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.core :as fold]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.tools :as tools]
            [knowledge.world.ws-client :as ws]))

(defn- ev [kind stream actor body] (log/pending kind stream actor body))

(defn- all-events [l]
  (loop [acc [] from 1]
    (let [batch (log/read-from l from 1000)]
      (if (empty? batch) acc (recur (into acc batch) (inc (:event/seq (peek batch))))))))

(defn- bare-world!
  "A log-only world directory with n field-set events on one stream (no server), closed."
  [dir n]
  (spit (io/file dir "world.edn") (pr-str {:world/id (random-uuid) :format-version 1}))
  (let [l (log/open! dir {:fsync :never})
        s (random-uuid) a (random-uuid)]
    (dotimes [i n]
      (log/append!! l (ev :field-set s a {:name (keyword "f" (str i)) :type :number :value i :reader :both})))
    (log/close! l)
    {:stream s :actor a}))

(deftest a-fork-reads-its-parent-and-writes-only-itself
  (let [pdir (fx/temp-dir) cdir (io/file (fx/temp-dir) "child")
        {:keys [stream actor]} (bare-world! pdir 10)]
    (try
      (let [{:keys [at]} (tools/fork! pdir cdir)
            _ (is (= 10 at) "forks at the parent's head by default")
            ;; the parent moves on after the fork
            pl (log/open! pdir {:fsync :never})
            _ (log/append!! pl (ev :field-set stream actor {:name :after-fork :type :number :value 99 :reader :both}))
            _ (log/close! pl)
            parent-ref {:data-dir (str pdir) :seq at}
            cl (log/open! cdir {:fsync :never :parent parent-ref})]
        (try
          (testing "the child sees the parent's events up to the fork seq and nothing after"
            (let [events (all-events cl)]
              (is (= 10 (count events)))
              (is (= (range 1 11) (map :event/seq events)))
              (is (not-any? #(= :after-fork (get-in % [:event/body :name])) events))
              (is (= 10 (log/head cl)))
              (is (= at (log/fork-seq cl)))))
          (testing "the child's own events begin at N+1 and are its own"
            (let [s (log/append!! cl (ev :field-set stream actor {:name :child-only :type :number :value 1 :reader :both}))]
              (is (= 11 s))
              (is (= [11] (map :event/seq (log/read-own-from cl 1 100))))
              (is (= 11 (count (all-events cl))))
              (is (= 11 (log/stream-head cl stream)) "stream-head walks both logs")))
          (finally (log/close! cl)))
        (testing "the parent is untouched: 11 events, none of them the child's"
          (let [pl (log/open! pdir {:fsync :never})]
            (try (let [events (all-events pl)]
                   (is (= 11 (count events)))
                   (is (not-any? #(= :child-only (get-in % [:event/body :name])) events)))
                 (finally (log/close! pl)))))
        (testing "a second open of the child continues where it left off"
          (let [cl (log/open! cdir {:fsync :never :parent parent-ref})]
            (try (is (= 11 (log/head cl)))
                 (finally (log/close! cl))))))
      (finally (fx/delete-tree! pdir) (fx/delete-tree! (.getParentFile cdir))))))

(deftest blobs-resolve-child-then-parent
  (let [pdir (fx/temp-dir) cdir (fx/temp-dir)]
    (try
      (let [pstore (blob/open pdir)
            {:keys [blob/sha256]} (blob/put! pstore (.getBytes (apply str (repeat 5000 "p")) "UTF-8"))
            cstore (blob/open cdir [pdir])
            {own :blob/sha256} (blob/put! cstore (.getBytes (apply str (repeat 5000 "c")) "UTF-8"))]
        (is (blob/exists? cstore sha256) "the parent's blob is visible through the child store")
        (is (= 5000 (alength (blob/get cstore sha256))))
        (is (.exists (io/file cdir "blobs" "sha256" (subs own 0 2) own)) "the child's own blob lives in the child")
        (is (not (blob/exists? pstore own)) "and never in the parent"))
      (finally (fx/delete-tree! pdir) (fx/delete-tree! cdir)))))

(deftest export-import-round-trip
  (let [pdir (fx/temp-dir) idir (io/file (fx/temp-dir) "imported") archive (io/file (fx/temp-dir) "w.tgz")]
    (try
      (bare-world! pdir 7)
      (spit (io/file pdir "root.token") "secret")
      (let [r (tools/export! pdir archive)]
        (is (= 7 (:head r)))
        (is (.exists archive))
        (let [m (tools/import! archive idir)]
          (is (= 7 (:head m)))
          (is (not (.exists (io/file idir "root.token"))) "tokens never travel")
          (is (not (.exists (io/file idir "manifest.edn"))))
          (is (= (tools/fold-at pdir) (tools/fold-at idir)) "the fold of the import equals the original's"))
        (testing "a second import into the same dir is refused"
          (is (thrown? clojure.lang.ExceptionInfo (tools/import! archive idir)))))
      (finally (fx/delete-tree! pdir) (fx/delete-tree! (.getParentFile idir)) (fx/delete-tree! (.getParentFile archive))))))

(deftest promote-appends-to-an-unmoved-parent-and-refuses-a-moved-one
  (let [pdir (fx/temp-dir) cdir (io/file (fx/temp-dir) "child") c2 (io/file (fx/temp-dir) "child2")
        {:keys [stream actor]} (bare-world! pdir 5)]
    (try
      (tools/fork! pdir cdir)
      (tools/fork! pdir c2)
      (let [cl (log/open! cdir {:fsync :never :parent {:data-dir (str pdir) :seq 5}})]
        (log/append!! cl (ev :field-set stream actor {:name :from-child :type :number :value 1 :reader :both}))
        (log/append!! cl (ev :field-set stream actor {:name :from-child-2 :type :number :value 2 :reader :both}))
        ;; what a fork's first boot writes about itself stays behind on promotion
        (log/append!! cl (ev :field-set stream actor {:name :principal/token-hash :type :string :value "child-secret" :reader :agent}))
        (log/append!! cl (ev :field-set stream actor {:name :world/forked-at :type :number :value 5 :reader :both}))
        (log/close! cl))
      (is (= {:events 4 :fields-set 4} (select-keys (tools/diff cdir) [:events :fields-set])))
      (let [r (tools/promote! cdir pdir)]
        (is (= 2 (:promoted r)) "the token hash and the fork mark did not travel")
        (is (= [6 7] [(:first-seq r) (:last-seq r)]))
        (is (= 7 (log/peek-head pdir)))
        (is (not (.contains (pr-str (tools/fold-at pdir)) "child-secret")) "the parent keeps its own credentials")
        (let [pl (log/open! pdir {:fsync :never})]
          (try (let [e (last (all-events pl))]
                 (is (string? (:event/from-fork e)))
                 (is (= 7 (:event/fork-seq e))))
               (finally (log/close! pl)))))
      (testing "the second fork cannot promote: the parent moved past its fork seq"
        (let [cl (log/open! c2 {:fsync :never :parent {:data-dir (str pdir) :seq 5}})]
          (log/append!! cl (ev :field-set stream actor {:name :late :type :number :value 3 :reader :both}))
          (log/close! cl))
        (is (thrown-with-msg? clojure.lang.ExceptionInfo #"parent moved" (tools/promote! c2 pdir))))
      (finally (fx/delete-tree! pdir) (fx/delete-tree! (.getParentFile cdir)) (fx/delete-tree! (.getParentFile c2))))))
