(ns knowledge.world.fixtures
  "Test worlds (ground rule 6: fixtures are Objects created by a named test Principal).
   `with-world` starts a fresh world on a temp dir with KE_FSYNC=never on a free port and
   creates the principal `test/witness` through `act create` as root. Grants are given per test
   through the grant op as root, like everyone else."
  (:require [clojure.java.io :as io]
            [knowledge.world.act.core :as act]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]
            [taoensso.timbre :as timbre])
  (:import [java.nio.file Files]
           [java.nio.file.attribute FileAttribute]))

(timbre/set-min-level! :warn)

(defn temp-dir [] (str (Files/createTempDirectory "ke-world" (make-array FileAttribute 0))))

(defn delete-tree! [dir]
  (doseq [f (reverse (file-seq (io/file dir)))] (.delete ^java.io.File f)))

(defn config [dir port]
  {:port port :data dir :fsync :never :format :transit :log-level :warn :root-leash false})

(defn start-world
  ([] (start-world (temp-dir) 0))
  ([dir port] (system/start (config dir port))))

(defn root-token [world] (slurp (io/file (:data-dir world) "root.token")))

(defn attach-root
  "A connected client attached as root. format :transit or :json."
  ([world] (attach-root world :transit))
  ([world format]
   (let [c (ws/connect (:port world) format)
         r (ws/request!! c :attach {:token (root-token world) :client-kind :test})]
     (when-not (:ok r) (throw (ex-info "root attach failed" {:response r})))
     (assoc c :session (get-in r [:result :session]) :principal (get-in r [:result :principal])))))

(defn create-witness!
  "test/witness, created by root through act. Returns {:id :token}."
  [root-client]
  (let [r (ws/request!! root-client :act {:class :principal :action :create :args {:driven-by :person :name "test/witness"}})]
    (when-not (:ok r) (throw (ex-info "witness create failed" {:response r})))
    {:id (get-in r [:result :object]) :token (get-in r [:result :result :token])}))

(defn full-ids
  "A prompt's text with its handles back as ids: what a test of content reads, so it says what it
   means whatever handle an id drew."
  [text table]
  (reduce (fn [t [h id]]
            (clojure.string/replace t (re-pattern (str "(?<![0-9a-zA-Z.-])" h "(?![0-9a-zA-Z-])")) id))
          text (sort-by (comp - count key) table)))

(defn grant!
  "root grants grantee scope ops. Returns the grant id."
  [root-client grantee scope ops & {:keys [delegable] :or {delegable false}}]
  (let [r (ws/request!! root-client :grant {:grantee grantee :scope scope :ops (set ops) :delegable delegable})]
    (when-not (:ok r) (throw (ex-info "grant failed" {:response r})))
    (get-in r [:result :grant])))

(defn policy!
  "A test's own setup of a policy/* Field, written as kernel bookkeeping: over the wire only a
   person's session may."
  [world root-client args]
  (act/set-field! world (:principal root-client) args))

(defn attach-as
  "A connected client attached with token, format :transit or :json."
  ([world token] (attach-as world token :transit))
  ([world token format]
   (let [c (ws/connect (:port world) format)
         r (ws/request!! c :attach {:token token :client-kind :test})]
     (when-not (:ok r) (throw (ex-info "attach failed" {:response r})))
     (assoc c :session (get-in r [:result :session]) :principal (get-in r [:result :principal])))))

(defn with-world*
  "Start a world, attach root, create test/witness, call (f ctx), stop and clean up.
   ctx = {:world :dir :root (client) :witness {:id :token}}."
  [f]
  (let [dir (temp-dir)
        world (start-world dir 0)]
    (try
      (let [root (attach-root world)
            witness (create-witness! root)]
        (try (f {:world world :dir dir :root root :witness witness})
             (finally (ws/close! root))))
      (finally
        (system/stop world)
        (delete-tree! dir)))))

(defmacro with-world
  "(with-world [ctx] body...)"
  [[sym] & body]
  `(with-world* (fn [~sym] ~@body)))
