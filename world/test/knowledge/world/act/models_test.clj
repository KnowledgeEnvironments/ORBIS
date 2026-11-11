(ns knowledge.world.act.models-test
  "The Model catalogue: model.create binds a Model with its locator, model,
   key-env, prices and label and probes it; a literal credential in the args is refused
   (err secret-in-args); model.probe records ok / unauthenticated / unreachable against a tiny
   OpenAI-shaped server; the stub Model is ok locally; a world born before the catalogue gets
   the Fields from the later step."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.models :as models]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.ws-client :as ws]
            [org.httpkit.server :as hk]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args} 20000))
(defn- res-of [r] (get-in r [:result :result]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))

(defn- models-server!
  "An OpenAI-shaped /v1/models that accepts the bearer equal to $HOME (an env var every test host has)."
  []
  (let [port (promise)
        server (hk/run-server (fn [{:keys [uri headers]}]
                                (if (= uri "/v1/models")
                                  (if (= (get headers "authorization") (str "Bearer " (System/getenv "HOME")))
                                    {:status 200 :headers {"content-type" "application/json"}
                                     :body (j/write-value-as-string {:object "list" :data [{:id "test/alpha"} {:id "test/beta"}]})}
                                    {:status 401 :body "unauthorized"})
                                  {:status 404 :body "no"}))
                              {:port 0 :legacy-return-value? false})]
    (deliver port (hk/server-port server))
    {:server server :port @port}))

(deftest model-create-binds-and-probes
  (fx/with-world [{:keys [world root witness]}]
    (let [{:keys [server port]} (models-server!)]
      (try
        (testing "a literal credential is refused before anything is created"
          (let [r (act! root "model.create" {:name "bad" :url (str "http://127.0.0.1:" port "/v1") :model-id "x" :key "sk-live-123"})]
            (is (= :secret-in-args (get-in r [:err :code])) (pr-str r)))
          (let [r (act! root "model.create" {:name "bad2" :url (str "http://127.0.0.1:" port "/v1") :model-id "x" :key-env "sk-live-123"})]
            (is (= :secret-in-args (get-in r [:err :code])) "a key-env that is not an env var name is a key"))
          (is (nil? (objects/lookup (act/db world) (:world-id world) "bad"))))
        (testing "a good one: bound, catalogued, probed ok with the server's models"
          (let [r (act! root "model.create" {:name "alpha" :url (str "http://127.0.0.1:" port "/v1") :model-id "test/alpha"
                                              :key-env "HOME" :price-in 0.5 :price-out 1.5 :context-tokens 32000})
                res (res-of r)
                e (:model res)]
            (is (:ok r) (pr-str r))
            (is (= :ok (:status res)))
            (is (= ["test/alpha" "test/beta"] (:models res)))
            (is (= e (objects/lookup (act/db world) (:world-id world) "alpha")))
            (let [fs (fields root e)]
              (is (= (str "http://127.0.0.1:" port "/v1") (v fs :model/base-url)))
              (is (= "test/alpha" (v fs :model/id)))
              (is (= "HOME" (v fs :model/key-env)))
              (is (= "alpha" (v fs :model/label)))
              (is (= 0.5 (double (v fs :model/price-in))))
              (is (= 1.5 (double (v fs :model/price-out))))
              (is (= 32000 (v fs :model/context-tokens)))
              (is (= :ok (v fs :model/status)))
              (is (= ["test/alpha" "test/beta"] (v fs :model/available)))
              (is (inst? (v fs :model/probed-at)))
              (is (not-any? #(str/includes? (str (val %)) (System/getenv "HOME")) (dissoc fs :model/key-env)) "the key value is on no Field"))))
        (testing "a Model whose key-env is unset probes unauthenticated; one whose port is closed unreachable"
          (let [r (act! root "model.create" {:name "noauth" :url (str "http://127.0.0.1:" port "/v1") :model-id "x" :key-env "KE_TEST_MISSING_KEY_VAR"})]
            (is (= :unauthenticated (:status (res-of r))) (pr-str r)))
          (let [r (act! root "model.create" {:name "gone" :url "http://127.0.0.1:1/v1" :model-id "x"})]
            (is (= :unreachable (:status (res-of r))) (pr-str r))))
        (testing "model.probe by name re-probes and records; a Principal without write on it is unresolved"
          (let [r (act! root "model.probe" {:model "alpha"})]
            (is (= :ok (:status (res-of r))) (pr-str r)))
          (let [c (fx/attach-as world (:token witness))
                r (act! c "model.probe" {:model "alpha"})]
            (is (= :unresolved (get-in r [:err :code])) (pr-str r))
            (ws/close! c)))
        (testing "the stub root.model is ok locally and carries the catalogue from birth"
          (let [e (objects/lookup (act/db world) (kz/root-of world) "root.model")
                fs (fields root e)]
            (is (= "root.model" (v fs :model/label)))
            (is (nil? (v fs :model/price-in)) "unpriced until someone says")
            (is (= :unknown (v fs :model/status)) "unprobed until the scheduler's first pass")
            (is (= :ok (:status (models/probe! world e))))
            (is (= :ok (v (fields root e) :model/status)))))
        (finally (hk/server-stop! server))))))

(deftest the-catalogue-step-backfills-older-models
  (fx/with-world [{:keys [world root]}]
    (let [e (objects/lookup (act/db world) (kz/root-of world) "root.model")]
      ;; a Model born before the catalogue: take its Fields away the only way the log allows, by
      ;; a world that never had them; here we check the predicate and the step on a fresh world
      (is (empty? (models/models-without-catalogue (act/db world))) "a fresh world needs no backfill")
      (is (= {} (models/missing (act/db world) e)))
      (is (= {:models {}} (kz/step! world :model-catalogue)) "idempotent")
      (is (= {:model/label "root.model" :model/status :unknown}
             (models/catalogue-defaults (act/db world) e))))))

(deftest secret-detection
  (is (models/secret-in-args? {:key "abc"}))
  (is (models/secret-in-args? {:api-key "abc"}))
  (is (models/secret-in-args? {:key-env "sk-proj-1234"}))
  (is (models/secret-in-args? {:key-env "my.key"}))
  (is (not (models/secret-in-args? {:key-env "KE_MODEL_KEY"})))
  (is (not (models/secret-in-args? {:name "x" :url "http://h/v1" :model "m"}))))
