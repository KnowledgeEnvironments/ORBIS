(ns knowledge.world.backup-test
  (:require [clojure.java.io :as io]
            [clojure.java.shell :as shell]
            [clojure.test :refer [deftest is]]
            [knowledge.world.fixtures :as fx]))

(deftest backup-default-destination-without-d
  (let [root (fx/temp-dir)
        data (doto (io/file root "example") .mkdirs)
        script (.getCanonicalPath (io/file ".." "scripts" "backup-world.sh"))
        env (dissoc (into {} (System/getenv)) "D" "KE_BACKUPS")]
    (try
      (spit (io/file data "world.edn") "{:world/name \"example\"}\n")
      (let [result (shell/sh "zsh" script (.getPath data) :env env)
            dest (io/file root "backups" "example")
            generations (filter #(.isDirectory %) (or (.listFiles dest) []))]
        (is (zero? (:exit result)) (str (:out result) (:err result)))
        (is (= 1 (count generations)))
        (when-let [generation (first generations)]
          (is (= (slurp (io/file data "world.edn"))
                 (slurp (io/file generation "world.edn")))))
        (is (zero? (:exit (shell/sh "zsh" script (.getPath data) :env env))))
        (is (= 1 (count (filter #(.isDirectory %) (or (.listFiles dest) []))))))
      (finally (fx/delete-tree! root)))))
