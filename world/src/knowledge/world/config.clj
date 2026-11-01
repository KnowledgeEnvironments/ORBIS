(ns knowledge.world.config
  "Configuration from the environment. No framework: a map.
   KE_PORT (7777), KE_DATA (../data relative to world/, or absolute), KE_FSYNC (batch|always|never),
   KE_FORMAT (transit|json, the default wire form), KE_MODEL_URL / KE_MODEL_KEY / KE_MODEL_ID for the
   model adapter (spec 17.1, 18.5 item 3), KE_LOG for the timbre level, KE_HARNESS_ROOT for the
   repository root root mounts as `harness` (else found by walking up from the working directory),
   KE_WORLD_NAME and KE_PUBLIC_URL for the world's name and the ws URL it tells clients (hello)."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]))

(def defaults
  {:port 7777
   :data "../data"
   :fsync :batch
   :format :transit
   :log-level :info
   :model-url nil
   :model-key nil
   :model-id nil
   :harness-root nil
   ;; a supervised instance: the running release's commit, and its working copy
   :release nil
   :work-root nil
   ;; how this world introduces itself (hello): a name and the ws URL clients reach it at
   :world-name nil
   :public-url nil})

(defn- env [k] (let [v (System/getenv k)] (when-not (str/blank? v) v)))

(defn absolute-data-dir
  "KE_DATA resolved against the working directory (world/ when run from the Makefile)."
  [data]
  (.getCanonicalFile (io/file data)))

(defn config-from-env
  "Read KE_* into a config map. Unknown KE_FSYNC values fall back to :batch."
  []
  (let [fsync (some-> (env "KE_FSYNC") str/lower-case keyword)
        format (some-> (env "KE_FORMAT") str/lower-case keyword)]
    (-> defaults
        (assoc :port (or (some-> (env "KE_PORT") parse-long) (:port defaults))
               :data (or (env "KE_DATA") (:data defaults))
               :fsync (if (#{:batch :always :never} fsync) fsync :batch)
               :format (if (#{:transit :json} format) format :transit)
               :log-level (or (some-> (env "KE_LOG") str/lower-case keyword) :info)
               :model-url (env "KE_MODEL_URL")
               :model-key (env "KE_MODEL_KEY")
               :model-id (env "KE_MODEL_ID")
               :harness-root (env "KE_HARNESS_ROOT")
               :release (env "KE_RELEASE")
               :work-root (env "KE_WORK")
               :world-name (env "KE_WORLD_NAME")
               :public-url (env "KE_PUBLIC_URL")))))
