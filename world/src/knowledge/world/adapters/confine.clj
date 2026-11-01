(ns knowledge.world.adapters.confine
  "The box around what agents run (docs/authority.md). A shell command and a terminal run inside the
   operating system's own sandbox, so the world's data, its credentials and a person's home are out of
   reach whatever the agent types: gates decide what is asked for, this decides what is possible.

   One interface, a primitive per platform: `sandbox-exec` on macOS, `bwrap` on Linux. No primitive, no
   shell: `command` refuses, and only a person can lift that for one Adapter, for a limited time
   (adapter.unconfine, act.unsafe). The profile allows by name and denies the rest: reading the
   toolchain and the Adapter's root, writing the root and a scratch dir under the data dir.

   adapter/network says what leaves: `model` (the default: loopback and the configured Models' ports),
   `none`, or `all`. Seatbelt filters by port, not by host, and bwrap only on or off, so `all` is how
   a LAN is opened, and on Linux `model` means the host's network."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.fold.objects :as objects])
  (:import [java.io File]
           [java.net URI]))

(def env-kept
  "What a confined command inherits. An allow-list: the world's environment holds the model key."
  ["PATH" "HOME" "USER" "LOGNAME" "SHELL" "LANG" "LC_ALL" "LC_CTYPE" "JAVA_HOME" "DEVELOPER_DIR" "SDKROOT"
   "CC" "CXX" "PKG_CONFIG_PATH"])

(def toolchain
  "Read by every confined command."
  ["/usr" "/bin" "/sbin" "/System" "/opt/homebrew" "/opt/local" "/private/etc" "/etc"
   "/private/var/db/timezone" "/private/var/select" "/var/select" "/Library/Developer" "/Library/Apple"
   "/Library/Java" "/Library/Preferences/com.apple.dt.Xcode.plist" "/private/var/db/xcode_select_link"
   "/var/db/xcode_select_link" "/Applications/Xcode.app" "/dev" "/lib" "/lib64" "/nix"])

(def home-toolchain
  "Under the home directory, read-only: a cache a command could write is one an unconfined JVM loads."
  [".m2" ".gitlibs" ".clojure" "opt" ".gitconfig"])

(def mach-services
  "macOS services a command may reach. Not listed, so not reachable: the pasteboard, the keychain's
   securityd, launchd's job submission, Launch Services and AppleEvents, each a way out of the box."
  ["com.apple.system.notification_center" "com.apple.system.opendirectoryd.libinfo" "com.apple.system.logger"
   "com.apple.logd" "com.apple.diagnosticd" "com.apple.SystemConfiguration.configd" "com.apple.dnssd.service"
   "com.apple.trustd" "com.apple.trustd.agent" "com.apple.FSEvents" "com.apple.cfprefsd.daemon"
   "com.apple.cfprefsd.agent" "com.apple.dt.xcodeselect"])

(def secret-names
  "A file by these names is read by no command and no mount, wherever it stands."
  [#"\.token$" #"/world\.env$" #"/\.envrc$" #"/\.m2/settings(-security)?\.xml$"])

(def networks #{"model" "none" "all"})

(defn secret-path?
  "Is this a path no agent reads, by name?"
  [path]
  (let [p (str path)] (boolean (some #(re-find % p) secret-names))))

(defn- real ^String [path]
  ;; the profile sees real paths: /tmp is /private/tmp here, and a symlinked root is its target
  (.getCanonicalPath (io/file (str path))))

(defn- home [] (real (System/getProperty "user.home")))

(defn primitive
  "The sandbox this host has, or nil."
  []
  (let [os (str/lower-case (System/getProperty "os.name"))]
    (cond
      (and (str/includes? os "mac") (.canExecute (io/file "/usr/bin/sandbox-exec"))) :seatbelt
      (and (str/includes? os "linux")
           (some #(.canExecute (io/file % "bwrap")) (str/split (or (System/getenv "PATH") "") #":"))) :bwrap
      :else nil)))

(defn- under? [^String path ^String dir] (or (= path dir) (str/starts-with? path (str dir "/"))))

(defn too-wide?
  "A root that would hold the home directory or the whole disk is no box at all."
  [root]
  (let [r (real root)] (or (= r "/") (under? (home) r))))

(defn model-ports
  "The port of every Model with a URL, for adapter/network model."
  [db]
  (->> (objects/objects-of-class db :model)
       (keep #(get-in (objects/fields db %) [:model/base-url :field/value]))
       (keep (fn [u] (try (let [uri (URI. (str u))
                                port (.getPort uri)]
                            (cond (pos? port) port
                                  (= "https" (.getScheme uri)) 443
                                  (= "http" (.getScheme uri)) 80))
                          (catch Exception _ nil))))
       distinct sort vec))

(defn- git-dirs
  "A git worktree's root holds a `.git` file naming a directory elsewhere: read it too, or git cannot
   run there. Nothing is written back through it; a commit happens in a clone."
  [root]
  (let [f (io/file root ".git")]
    (when (.isFile f)
      (when-let [[_ dir] (re-find #"(?m)^gitdir:\s*(.+)$" (slurp f))]
        (let [d (io/file (if (.isAbsolute (io/file dir)) dir (str root "/" dir)))
              common (io/file d "commondir")]
          (cond-> [(real d)]
            (.isFile common) (conj (real (io/file d (str/trim (slurp common)))))))))))

(defn spec
  "What one Adapter's commands may touch. root is its locator; scratch is made here, under the data
   dir, and is the command's TMPDIR."
  [{:keys [root data-dir adapter read data-read network ports]}]
  (let [data (real data-dir)
        scratch (io/file data "scratch" (subs (str adapter) 0 8))]
    (.mkdirs scratch)
    {:root (real root) :data-dir data :scratch (real scratch)
     :read (into (mapv real read) (git-dirs (real root))) :data-read (mapv real data-read)
     :network (or network "model") :ports (vec ports)}))

(defn- q [s] (str "\"" (-> (str s) (str/replace "\\" "\\\\") (str/replace "\"" "\\\"")) "\""))
(defn- subpaths [paths] (str/join " " (map #(str "(subpath " (q %) ")") paths)))
(defn- rw [paths] (str "(allow file-read* file-write* " (subpaths paths) ")"))

(defn- network-rules [{:keys [network ports]}]
  (let [loopback ["(allow network-bind (local ip \"localhost:*\"))"
                  "(allow network-inbound (local ip \"localhost:*\"))"
                  "(allow network-outbound (remote ip \"localhost:*\"))"]
        dns "(allow network-outbound (literal \"/private/var/run/mDNSResponder\"))"]
    (case (str network)
      "none" []
      "all" (conj loopback dns "(allow network-outbound (remote ip \"*:*\"))")
      (concat loopback [dns] (map #(str "(allow network-outbound (remote ip " (q (str "*:" %)) "))") ports)))))

(defn seatbelt-profile
  "The SBPL profile. Order matters: the last rule to match wins, so the denials of the data dir and of
   secret names come after the allowances, and a root inside the data dir is allowed again between them."
  [{:keys [root scratch data-dir read data-read] :as s}]
  (let [h (home)
        reads (concat toolchain (map #(str h "/" %) home-toolchain) read)]
    (str/join
     "\n"
     (concat
      ["(version 1)" "(deny default)"
       "(allow process-fork)" "(allow process-exec*)" "(allow process-info* (target same-sandbox))"
       "(allow signal (target same-sandbox))" "(allow sysctl-read)" "(allow file-read-metadata)"
       "(allow file-map-executable)" "(allow pseudo-tty)" "(allow file-ioctl)" "(allow system-socket)"
       "(allow ipc-posix-shm-read* ipc-posix-shm-write-data)"
       (str "(allow mach-lookup (global-name " (str/join " " (map q mach-services)) "))")
       (str "(allow file-read* (literal \"/\") (literal \"/private\") " (subpaths reads) ")")
       (rw [root scratch])
       (str "(allow file-read* file-write* (literal \"/dev/null\") (literal \"/dev/zero\") (literal \"/dev/tty\") "
            "(literal \"/dev/dtracehelper\") (literal \"/dev/ptmx\") (regex #\"^/dev/ttys[0-9]+$\"))")]
      (network-rules s)
      [(str "(deny file-read* file-write* (subpath " (q data-dir) "))")
       (rw (cond-> [scratch] (under? root data-dir) (conj root)))
       ;; what a tool needs from inside the data dir, read-only: tectonic's primed packages
       (if (seq data-read) (str "(allow file-read* " (subpaths data-read) ")") "")
       ;; a world's own developer reads what the supervisor and the world wrote about it
       (if (under? root data-dir) (str "(allow file-read* (subpath " (q (str data-dir "/logs")) "))") "")
       (str "(deny file-read* file-write* file-link "
            (str/join " " (map #(str "(regex #\"" (.pattern ^java.util.regex.Pattern %) "\")") secret-names))
            ")")
       ;; a command's own scratch holds nothing but what it made: a project's tests make a world there,
       ;; root.token and all, and the box stopped ORBIS's own suite from running at all
       (rw [scratch])]))))

(defn- bwrap-args
  "bwrap's form of the same box. Hides by name what Seatbelt denies by regex: what exists at start."
  [{:keys [root scratch data-dir read data-read network]}]
  (let [h (home)
        ro (filter #(.exists (io/file %)) (concat toolchain (map #(str h "/" %) home-toolchain) read))
        secrets (when (.isDirectory (io/file root))
                  (->> (file-seq (io/file root)) (take 20000) (filter #(.isFile ^File %))
                       (map #(.getPath ^File %)) (filter secret-path?)))]
    (concat ["bwrap" "--die-with-parent" "--unshare-pid" "--unshare-ipc" "--unshare-uts" "--proc" "/proc" "--dev" "/dev"]
            (mapcat (fn [p] ["--ro-bind" p p]) ro)
            ["--tmpfs" data-dir "--bind" root root "--bind" scratch scratch]
            (mapcat (fn [p] ["--ro-bind" p p]) data-read)
            (when (= "none" (str network)) ["--unshare-net"])
            (mapcat (fn [p] ["--ro-bind" "/dev/null" p]) secrets)
            ["--"])))

(defn command
  "argv inside the box."
  [s argv]
  (case (primitive)
    :seatbelt (into ["/usr/bin/sandbox-exec" "-p" (seatbelt-profile s)] argv)
    :bwrap (into (vec (bwrap-args s)) argv)
    (throw (ex-info "no sandbox on this host" {:err {:code :unconfined :message "no sandbox on this host"}}))))

(defn environment
  "The environment of a command: the allow-list, a terminal type, and scratch as TMPDIR."
  ^java.util.HashMap [s]
  (let [env (java.util.HashMap.)]
    (doseq [k env-kept]
      (when-let [v (System/getenv k)] (.put env k v)))
    (.put env "TERM" "xterm-256color")
    (when s (.put env "TMPDIR" (str (:scratch s) "/")))
    ;; a build can tell it has no network, and use what is on disk
    (when s (.put env "ORBIS_BOX" "1"))
    env))
