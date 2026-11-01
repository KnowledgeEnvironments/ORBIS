(ns knowledge.world.act.gates-defaults
  "The policy Fields of approval gates, apart from the gate itself so act.core (create!) can give
   them to every process Principal at birth without a dependency cycle (gates needs act.core)."
  (:require [clojure.set]))

(def default-policy ["task.run" "shell" "grant" "principal.create"]) ; "shell" too, since `act run` on a shell Adapter is the same command as task.run (world-dev ran four ungated)
(def default-ttl-s 86400)

(def defaults
  {:policy/approval default-policy
   :policy/approval-ttl-s default-ttl-s})

(def types {:policy/approval [:list :string] :policy/approval-ttl-s :number})

(defn missing
  "The policy defaults a Principal's Fields lack."
  [fields]
  (into {} (remove (fn [[k _]] (contains? fields k))) defaults))
