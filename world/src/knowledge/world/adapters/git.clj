(ns knowledge.world.adapters.git
  "The first clone of a project, which has no directory yet for a command to run in. Run by the world
   outside the box, into a directory that does not exist yet, so a repository chooses nothing: no
   hooks, no submodules. From then on a project's git is a command like any other."
  (:require [clojure.string :as str])
  (:import [java.io File]
           [java.util.concurrent TimeUnit]))

(def timeout-s 600)
(def ^:private output-cap 4000)
(def allowed-protocols "What a remote may be." ["ssh" "https"])

(def ^:private url-re
  #"^(?:[\w.-]+@[\w.-]+:[\w][\w./-]*|ssh://[\w@.-]+(?::\d+)?/[\w./-]+|https://[\w.-]+(?::\d+)?/[\w./-]+)$")

(defn url-ok?
  "A plain remote: nothing git would take for an option or a helper."
  [url]
  (and (string? url) (some? (re-matches url-re url))))

(defn name-of
  "The project's name in a URL: `git@github.com:me/skyHAB.git` is skyHAB."
  [url]
  (-> (str url) (str/replace #"/+$" "") (str/replace #"\.git$" "") (str/split #"[/:]") last))

(defn- locked-options
  "Each overrides what the repository's own config says."
  []
  (concat ["-c" "core.hooksPath=/dev/null" "-c" "core.fsmonitor=false" "-c" "core.pager=cat"
           "-c" "fetch.recurseSubmodules=false" "-c" "push.recurseSubmodules=no" "-c" "submodule.recurse=false"
           "-c" "gc.auto=0" "-c" "maintenance.auto=false" "-c" "credential.helper=" "-c" "protocol.allow=never"]
          (mapcat (fn [p] ["-c" (str "protocol." p ".allow=always")]) allowed-protocols)))

(defn- run
  "git in dir: {:ok output} or {:error output}."
  [^File dir args]
  (let [pb (doto (ProcessBuilder. ^java.util.List (vec (concat ["git"] (locked-options) args)))
             (.directory dir)
             (.redirectErrorStream true))
        env (.environment pb)]
    (doseq [k ["GIT_DIR" "GIT_WORK_TREE" "GIT_CONFIG_PARAMETERS"]] (.remove env k))
    (.put env "GIT_TERMINAL_PROMPT" "0")
    (.put env "GIT_CONFIG_NOSYSTEM" "1")
    (.put env "GIT_SSH_COMMAND" "ssh -o BatchMode=yes -o ConnectTimeout=20")
    (let [p (.start pb)
          out (future (slurp (.getInputStream p)))
          done (.waitFor p timeout-s TimeUnit/SECONDS)]
      (when-not done (.destroyForcibly p))
      (let [text (str/trim (deref out 2000 ""))
            text (if (> (count text) output-cap) (subs text (- (count text) output-cap)) text)]
        (cond (not done) {:error (str "git took over " timeout-s " s")}
              (zero? (.exitValue p)) {:ok text}
              :else {:error text})))))

(defn clone!
  "Clone url to dest, a directory that is not there yet, with no hooks of any kind."
  [url ^File dest]
  (.mkdirs (.getParentFile dest))
  (run (.getParentFile dest) ["clone" "--template=" "--no-recurse-submodules" "--" url (.getName dest)]))
