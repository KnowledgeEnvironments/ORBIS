(ns knowledge.world.loop.resume-test
  "A person never loses a message to a restart (decisions 2026-10-31). A wake running when the world
   stops, or when the process dies, comes back at the next boot; its unfinished Evaluation ends
   failed with reason restart. A copy of the data directory taken mid-wake is what a kill leaves."
  (:require [clojure.java.io :as io]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.core :as rloop]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- field [c id k] (get-in (ws/request!! c :read {:object id}) [:result :object/fields k :field/value]))
(defn- pace! [root model ms]
  (assert (:ok (ws/request!! root :set-field {:object model :field :model/stub-delta-ms :type :number :value ms :reader :both}))))
(defn- say! [c to route text]
  (let [r (ws/request!! c :send {:to #{to} :route route :text text})]
    (assert (:ok r) (pr-str r))
    (get-in r [:result :message])))
(defn- f [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- evaluations-of [world message]
  (filter #(= message (f world % :evaluation/message)) (objects/objects-with-field (act/db world) :evaluation/message)))
(defn- reply-to [world message p]
  (some #(when (and (= message (f world % :message/in-reply-to)) (= p (f world % :message/from))) %)
        (objects/objects-with-field (act/db world) :message/from)))
(defn- running? [world message]
  (some #(= :running (some-> (f world % :evaluation/state) name keyword)) (evaluations-of world message)))
(defn- copy-tree! [from to]
  (doseq [^java.io.File src (file-seq (io/file from)) :when (.isFile src)]
    (let [rel (subs (.getPath src) (inc (count (str from))))
          dst (io/file to rel)]
      (when-not (= "world.pid" (.getName src))
        (io/make-parents dst)
        (io/copy src dst)))))

(defn- wake-then [dir check]
  (let [world (fx/start-world dir 0)
        root (fx/attach-root world)]
    (try (check world root) (finally (ws/close! root) (system/stop world)))))

(deftest a-wake-running-at-a-stop-resumes
  (let [dir (fx/temp-dir)]
    (try
      (let [world (fx/start-world dir 0)
            root (fx/attach-root world)
            root-id (:principal root)
            route (resolve-as root "root.route")
            model (resolve-as root "root.model")
            _ (pace! root model 400)
            m (say! root root-id route "a long one")]
        (is (until #(running? world m) 8000))
        (ws/close! root)
        (system/stop world)
        (is (nil? (reply-to world m root-id)) "no answer before the stop")
        (wake-then dir
                   (fn [world2 root2]
                     (pace! root2 model 0)
                     (is (until #(reply-to world2 m root-id) 15000) "the Message was answered after the restart")
                     (let [orphan (first (filter #(= "restart" (:error (f world2 % :evaluation/outcome)))
                                                 (evaluations-of world2 m)))]
                       (is (some? orphan) "the Evaluation the stop interrupted ended with reason restart")
                       (is (= :failed (keyword (name (f world2 orphan :evaluation/state))))))
                     (is (= 1 (f world2 m :message/resumed)))
                     (is (until #(zero? (long (or (field root2 root-id :loop/iteration) 0))) 5000)))))
      (finally (fx/delete-tree! dir)))))

(deftest a-wake-running-at-a-crash-resumes
  (let [dir (fx/temp-dir) image (fx/temp-dir)]
    (try
      (let [world (fx/start-world dir 0)
            root (fx/attach-root world)
            root-id (:principal root)
            route (resolve-as root "root.route")
            model (resolve-as root "root.model")
            _ (pace! root model 400)
            m (say! root root-id route "a long one")]
        (is (until #(running? world m) 8000))
        (copy-tree! dir image) ; what kill -9 leaves: no stop ran
        (ws/close! root)
        (system/stop world)
        (wake-then image
                   (fn [world2 root2]
                     (pace! root2 model 0)
                     (is (until #(reply-to world2 m root-id) 15000) "answered after the crash")
                     (is (= 1 (f world2 m :message/resumed))))))
      (finally (fx/delete-tree! dir) (fx/delete-tree! image)))))

(deftest a-persons-message-the-lane-never-saw-resumes
  (let [dir (fx/temp-dir) image (fx/temp-dir)]
    (try
      (let [world (fx/start-world dir 0)
            root (fx/attach-root world)
            root-id (:principal root)
            person (fx/attach-as world (.trim ^String (slurp (io/file dir "person.token"))))
            route (resolve-as root "root.person")
            ;; the lanes are gone, the socket is not: delivered, and no wake ever starts
            _ (rloop/stop! (:loop world) world)
            m (say! person root-id route "are you there")]
        (is (empty? (evaluations-of world m)))
        (Thread/sleep 300)
        (copy-tree! dir image)
        (ws/close! person)
        (ws/close! root)
        (system/stop world)
        (wake-then image
                   (fn [world2 _]
                     (is (until #(reply-to world2 m root-id) 15000) "answered after the crash"))))
      (finally (fx/delete-tree! dir) (fx/delete-tree! image)))))

(deftest an-answered-wake-does-not-run-again
  (let [dir (fx/temp-dir)]
    (try
      (let [world (fx/start-world dir 0)
            root (fx/attach-root world)
            root-id (:principal root)
            route (resolve-as root "root.route")
            m (say! root root-id route "quick")]
        (is (until #(reply-to world m root-id) 10000))
        (is (until #(zero? (long (or (field root root-id :loop/iteration) 0))) 5000))
        (let [n (count (evaluations-of world m))]
          (ws/close! root)
          (system/stop world)
          (wake-then dir
                     (fn [world2 _]
                       (Thread/sleep 1000)
                       (is (= n (count (evaluations-of world2 m))) "nothing ran again")
                       (is (nil? (f world2 m :message/resumed)))))))
      (finally (fx/delete-tree! dir)))))

(deftest a-wake-that-keeps-dying-stops-coming-back
  (let [dir (fx/temp-dir)]
    (try
      (let [world (fx/start-world dir 0)
            root (fx/attach-root world)
            root-id (:principal root)
            route (resolve-as root "root.route")
            model (resolve-as root "root.model")
            _ (pace! root model 400)
            m (say! root root-id route "a long one")]
        (is (until #(running? world m) 8000))
        (ws/close! root)
        (system/stop world)
        (dotimes [_ 2]
          (let [w (fx/start-world dir 0)]
            (is (until #(running? w m) 8000))
            (system/stop w)))
        (let [w (fx/start-world dir 0)]
          (try
            (Thread/sleep 800)
            (is (= 2 (f w m :message/resumed)) "resumed twice, then left alone")
            (is (not (running? w m)))
            (finally (system/stop w)))))
      (finally (fx/delete-tree! dir)))))
