(ns knowledge.world.adapters.ssh-test
  (:require [clojure.java.io :as io]
            [clojure.test :refer [deftest is]]
            [knowledge.world.adapters.ssh :as ssh]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.system :as system]))

(defn- start-empty-agent! [dir]
  (let [sock (.getPath (io/file (doto (io/file dir "ssh") .mkdirs) "agent.sock"))
        r (#'ssh/run {} "ssh-agent" "-a" sock)]
    (is (zero? (:exit r)) (:out r))
    (first (filter #(#'ssh/same-socket? sock (:sock %)) (#'ssh/agents)))))

(defn- alive? [pid]
  (and pid (some-> (java.lang.ProcessHandle/of pid) (.orElse nil) .isAlive)))

(deftest stopped-world-leaves-no-agent
  (let [dir (fx/temp-dir)
        world (fx/start-world dir 0)
        agent (or (first (filter #(#'ssh/same-socket? (.getPath (io/file dir "ssh" "agent.sock")) (:sock %))
                                 (#'ssh/agents)))
                  (start-empty-agent! dir))]
    (try
      (is (some? (:pid agent)))
      (is (alive? (:pid agent)))
      (system/stop world)
      (is (not (alive? (:pid agent))))
      (ssh/stop! dir)
      (is (not (alive? (:pid agent))))
      (finally (ssh/stop! dir) (fx/delete-tree! dir)))))

(deftest startup-reaps-missing-socket-not-live-agent
  (let [gone-dir (fx/temp-dir)
        live-dir (fx/temp-dir)
        gone (start-empty-agent! gone-dir)
        live (start-empty-agent! live-dir)]
    (try
      (is (some? (:pid gone)))
      (is (some? (:pid live)))
      (io/delete-file (:sock gone))
      (fx/with-world [_]
        (is (not (alive? (:pid gone))))
        (is (alive? (:pid live))))
      (finally
        (ssh/stop! gone-dir)
        (ssh/stop! live-dir)
        (fx/delete-tree! gone-dir)
        (fx/delete-tree! live-dir)))))

(deftest discovery-parser-is-conservative
  (doseq [exe ["ssh-agent" "/usr/bin/ssh-agent"]]
    (is (= {:pid 123 :sock "/tmp/example/agent.sock"}
           (#'ssh/parse-agent (str " 123 " exe " -a /tmp/example/agent.sock")))))
  (doseq [line ["123 other-ssh-agent -a /tmp/a"
                "123 ssh-agent -a /tmp/a -D"
                "123 ssh-agent -a /tmp/has spaces"
                "oops ssh-agent -a /tmp/a"
                "0 ssh-agent -a /tmp/a"
                "9999999999999999999999999 ssh-agent -a /tmp/a"]]
    (is (nil? (#'ssh/parse-agent line))))
  (let [dir (fx/temp-dir)]
    (try
      (is (#'ssh/same-socket? (str dir "/ssh/../agent.sock")
                              (str (.getCanonicalPath (io/file dir)) "/agent.sock")))
      (is (not (#'ssh/same-socket? (str dir "/a") (str dir "/b"))))
      (finally (fx/delete-tree! dir))))
  (with-redefs-fn {#'ssh/run (fn [& _] {:exit 1 :out "private process output"})}
    #(is (thrown-with-msg? clojure.lang.ExceptionInfo #"discovery failed"
                          (#'ssh/agents)))))
