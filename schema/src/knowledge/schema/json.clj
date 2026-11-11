(ns knowledge.schema.json
  "Export every named schema as JSON Schema for the C++ listener (make schema-json)."
  (:require [knowledge.schema.registry :as r]
            [malli.core :as m]
            [malli.json-schema :as js]
            [jsonista.core :as j]
            [clojure.java.io :as io]))

(def mapper (j/object-mapper {:pretty true}))

(defn export!
  [{:keys [out] :or {out "../listener/generated/schema"}}]
  (.mkdirs (io/file out))
  (doseq [[k v] (sort-by (comp str key) r/named)]
    (let [f (io/file out (str (namespace k) "." (name k) ".json"))
          schema (try (js/transform (m/schema v r/opts))
                      (catch Exception e {:error (str (.getMessage e)) :form (pr-str (m/form (m/schema v r/opts)))}))]
      (spit f (j/write-value-as-string schema mapper))))
  (spit (io/file out "index.json")
        (j/write-value-as-string {:schemas (mapv (fn [[k _]] (str (namespace k) "." (name k))) (sort-by (comp str key) r/named))
                                  :ops (mapv name knowledge.schema.protocol/ops)
                                  :event-kinds (mapv name knowledge.schema.events/kinds)}
                                 mapper))
  (println "exported" (count r/named) "schemas to" out))
