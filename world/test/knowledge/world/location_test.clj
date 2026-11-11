(ns knowledge.world.location-test
  "A Principal knows where it is: the world line says which host it runs on and how to reach a path,
   a read of a local path says how, and a world cannot be linked to itself."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.handlers :as handlers]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- host [world] (get-in (objects/fields (act/db world) (:world-id world)) [:world/host :field/value]))

(deftest the-world-line-says-where-you-are
  (fx/with-world [{:keys [world root]}]
    (let [l (ser/world-line (act/db world) (:principal root))]
      (is (str/includes? l (str "on host " (host world) ", you run here")) l)
      (is (str/includes? l "local paths are reachable through shell.local (") l)
      (is (str/includes? l "confined to that directory") l))))

(deftest a-local-path-says-how
  (fx/with-world [{:keys [world root]}]
    (testing "an absolute path"
      (let [e (:err (ws/request!! root :read {:name "/Users/someone/SOUL.md"}))]
        (is (= :unresolved (:code e)))
        (is (str/includes? (:message e) "cat /Users/someone/SOUL.md") (:message e))
        (is (str/includes? (:message e) "shell.local") (:message e))))
    (testing "host/path, as a person might write it"
      (let [e (:err (ws/request!! root :read {:name (str (host world) "/you/old/SOUL.md")}))]
        (is (str/includes? (:message e) "cat /you/old/SOUL.md") (:message e))))
    (testing "an unknown name stays plainly unresolved"
      (is (= "does not resolve" (get-in (ws/request!! root :read {:name "nothing.here"}) [:err :message]))))))

(deftest a-world-is-not-another-world
  (fx/with-world [{:keys [world root]}]
    (let [link (fn [args] (ws/request!! root :act {:action :world.link :args args}))
          h (host world)]
      (is (= :conflict (get-in (link {:name h :url "ws://elsewhere:7777/ws" :key-env "X"}) [:err :code])) "by name")
      (is (= :conflict (get-in (ws/request!! root :act {:action :world.send :args {:world h :text "hi"}}) [:err :code])) "send to self")
      (is (handlers/this-world? (assoc world :config {:port 7777}) "other" (str "ws://" h ":7777/ws")) "by host and port")
      (is (not (handlers/this-world? (assoc world :config {:port 7777}) "other" (str "ws://" h ":7778/ws"))) "another port is another world"))))
