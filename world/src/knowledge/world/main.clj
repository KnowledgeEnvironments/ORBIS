(ns knowledge.world.main
  "clojure -M:run: start a world from the environment and block until the process ends."
  (:require [knowledge.world.system :as system])
  (:gen-class))

(defn -main [& _]
  (let [world (system/start (system/config-from-env))
        done (promise)]
    (.addShutdownHook (Runtime/getRuntime)
                      (Thread. (fn [] (system/stop world) (deliver done :stopped))))
    (println (str "world on ws://127.0.0.1:" (:port world) "/ws  data=" (:data-dir world)))
    @done))
