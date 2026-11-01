(ns user
  "REPL entry: (go) starts a world on KE_PORT, (reset) reloads changed namespaces and restarts."
  (:require [clojure.tools.namespace.repl :as tn]
            [knowledge.world.system :as system]))

(tn/set-refresh-dirs "src" "dev" "test" "../schema/src")

(defonce ^:private the-world (atom nil))

(defn go []
  (reset! the-world (system/start (system/config-from-env)))
  :started)

(defn halt []
  (when-let [w @the-world] (system/stop w) (reset! the-world nil))
  :stopped)

(defn reset []
  (halt)
  (tn/refresh :after 'user/go))

(defn world [] @the-world)
