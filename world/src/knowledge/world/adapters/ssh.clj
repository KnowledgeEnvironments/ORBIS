(ns knowledge.world.adapters.ssh
  "SSH for a command in the box, with the key out of it. Principals use SSH all the time, for remotes
   and for other machines, and a box that can read ~/.ssh hands the private key to anything a model
   runs. So the world keeps an ssh-agent holding the machine's keys, and a command gets its socket:
   it can use a key and never read one. The world owns its agent and stops it at shutdown."
  (:require [clojure.java.io :as io]
            [clojure.string :as str])
  (:import [java.io File]
           [java.util.concurrent TimeUnit]))

(defn- run [env & argv]
  (try
    (let [pb (doto (ProcessBuilder. ^java.util.List (vec argv)) (.redirectErrorStream true))]
      (.putAll (.environment pb) env)
      (let [p (.start pb)
            out (future (slurp (.getInputStream p)))]
        (if (.waitFor p 10 TimeUnit/SECONDS)
          {:exit (.exitValue p) :out (deref out 1000 "")}
          (do (.destroyForcibly p) {:exit -1 :out "timed out"}))))
    (catch Exception e {:exit -1 :out (str (ex-message e))})))

(defn- dir-of ^File [data-dir] (io/file data-dir "ssh"))

(defn- agent-up? [sock]
  ;; 0 keys listed, 1 none loaded, 2 no agent
  (and (.exists (io/file sock)) (#{0 1} (:exit (run {"SSH_AUTH_SOCK" sock} "ssh-add" "-l")))))

(defn- parse-agent [line]
  ;; ps is not quoted argv: reject ambiguous whitespace rather than risk signalling
  ;; a process whose trailing arguments were mistaken for part of a socket path.
  (when-let [[_ pid sock]
             (re-matches #"\s*(\d+)\s+(?:/\S*/)?ssh-agent -a (\S+)\s*" line)]
    (try
      (let [pid (Long/parseLong pid)]
        (when (pos? pid) {:pid pid :sock sock}))
      (catch NumberFormatException _ nil))))

(defn- same-socket? [a b]
  (= (.getCanonicalPath (io/file a)) (.getCanonicalPath (io/file b))))

(defn- agents []
  ;; Limit discovery to this OS user and explicit socket arguments; never guess ownership.
  (let [{:keys [exit out]} (run {} "ps" "-ww" "-u" (System/getProperty "user.name")
                              "-o" "pid=" "-o" "args=")]
    (when-not (zero? exit)
      ;; Do not attach process output: it may contain unrelated private commands.
      (throw (ex-info "ssh-agent discovery failed" {:exit exit})))
    (keep parse-agent (str/split-lines out))))

(defn- stop-agent! [{:keys [pid sock] :as agent} missing-only?]
  ;; Recheck process identity and socket immediately before signalling.
  (when (and (some #{agent} (agents))
             (or (not missing-only?) (not (.exists (io/file sock)))))
    (when-let [p (.orElse (java.lang.ProcessHandle/of pid) nil)]
      (.destroy p)
      (try (.get (.onExit p) 2 TimeUnit/SECONDS) (catch Exception _ nil)))))

(defn reap!
  "Stop this user's explicit-socket agents only when their socket has disappeared."
  []
  (doseq [{:keys [sock] :as agent} (agents)
          :when (not (.exists (io/file sock)))]
    (stop-agent! agent true)))

(defn ready!
  "{:sock}, with an agent running and the machine's keys in it, or nil when this machine has no
   ssh-agent or no key: a box with no SSH is an ordinary box."
  [data-dir]
  (let [dir (doto (dir-of data-dir) .mkdirs)
        sock (.getPath (io/file dir "agent.sock"))]
    (when-not (agent-up? sock)
      (io/delete-file sock true)
      (run {} "ssh-agent" "-a" sock))
    (when (agent-up? sock)
      ;; each default key; a key with a passphrase stays out rather than prompting
      (run {"SSH_AUTH_SOCK" sock "SSH_ASKPASS_REQUIRE" "never"} "ssh-add" "-q")
      (when (zero? (:exit (run {"SSH_AUTH_SOCK" sock} "ssh-add" "-l")))
        {:sock sock}))))

(defonce ^:private known (atom {}))

(defn stop!
  "Stop the agent at this world's exact socket, including agents with no loaded keys."
  [data-dir]
  (let [sock (.getPath (io/file (dir-of data-dir) "agent.sock"))]
    (doseq [agent (agents) :when (same-socket? sock (:sock agent))]
      (stop-agent! agent false))
    (swap! known dissoc data-dir)))

(defn for-world
  "ready!, once for a data dir while its agent lives."
  [data-dir]
  (let [have (get @known data-dir)]
    (if (and have (agent-up? (:sock have)))
      have
      (let [r (ready! data-dir)]
        (swap! known assoc data-dir r)
        r))))
