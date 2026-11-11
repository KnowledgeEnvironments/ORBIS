(ns knowledge.world.system-test
  "Kernel-zero objects on an empty data dir; idempotent on restart; root.token is 0600."
  (:require [clojure.java.io :as io]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.index.core :as index]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws])
  (:import [java.nio.file Files]
           [java.nio.file.attribute PosixFilePermission]))

(deftest kernel-zero-boot
  (let [dir (fx/temp-dir)
        w1 (fx/start-world dir 0)
        head (log/head (:log w1))
        head-at-stop (atom nil)
        db (index/db (:index w1))
        world-ns (:world-id w1)
        root (objects/lookup db world-ns "root")]
    (try
      (testing "the first objects"
        (is (pos? head))
        (is (= (:world-id w1) (objects/lookup db world-ns "world")))
        (is (= :process (:principal/driven-by (objects/state db root))))
        (is (some? (objects/lookup db world-ns "schema.Grant")))
        (is (some? (objects/lookup db world-ns "schema.args.attach")))
        (is (string? (get-in (objects/state db (objects/lookup db world-ns "schema.Event")) [:object/fields :instructions :field/value])))
        (is (= #{:instructions/charter :instructions/ethos :instructions/ground-rules :instructions/protocol :instructions/harness
                 :instructions/charter-template/developer :instructions/own}
               (set (filter #(= "instructions" (namespace %)) (keys (objects/fields db root)))))
            "the six shipped instruction Fields (adds the developer charter template) and the one root writes in kernel-zero step 2"))
      (testing "root.token"
        (let [f (io/file dir "root.token")]
          (is (.exists f))
          (is (= #{PosixFilePermission/OWNER_READ PosixFilePermission/OWNER_WRITE}
                 (set (Files/getPosixFilePermissions (.toPath f) (make-array java.nio.file.LinkOption 0)))))
          (let [c (fx/attach-root w1 :json)]
            (is (= root (:principal c)))
            (ws/close! c)
            (Thread/sleep 300)
            (reset! head-at-stop (log/head (:log w1))))))
      (finally (system/stop w1)))
    (testing "a second start on the same data dir adds nothing"
      (let [w2 (fx/start-world dir 0)]
        (try
          (is (= @head-at-stop (log/head (:log w2))) "no new boot events")
          (is (= root (objects/lookup (index/db (:index w2)) world-ns "root")))
          (finally (system/stop w2)))))
    (fx/delete-tree! dir)))
