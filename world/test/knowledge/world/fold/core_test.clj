(ns knowledge.world.fold.core-test
  "The fold is pure: a rebuild from the log equals the incrementally kept index."
  (:require [clojure.test :refer [deftest is testing]]
            [datascript.core :as d]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.core :as fold]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.index.core :as index]
            [knowledge.world.index.snapshot :as snap]
            [knowledge.world.log.core :as log]))

(deftest fold-per-kind
  (let [root (random-uuid) doc (random-uuid) g (random-uuid) grp (random-uuid) bob (random-uuid)
        at (java.util.Date.)
        ev (fn [kind stream body & {:keys [seq]}] {:event/kind kind :event/stream stream :event/actor root :event/at at :event/seq (or seq 1) :event/body body})
        db (-> (fold/empty-db)
               (fold/apply-event (ev :created root {:class :principal :fields {:principal/driven-by :process :principal/namespace root}} :seq 1))
               (fold/apply-event (ev :created doc {:class :object :fields {:title "a"}} :seq 2))
               (fold/apply-event (ev :bound root {:namespace root :name "doc" :object doc} :seq 3))
               (fold/apply-event (ev :field-set doc {:name :title :type :string :value "b" :reader :both} :seq 4))
               (fold/apply-event (ev :field-read doc {:name :title :by root} :seq 5))
               (fold/apply-event (ev :granted g {:grant g :grantee grp :ops #{:read} :scope #{doc} :delegable false} :seq 6))
               (fold/apply-event (ev :created grp {:class :group :fields {:group/members #{bob} :group/kind :team :principal/namespace grp :principal/driven-by :none}} :seq 7)))]
    (testing "state"
      (let [s (objects/state db doc)]
        (is (= :object (:object/class s)))
        (is (= #{[root "doc"]} (:object/names s)))
        (is (= "b" (get-in s [:object/fields :title :field/value])))
        (is (= 1 (get-in s [:object/fields :title :field/reads])))))
    (is (= doc (objects/lookup db root "doc")))
    (is (= [doc] (vec (objects/objects-with-field db :title))))
    (is (= #{bob} (:group/members (objects/state db grp))))
    (is (= #{doc} (:grant/scope (objects/state db g))))
    (testing "rebind moves the name, unbind removes it, revoke and retire mark"
      (let [db2 (-> db
                    (fold/apply-event (ev :bound root {:namespace root :name "doc" :object root} :seq 8))
                    (fold/apply-event (ev :revoked g {:grant g} :seq 9))
                    (fold/apply-event (ev :retired doc {} :seq 10)))]
        (is (= root (objects/lookup db2 root "doc")))
        (is (empty? (:object/names (objects/state db2 doc))))
        (is (:grant/revoked (objects/state db2 g)))
        (is (:object/retired (objects/state db2 doc)))
        (is (nil? (objects/lookup (fold/apply-event db2 (ev :unbound root {:namespace root :name "doc" :object root} :seq 11)) root "doc")))))))

(deftest rebuild-equals-incremental
  (fx/with-world [{:keys [world dir]}]
    (let [{:keys [log index world-id]} world
          live (index/db index)
          fresh (index/db (index/start! (index/create log dir world-id)))
          datoms (fn [db] (set (map (fn [[e a v]] [a v]) (filter (fn [[_ a]] (not= a :db/id)) (d/datoms db :eavt)))))]
      (is (= (count (d/datoms live :eavt)) (count (d/datoms fresh :eavt))))
      (is (= (datoms live) (datoms fresh)) "the index is a pure function of the log")
      (testing "snapshot round trip"
        (snap/write! (:dir index) (log/head log) live)
        (let [{s :seq db :db} (snap/load-newest (:dir index) (log/head log))]
          (is (= (log/head log) s))
          (is (= (datoms live) (datoms db))))))))
