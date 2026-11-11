(ns knowledge.world.config
  "Configuration from the environment. No framework: a map.
   KE_PORT (7777), KE_DATA (../data relative to world/, or absolute), KE_FSYNC (batch|always|never),
   KE_FORMAT (transit|json, the default wire form), KE_MODEL_URL / KE_MODEL_KEY / KE_MODEL_ID for the
   model adapter (spec 17.1, 18.5 item 3), KE_LOG for the timbre level, KE_HARNESS_ROOT for the
   repository root root mounts as `harness` (else found by walking up from the working directory),
   KE_WORLD_NAME and KE_PUBLIC_URL for the world's name and the ws URL it tells clients (hello),
   KE_RETENTION_GB for the disk limit until a person sets one: a number, or infinite. KE_BIND is the
   address the server listens on: 127.0.0.1 unless set, 0.0.0.0 for clients on other machines."
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
   ;; root is the operator: outside the box and without routine cards, unless a person says otherwise
   :root-leash true
   ;; how this world introduces itself (hello): a name and the ws URL clients reach it at
   :world-name nil
   :public-url nil
   ;; blobs and log segments, in GiB, or "infinite"; retention.set overrides it. :retention takes
   ;; the sweep's other knobs, knowledge.world.retention/defaults
   :retention-gb nil})

(defn- env [k] (let [v (System/getenv k)] (when-not (str/blank? v) v)))

(defn absolute-data-dir
  "KE_DATA resolved against the working directory (world/ when run from the Makefile)."
  [data]
  (.getCanonicalFile (io/file data)))

(defn- retention-gb
  "KE_RETENTION_GB: a positive number or infinite. Anything else is the default, not no limit."
  [v]
  (when v
    (let [s (str/lower-case (str/trim v))]
      (if (= "infinite" s)
        s
        (let [n (parse-double s)] (when (and n (pos? n)) n))))))

(defn config-from-env
  "Read KE_* into a config map. Unknown KE_FSYNC values fall back to :batch."
  []
  (let [fsync (some-> (env "KE_FSYNC") str/lower-case keyword)
        format (some-> (env "KE_FORMAT") str/lower-case keyword)]
    (-> defaults
        (assoc :port (or (some-> (env "KE_PORT") parse-long) (:port defaults))
               :bind (or (not-empty (env "KE_BIND")) "127.0.0.1")
               :data (or (env "KE_DATA") (:data defaults))
               :fsync (if (#{:batch :always :never} fsync) fsync :batch)
               :format (if (#{:transit :json} format) format :transit)
               :log-level (or (some-> (env "KE_LOG") str/lower-case keyword) :info)
               :model-url (env "KE_MODEL_URL")
               :model-key (env "KE_MODEL_KEY")
               :model-id (env "KE_MODEL_ID")
               :harness-root (env "KE_HARNESS_ROOT")
               :root-leash (not (#{"0" "false" "off"} (some-> (env "KE_ROOT_LEASH") str/lower-case)))
               :release (env "KE_RELEASE")
               :work-root (env "KE_WORK")
               :world-name (env "KE_WORLD_NAME")
               :public-url (env "KE_PUBLIC_URL")
               :retention-gb (retention-gb (env "KE_RETENTION_GB"))))))
