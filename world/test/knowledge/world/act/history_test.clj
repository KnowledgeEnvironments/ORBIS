(ns knowledge.world.act.history-test
  "Global history and the snapshot read (sprint item W7): `history` without a stream
   pages the whole log through resolution; with `snapshot true` a client gets the resolvable state
   of every Object at one seq plus the tail after it, and folds from there instead of from 0."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.core :as fold]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.ws-client :as ws]))

(defn- create! [c class args] (get-in (ws/request!! c :act {:class class :action :create :args args}) [:result :object]))
(defn- shape [state] (select-keys state [:object/id :object/class :object/fields :object/names]))

(defn- all-events
  "Every event the client may read, from seq 0, paged."
  [c]
  (loop [after 0 acc []]
    (let [{:keys [result]} (ws/request!! c :history {:after after :limit 1000})
          acc (into acc (:events result))]
      (if-let [n (:next result)] (recur n acc) acc))))

(deftest a-narrow-principal-sees-only-its-subset
  (fx/with-world [{:keys [world root witness]}]
    (let [objects (vec (for [i (range 50)] (create! root :object {:fields {:n i}})))
          mine (set (take 10 objects))
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) mine #{:read})
      (let [r (ws/request!! w :history {:snapshot true})
            snap (get-in r [:result :snapshot])]
        (is (:ok r) (pr-str r))
        (is (= mine (set (map :object/id (:objects snap)))) "exactly the ten Objects its Grant covers")
        (is (= 10 (:total snap)))
        (is (nil? (get-in r [:result :next])) "one page: no object cursor")
        (is (int? (:seq snap)))
        (is (every? #(= 1 (count (:object/fields %))) (:objects snap)) "each with its Fields as read would give them")
        (testing "the tail after the snapshot seq holds only events on streams it resolves"
          (is (every? (fn [e] (or (mine (:event/stream e)) (some mine (:event/streams e)))) (get-in r [:result :events])))))
      (testing "one resolved event summarises the read, with the count and the seq, no object"
        (let [events (get-in (ws/request!! root :history {:stream (:id witness) :limit 1000}) [:result :events])
              summary (last (filter #(and (= :resolved (:event/kind %)) (= "*" (get-in % [:event/body :name]))) events))]
          (is (some? summary))
          (is (= 10 (get-in summary [:event/body :count])))
          (is (int? (get-in summary [:event/body :seq])))
          (is (nil? (get-in summary [:event/body :object])))))
      (ws/close! w))))

(deftest snapshot-plus-tail-equals-replay-from-zero
  (fx/with-world [{:keys [world root]}]
    (let [_ (dotimes [i 20] (create! root :object {:fields {:n i :name (str "o" i)}}))
          r (ws/request!! root :history {:snapshot true})
          snap (get-in r [:result :snapshot])
          s (:seq snap)
          ;; pages, taken before anything else is created, so they are of one state: the listener
          ;; passes the answer's top-level next back as next (cursor is the same thing by name)
          ids (sort (map (comp str :object/id) (:objects snap)))
          cursor (nth ids 4)
          page (get-in (ws/request!! root :history {:snapshot true :cursor cursor}) [:result :snapshot])
          first-page (:result (ws/request!! root :history {:snapshot true :limit 7}))
          second-page (:result (ws/request!! root :history {:snapshot true :limit 7 :next (:next first-page)}))
          _ (dotimes [i 5] (create! root :object {:fields {:later i}}))
          replayed (all-events root)
          up-to-s (filter #(<= (:event/seq %) s) replayed)
          db (fold/apply-events (fold/empty-db) up-to-s)
          folded (set (for [id (map :object/id (:objects snap))] (shape (objects/state db id))))]
      (is (:ok r) (pr-str r))
      (is (= (count (:objects snap)) (:total snap)) "root sees every Object, in one page")
      (is (= folded (set (map shape (:objects snap)))) "the snapshot is the fold of events 1..seq")
      (testing "the tail after the snapshot seq continues to the head with no gap"
        (let [tail (get-in (ws/request!! root :history {:after s :limit 1000}) [:result :events])
              seqs (map :event/seq tail)]
          (is (= (inc s) (first seqs)))
          (is (= seqs (range (inc s) (inc (last seqs)))))
          (is (= (map :event/seq (filter #(> (:event/seq %) s) replayed)) seqs))))
      (testing "objects page by cursor: the ids after the cursor, the same total"
        (is (= (drop 5 ids) (sort (map (comp str :object/id) (:objects page)))))
        (is (= (:total snap) (:total page))))
      (testing "limit is objects per page in snapshot mode and next continues the objects, as the listener boots"
        (is (= 7 (count (get-in first-page [:snapshot :objects]))))
        (is (string? (:next first-page)))
        (is (= (take 7 ids) (map (comp str :object/id) (get-in first-page [:snapshot :objects]))))
        (is (= (take 7 (drop 7 ids)) (map (comp str :object/id) (get-in second-page [:snapshot :objects]))))
        (is (= (get-in first-page [:snapshot :seq]) (get-in second-page [:snapshot :seq]))
            "every page of one boot is the fold at the first page's seq")
        (is (re-matches #"\d+:[0-9a-f-]{36}" (:next first-page)) "the cursor carries the pinned seq")))))

(deftest a-limit-above-the-maximum-is-clamped-not-refused
  ;; another track saw `history {after 0 limit 3000}` come back empty: the schema refused the limit
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          r (ws/request!! root :history {:stream root-id :limit 3000})
          g (ws/request!! root :history {:after 0 :limit 3000})]
      (is (:ok r) (pr-str r))
      (is (seq (get-in r [:result :events])))
      (is (<= (count (get-in r [:result :events])) 1000))
      (is (:ok g) (pr-str g))
      (is (seq (get-in g [:result :events])))
      (is (<= (count (get-in g [:result :events])) 1000)))))
