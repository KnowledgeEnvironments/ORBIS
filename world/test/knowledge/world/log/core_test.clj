(ns knowledge.world.log.core-test
  "Round trip over the log with events generated from the schema."
  (:require [clojure.test :refer [deftest is testing]]
            [clojure.walk :as walk]
            [knowledge.schema.registry :as r]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [malli.generator :as mg]))

(defn- comparable
  "Bytes to vectors (a blob receipt or bytes above the threshold to :blob), NaN to a marker,
   so generated events compare with their read-back."
  [v]
  (walk/postwalk (fn [x] (cond (blob/receipt? x) :blob
                               (and (bytes? x) (> (alength ^bytes x) blob/threshold)) :blob
                               (bytes? x) (vec x)
                               (and (double? x) (Double/isNaN x)) :nan
                               :else x))
                 v))

(defn- gen-events
  "n events generated from the PendingEvent schema."
  [n]
  (mg/sample (r/schema :ke/PendingEvent) {:size n :seed 42}))

(deftest a-subscribe-during-a-write-survives-it
  (let [l (log/open! (fx/temp-dir) {:fsync :never})
        a (random-uuid)
        seen (atom [])
        write @#'knowledge.world.log.writer/write-entries!
        event (fn [v] (log/pending :field-set a a {:name :x :type :string :value v :reader :both}))]
    (with-redefs-fn {#'knowledge.world.log.writer/write-entries!
                     (fn [st entries fsync]
                       (let [r (write st entries fsync)]
                         (log/subscribe! l :late (fn [batch] (swap! seen into (map :event/seq batch))))
                         r))}
      #(log/append!! l (event "one")))
    (log/append!! l (event "two"))
    (Thread/sleep 50)
    (is (some #{2} @seen) "the subscriber made while a batch was being written still hears the next")))

(deftest generated-events-round-trip
  (let [dir (fx/temp-dir)
        l (log/open! dir {:fsync :never})
        events (vec (take 60 (gen-events 60)))
        seqs (mapv #(log/append!! l %) events)]
    (is (= (range 1 61) seqs) "seqs are dense and global")
    (is (= 60 (log/head l)))
    (let [back (log/read-from l 1 1000)]
      (is (= 60 (count back)))
      (doseq [[ev got] (map vector events back)]
        (is (= (comparable (:event/body ev)) (comparable (:event/body got))))
        (is (= (:event/kind ev) (:event/kind got)))
        (is (= (:event/stream ev) (:event/stream got)))
        (is (uuid? (:event/id got)))))
    (testing "read-from windows"
      (is (= [10 11 12] (map :event/seq (log/read-from l 10 3))))
      (is (= [] (log/read-from l 61 10))))
    (testing "reopen sees everything"
      (log/close! l)
      (let [l2 (log/open! dir {:fsync :batch})]
        (is (= 60 (log/head l2)))
        (is (= (range 1 61) (map :event/seq (log/read-from l2 1 100))))
        (log/close! l2)))
    (fx/delete-tree! dir)))

(deftest streams-and-subscribers
  (let [dir (fx/temp-dir)
        l (log/open! dir {:fsync :never})
        a (random-uuid) b (random-uuid)
        seen (atom [])]
    (log/subscribe! l :t (fn [batch] (swap! seen into (map :event/seq batch))))
    (log/append!! l (log/pending :field-set a a {:name :x :type :string :value "1" :reader :both}))
    (log/append!! l (log/pending :field-set b a {:name :x :type :string :value "2" :reader :both}))
    (log/append!! l (log/pending :bound a a {:namespace a :name "n" :object b} :streams #{b}))
    (is (= [1 3] (map :event/seq (log/read-stream l a 0 10))))
    (is (= [2 3] (map :event/seq (log/read-stream l b 0 10))))
    (is (= [3] (map :event/seq (log/read-stream l b 2 10))))
    (Thread/sleep 50)
    (is (= [1 2 3] @seen) "subscribers see each committed batch")
    (log/unsubscribe! l :t)
    (testing "invalid pending events are rejected before the writer"
      (is (thrown? clojure.lang.ExceptionInfo (log/append! l {:event/kind :field-set}))))
    (testing "large bytes go to the blob store"
      (let [big (byte-array 5000 (byte 7))
            s (log/append!! l (log/pending :field-set a a {:name :blob :type :bytes :value big :reader :both}))
            body (:event/body (first (log/read-from l s 1)))]
        (is (blob/receipt? (:value body)))
        (is (= :blob (:mode body)))
        (is (= (seq big) (seq (blob/get (log/blobs l) (get-in body [:value :blob/sha256])))))))
    (log/close! l)
    (fx/delete-tree! dir)))

(deftest an-interrupted-reader-takes-nothing-with-it
  ;; Java closes a FileChannel when a thread is interrupted inside it. Stopping a world interrupts
  ;; lanes and the scheduler, and a reader caught there used to close the channel the writer used.
  (let [dir (fx/temp-dir)
        l (log/open! dir {:fsync :never})
        id (random-uuid)
        put! #(log/append!! l (log/pending :field-set id id {:name :notes/x :type :number :value % :reader :both}))]
    (try
      (put! 1)
      (put! 2)
      (let [read-interrupted (fn []
                               (let [out (promise)
                                     t (Thread. (fn []
                                                  (.interrupt (Thread/currentThread))
                                                  (deliver out {:events (count (log/read-from l 1 10))
                                                                :still-interrupted (.isInterrupted (Thread/currentThread))})))]
                                 (.start t)
                                 (.join t 5000)
                                 (deref out 100 :timeout)))]
        (testing "the reader finishes and keeps its interrupt for whoever waits on it"
          (is (= {:events 2 :still-interrupted true} (read-interrupted))))
        (testing "the writer and other readers carry on"
          (put! 3)
          (is (= 3 (count (log/read-from l 1 10))))
          (is (= {:events 3 :still-interrupted true} (read-interrupted)))
          (put! 4)
          (is (= 4 (count (log/read-from l 1 10))))))
      (finally (log/close! l)))
    (testing "a closed log refuses reads instead of reopening its files"
      (is (empty? (log/read-from l 1 10))))))
