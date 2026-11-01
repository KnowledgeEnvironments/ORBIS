(ns knowledge.world.adapters.shell
  "Host shell Adapter (spec 5.10 binds :host, 12.6, decisions 2026-10-30 'act run'). `run!` executes
   `/bin/zsh -lc cmd` in a working directory with a timeout and caps the captured output; nothing
   here appends to the log. knowledge.world.act.perform turns a run into a Task: created by the
   acting Principal, moved open -> active -> done (any exit code) or cancelled (timeout), with the
   command, exit code, stdout and stderr as Fields, so a run is a record anyone with a Grant can read."
  (:refer-clojure :exclude [run!])
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.adapters.confine :as confine])
  (:import [java.io ByteArrayOutputStream InputStream]
           [java.util.concurrent TimeUnit]))

(def default-timeout-s 120)
(def max-timeout-s 1800)
(def output-cap 65536)

(defn- drain
  "Read a stream to its end on a future, keeping the first `cap` bytes. Returns a future of
   {:bytes :total}."
  [^InputStream in cap]
  (future
    (let [kept (ByteArrayOutputStream.)
          buf (byte-array 8192)]
      (loop [total 0]
        (let [n (.read in buf)]
          (if (neg? n)
            {:bytes (.toByteArray kept) :total total}
            (do (when (< (.size kept) cap)
                  (.write kept buf 0 (min n (- cap (.size kept)))))
                (recur (+ total n)))))))))

(def ^:private failing-line
  #"(?i)\b(error|fatal|failed|failure|undefined|unresolved|exception|no such|not found|cannot|can't)\b|^\S+:\d+(:\d+)?: ")

(defn failing-lines
  "The lines of a run's output that say what went wrong, oldest first, at most 12 of 200 chars: the
   first errors are usually the cause, so they are kept before the last. nil when none matches."
  [& outputs]
  (let [ls (->> outputs (mapcat #(some-> % str str/split-lines)) (filter #(re-find failing-line %)) distinct)
        ;; a compiler's own line first: a toolchain's warnings came ahead of the error (seen live: xcodebuild)
        [compiler other] ((juxt filter remove) #(re-find #"^\S+:\d+(:\d+)?: " %) ls)]
    (when (seq ls)
      (str/join "\n" (map #(subs % 0 (min 200 (count %))) (take 12 (concat compiler other)))))))

(def ^:private box-refused
  "What a command says when the box stops it. Seen live: `ssh: connect to host github.com port 22:
   Operation not permitted`, reported to a person as macOS, then as the environment."
  #"(?i)operation not permitted|sandbox|could not resolve host|network is unreachable")

(defn box-note
  "When a command's output shows the box refused it, what that means, or nil. Whatever the exit: a
   pipe to `head` or an `|| true` hides the failure and not the words."
  [stderr stdout box]
  (when (and box (re-find box-refused (str stderr "\n" stdout)))
    (str "That refusal is ORBIS's box around every command, not macOS or the machine. Commands write only your mounts "
         "and cannot read the home directory or a private key; `git` and `ssh` work through an agent, so never read a "
         "key. Say so plainly and do not guess at another cause. What a person chose to close, or a path you need "
         "opened, is theirs to change with adapter.confine: ask, naming what you need.")))

(defn run!
  "{:cmd :cwd :timeout-s} -> {:exit :stdout :stderr :stdout-total :stderr-total :timed-out
   :duration-ms}. stdout and stderr are strings of at most output-cap bytes (UTF-8, lossy).
   box is a confine spec, or nil only when a person has lifted confinement for this Adapter."
  [{:keys [cmd cwd timeout-s box]}]
  (let [timeout (long (min max-timeout-s (max 1 (or timeout-s default-timeout-s))))
        argv ["/bin/zsh" "-lc" (str "set -o pipefail\n" cmd)] ; `make | tail` fails when make does
        pb (doto (ProcessBuilder. ^java.util.List (if box (confine/command box argv) argv))
             (.directory (io/file (or cwd ".")))
             (.redirectErrorStream false))
        env (.environment pb)
        _ (.clear env)
        _ (.putAll env (confine/environment box))
        started (System/currentTimeMillis)
        proc (.start pb)
        _ (.close (.getOutputStream proc))
        out (drain (.getInputStream proc) output-cap)
        err (drain (.getErrorStream proc) output-cap)
        finished (.waitFor proc timeout TimeUnit/SECONDS)
        _ (when-not finished (.destroyForcibly proc) (.waitFor proc 5 TimeUnit/SECONDS))
        o (deref out 5000 {:bytes (byte-array 0) :total 0})
        e (deref err 5000 {:bytes (byte-array 0) :total 0})]
    {:exit (if finished (.exitValue proc) -1)
     :timed-out (not finished)
     :stdout (String. ^bytes (:bytes o) "UTF-8") :stdout-total (:total o)
     :stderr (String. ^bytes (:bytes e) "UTF-8") :stderr-total (:total e)
     :duration-ms (- (System/currentTimeMillis) started)}))
