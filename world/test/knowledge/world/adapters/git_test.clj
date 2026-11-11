(ns knowledge.world.adapters.git-test
  "A project's remote: the world runs git outside the box, and nothing an agent wrote may choose what runs."
  (:require [clojure.java.io :as io]
            [clojure.java.shell :as sh]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.git :as git]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.adapters.shell :as shell]
            [knowledge.world.authority :as authority]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.projects]
            [knowledge.world.ws-client :as ws]))

(defn- temp-dir [] (doto (java.io.File/createTempFile "orbis-git" "") .delete .mkdirs))
(defn- git! [dir & args] (let [r (apply sh/sh "git" (concat args [:dir dir]))] (assert (zero? (:exit r)) (pr-str r)) (:out r)))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args} 30000))

(defn- bare-with-a-commit
  "A bare remote holding master."
  []
  (let [seed (temp-dir) bare (temp-dir)]
    (git! seed "init" "-q" "-b" "master")
    (spit (io/file seed "README.md") "# skyHAB\n\nA project.\n")
    (git! seed "add" "-A")
    (git! seed "-c" "user.name=t" "-c" "user.email=t@t" "commit" "-q" "-m" "one")
    (git! bare "init" "-q" "--bare" "-b" "master")
    (git! seed "push" "-q" (str "file://" bare) "master:master")
    {:seed seed :bare bare :url (str "file://" bare "/skyHAB.git")
     :plain (str "file://" bare)}))

(defmacro with-file-remotes [& body]
  `(with-redefs [git/allowed-protocols ["file"] git/url-ok? (fn [u#] (str/starts-with? (str u#) "file://"))] ~@body))

(deftest what-counts-as-a-remote
  (doseq [u ["git@github.com:KnowledgeEnvironments/skyHAB.git" "ssh://git@host:22/a/b.git" "https://github.com/a/b.git"]]
    (is (git/url-ok? u) u))
  (doseq [u ["ext::sh -c touch%20/tmp/x" "--upload-pack=evil" "file:///etc" "/tmp/x" "git@github.com:a/b;rm" "" nil
             "https://x/a b" "-oProxyCommand=x"]]
    (is (not (git/url-ok? u)) (pr-str u)))
  (is (= "skyHAB" (git/name-of "git@github.com:KnowledgeEnvironments/skyHAB.git")))
  (is (= "b" (git/name-of "https://github.com/a/b"))))

(deftest a-project-by-url
  (with-file-remotes
    (fx/with-world [{:keys [world root]}]
      (let [{:keys [seed plain]} (bare-with-a-commit)
            r (act! root "project.add" {:name "skyhab" :url plain})
            dir (io/file (:data-dir world) "projects" "skyhab")
            db (act/db world)]
        (is (:ok r) (pr-str r))
        (testing "it lives under the world, apart from the ORBIS source"
          (is (.exists (io/file dir "README.md")))
          (is (= (.getCanonicalPath dir) (get-in r [:result :result :path]))))
        (testing "a second project by the same name is refused before anything is cloned"
          (is (= :conflict (get-in (act! root "project.add" {:name "skyhab" :url plain}) [:err :code]))))))))

(deftest a-failed-project-leaves-no-clone-behind
  (with-file-remotes
    (fx/with-world [{:keys [world root]}]
      (let [{:keys [plain]} (bare-with-a-commit)]
        (with-redefs [knowledge.world.projects/head-of-readme (fn [& _] (throw (ex-info "nope" {})))]
          (is (not (:ok (act! root "project.add" {:name "half" :url plain})))))
        (is (not (.exists (io/file (:data-dir world) "projects" "half"))))
        (is (:ok (act! root "project.add" {:name "half" :url plain})) "and the next try works")))))

(deftest a-box-refusal-says-it-is-the-box
  (let [box {:root "/x"}]
    (is (str/includes? (shell/box-note "ssh: connect to host github.com port 22: Operation not permitted" "" box) "not macOS"))
    (is (str/includes? (shell/box-note "" "cat: x: Operation not permitted" box) "adapter.confine"))
    (is (nil? (shell/box-note "cc: error: nope" "" box)) "an ordinary failure is its own")
    (is (nil? (shell/box-note "Operation not permitted" "" nil)) "with no box it is not the box")))

(deftest root-holds-a-leash-a-person-can-end
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          p (:principal root)
          shell (objects/lookup db (:world-id world) "shell.local")
          put! (fn [o k v] (log/append!! (:log world) (log/pending :field-set o (:world-id world) {:name k :type :boolean :value v :reader :both})))]
      (testing "off, root's shell is boxed like any"
        (is (some? (:box (unsafe/box-for world shell p)))))
      (put! shell authority/leash-field true)
      (testing "on, for root alone: a Grant on its shell carries no way out"
        (is (nil? (:box (unsafe/box-for world shell p))))
        (is (some? (:box (unsafe/box-for world shell (random-uuid)))))
        (is (some? (:box (unsafe/box-for world shell)))))
      (testing "no agent writes it, root included"
        (is (contains? authority/kernel-fields authority/leash-field))
        (is (contains? authority/kernel-fields :principal/wide-leash)))
      (testing "what must be a person's stays a card on the leash"
        (put! p :principal/wide-leash true)
        (let [db (act/db world)]
          (is (nil? (gates/gated? db p :act {:action "project.add" :args {:name "x"}})))
          (is (some? (gates/gated? db p :act {:action "adapter.unconfine" :args {:adapter "x"}}))))))))
