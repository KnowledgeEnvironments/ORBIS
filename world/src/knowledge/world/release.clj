(ns knowledge.world.release
  "Self-hosting (decisions 2026-10-31). Every instance carries the source it runs: a release is an exact
   tree of one commit with a manifest, release.edn, under <data>/releases/<commit>, read-only, and
   <data>/release names the one running. scripts/release.sh makes the first, release.make the rest. <data>/src is the working
   copy, where edits go; the running release never changes.

   scripts/supervise.sh is the world's parent. world.restart writes <data>/restart.request and exits with
   restart-code; the supervisor starts the named release, checks its health, and falls back to the
   last good one by itself. Back up, the world reads <data>/restart.outcome, tells the person and
   carries on: the wake that asked resumes through knowledge.world.loop.resume, so an agent survives its
   own restart. The world cannot restart itself, only ask, so without a supervisor the action refuses.

   The log format never moves backward: a release older than the log is refused here and again by the
   supervisor."
  (:require [clojure.edn :as edn]
            [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.adapters.confine :as confine]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.segment :as segment]
            [knowledge.world.notify :as notify]
            [taoensso.timbre :as timbre])
  (:import [java.io File]
           [java.nio.file Files StandardCopyOption]
           [java.time Instant]))

(def restart-code "The exit code that means: start me again." 75)
(def exit-delay-ms "Long enough for the op's result to land before the process goes." 2000)
(def release-field :world/release)
(def restart-field :world/restart)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn releases-dir ^File [data-dir] (io/file data-dir "releases"))

(defn manifest
  "The release.edn of a release directory, or nil."
  [dir]
  (let [f (io/file dir "release.edn")]
    (when (.isFile f) (try (edn/read-string (slurp f)) (catch Exception _ nil)))))

(defn running
  "The running release's manifest; nil for a world started from a checkout."
  [config]
  (when (:release config)
    (or (some-> (:harness-root config) manifest) {:commit (:release config)})))

(defn find-release
  "The one release under data-dir whose commit starts with wanted: its manifest, or nil."
  [data-dir wanted]
  (let [dirs (filter (fn [^File d] (str/starts-with? (.getName d) (str wanted)))
                     (seq (.listFiles (releases-dir data-dir))))]
    (when (and (seq (str wanted)) (= 1 (count dirs)))
      (manifest (first dirs)))))

(defn refusal
  "The err for this restart, or nil when it can go."
  [world {:keys [release]}]
  (let [config (:config world)
        wanted (not-empty (str release))
        target (when wanted (find-release (:data-dir world) wanted))]
    (cond
      (nil? (:release config))
      (act/err :unsupervised "this world was not started by scripts/supervise.sh: nothing would bring it back")
      (and wanted (nil? target))
      (act/err :unresolved (str "no single release " wanted " under releases/; release.make makes one"))
      (and target (< (long (or (:log-version target) 0)) (long segment/version)))
      (act/err :log-version (str "release " (:commit target) " reads log format " (:log-version target)
                                 " and this log is " segment/version ": the format never moves backward"))
      :else nil)))

(defn- boxed-git
  "git on the working copy, inside the box: an agent wrote that repository, and its config can name
   programs (a filter, a hook) that must not run as the world. out is a File for stdout, or nil to
   capture it. Returns {:exit :out}."
  [data-dir ^File src out args]
  (let [box (confine/spec {:root src :data-dir data-dir :adapter "release-" :network "none"})
        argv (into ["/usr/bin/git" "-C" (.getPath src)] args)
        pb (doto (ProcessBuilder. ^java.util.List (confine/command box argv)) (.redirectError (java.lang.ProcessBuilder$Redirect/DISCARD)))
        env (.environment pb)]
    (.clear env)
    (.putAll env (confine/environment box))
    (when out (.redirectOutput pb ^File out))
    (let [p (.start pb)
          text (when-not out (slurp (.getInputStream p)))]
      {:exit (.waitFor p) :out (str/trim (str text))})))

(defn- sh! [& argv]
  (let [p (-> (ProcessBuilder. ^java.util.List (vec argv)) (.redirectErrorStream true) .start)
        out (slurp (.getInputStream p))]
    (when-not (zero? (.waitFor p)) (throw (ex-info (str (first argv) " failed: " out) {})))))

(defn make!
  "A release of rev in the working copy, by the world: a confined command cannot write under releases/,
   and must not be able to, or it could change the running kernel with no card. The tree comes out of
   the box as a tar stream; nothing in it runs until a person's card moves the world onto it.
   Returns {:ok {:result {:commit :log-version}}} or {:err}."
  [world {:keys [rev]}]
  (let [data (:data-dir world)
        src (io/file data "src")
        rev (or (not-empty (str rev)) "HEAD")]
    (cond
      (not (.isDirectory (io/file src ".git"))) (act/err :unsupervised "this world has no working copy under src/")
      (not (re-matches #"[0-9A-Za-z][0-9A-Za-z._/-]*" rev)) (act/err :invalid-args "rev is a commit or a branch")
      (nil? (confine/primitive)) (act/err :unconfined "no sandbox on this host, so no release is made from the working copy")
      :else
      (let [{:keys [exit out]} (boxed-git data src nil ["rev-parse" "--verify" (str rev "^{commit}")])
            commit out]
        (cond
          (or (not (zero? exit)) (not (re-matches #"[0-9a-f]{40}" commit)))
          (act/err :unresolved (str "no commit " rev " in the working copy"))
          (.isDirectory (io/file (releases-dir data) commit))
          {:ok {:result {:commit commit :log-version (:log-version (manifest (io/file (releases-dir data) commit)))}}}
          :else
          (let [tmp (io/file (releases-dir data) (str ".tmp-" commit))
                tar (io/file (releases-dir data) (str ".tmp-" commit ".tar"))]
            (try
              (.mkdirs tmp)
              (when-not (zero? (:exit (boxed-git data src tar ["archive" "--format=tar" commit])))
                (throw (ex-info "git archive failed" {})))
              (sh! "tar" "-x" "-f" (.getPath tar) "-C" (.getPath tmp))
              (let [seg (io/file tmp "world/src/knowledge/world/log/segment.clj")
                    logv (or (some->> (when (.isFile seg) (slurp seg)) (re-find #"\(def version (\d+)\)") second parse-long) 0)
                    edn {:commit commit :version (subs commit 0 12) :log-version logv :built-at (str (Instant/now))}
                    dest (io/file (releases-dir data) commit)]
                (spit (io/file tmp "release.edn") (pr-str edn))
                (sh! "chmod" "-R" "a-w" (.getPath tmp))
                (sh! "chmod" "u+w" (.getPath tmp))
                (sh! "mv" (.getPath tmp) (.getPath dest))
                (sh! "chmod" "a-w" (.getPath dest))
                {:ok {:result {:commit commit :log-version logv}}})
              (catch Exception e
                (timbre/warn e "release failed")
                (act/err :invalid (ex-message e)))
              (finally
                (.delete tar)
                (when (.exists tmp) (sh! "chmod" "-R" "u+w" (.getPath tmp)) (sh! "rm" "-rf" (.getPath tmp)))))))))))

(defn- write-atomically! [^File f ^String text]
  (let [tmp (io/file (.getParentFile f) (str (.getName f) ".tmp"))]
    (spit tmp text)
    (Files/move (.toPath tmp) (.toPath f) (into-array java.nio.file.CopyOption [StandardCopyOption/ATOMIC_MOVE]))))

(defn- one-line [s] (str/replace (str s) #"[\r\n]+" " "))

(defn request!
  "Record the restart, leave the supervisor its request and exit. A resumed wake that asks again is
   answered, not restarted: the world it wanted is the one answering."
  [world p {:keys [release reason]} {:keys [via message]}]
  (let [db (act/db world)
        wid (:world-id world)
        from (:commit (running (:config world)))
        to (or (when (not-empty (str release)) (:commit (find-release (:data-dir world) release))) from)
        last-one (field db wid restart-field)]
    (if (and message (= (str message) (:message last-one)) (= to from))
      {:ok {:result {:already true :release from :note "this wake's restart already happened; this is the world it brought back"}}}
      (let [record (cond-> {:by (str p) :from from :to to :reason (one-line reason) :at (str (Instant/now))}
                     message (assoc :message (str message)))
            {:keys [ok err]} (act/set-field! world (gates/root-of db)
                                             {:object wid :field restart-field :type :map :value record :reader :both :via via})]
        (if err
          {:err err}
          (do (write-atomically! (io/file (:data-dir world) "restart.request")
                                 (str "release=" to "\nby=" p "\nreason=" (one-line reason) "\n"))
              ((or (:on-restart (:config world))
                   (fn [code] (future (Thread/sleep exit-delay-ms) (System/exit code))))
               restart-code)
              {:ok {:result {:restarting true :from from :to to} :seq (:seq ok)}}))))))

;; ---------------------------------------------------------------- back up

(defn- parse-outcome
  "key=value lines, as the supervisor writes them."
  [text]
  (into {} (for [l (str/split-lines text) :let [[k v] (str/split l #"=" 2)] :when (and k v)] [(keyword k) v])))

(defn- tell!
  "A status Message and a notification for the people who own this world."
  [world level text]
  (let [root (gates/root-of (act/db world))]
    (gates/tell-person! world root text)
    (notify/send! world root {:level level :text text})))

(defn- report-outcome!
  "What the supervisor left for the world that came up."
  [world]
  (let [f (io/file (:data-dir world) "restart.outcome")]
    (when (.isFile f)
      (let [{:keys [result from to reason]} (parse-outcome (slurp f))
            short (fn [c] (subs (str c) 0 (min 12 (count (str c)))))]
        (.delete f)
        (case result
          "rolled-back" (tell! world "urgent" (str "Release " (short from) " failed (" reason "). Back on " (short to) "."))
          "refused" (tell! world "urgent" (str "Release " (short from) " was refused (" reason "). Still on " (short to) "."))
          "restarted" (tell! world "info" (str "Restarted on " (short to) (when (seq reason) (str ": " reason))))
          nil)))))

(defn- repoint!
  "Adapters follow the running release: the harness mount, anything mounted inside an older release,
   and shell.local at the working copy."
  [world]
  (let [{:keys [config data-dir world-id]} world
        db (act/db world)
        root (gates/root-of db)
        here (fs/harness-root config)
        work (fs/work-root config)
        prefix (str (.getCanonicalPath (releases-dir data-dir)) "/")
        point! (fn [id v] (when (not= v (field db id :adapter/locator))
                          (act/set-field! world root {:object id :field :adapter/locator :type :string :value v :reader :both})))]
    (when-let [h (objects/lookup db world-id "harness")] (point! h here))
    (when-let [s (objects/lookup db world-id "shell.local")] (point! s work))
    (doseq [a (objects/objects-of-class db :adapter)
            :let [l (str (field db a :adapter/locator))]
            :when (and (str/starts-with? l prefix) (not (str/starts-with? l (str here "/"))) (not= l here))]
      (let [rest-of (second (str/split (subs l (count prefix)) #"/" 2))]
        (point! a (if rest-of (str work "/" rest-of) work))))))

(defn boot!
  "A supervised world says what it is running, points its mounts at it and reports the restart."
  [world]
  (when-let [m (running (:config world))]
    (let [db (act/db world)
          value (select-keys m [:commit :version :log-version])]
      (when (not= value (field db (:world-id world) release-field))
        (act/set-field! world (gates/root-of db) {:object (:world-id world) :field release-field :type :map :value value :reader :both}))
      (repoint! world)
      (try (report-outcome! world)
           (catch Exception e (timbre/warn "restart outcome not reported" {:reason (ex-message e)}))))))
