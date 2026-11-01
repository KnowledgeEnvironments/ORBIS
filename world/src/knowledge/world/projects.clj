(ns knowledge.world.projects
  "What a Principal's projects are (decisions 2026-10-31 'projects are in the record'). A project is a
   filesystem mount another Principal owns or p owns, other than the harness. Asked for status across
   projects, root searched the harness and the log for 12 runs and passed once: the facts were in the
   world, and nothing put them together. Everything here is read from the fold and the mount, so a
   status answer rests on the record, not on a model's guess."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.adapters.harness :as harness]
            [knowledge.world.code :as code]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.resolve.core :as resolve])
  (:import [java.io File]))

(def ^:private clip-chars 200)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- clip [s]
  (let [t (str/trim (str/replace (str s) #"\s+" " "))]
    (if (> (count t) clip-chars) (str (subs t 0 clip-chars) "...") t)))

(defn- world-name [db id]
  (let [w (objects/world-id db)
        ns (sort-by second (objects/names db id))]
    (or (some (fn [[n nm]] (when (= n w) nm)) ns) (some (fn [[_ nm]] nm) ns))))

(defn- locator-dir [l] (str/replace (str l) #"^file://" ""))

(defn- owner-of
  "The Principal whose principal/adapters holds the Adapter."
  [db adapter]
  (first (for [p (objects/objects-of-class db :principal)
               :when (some #{adapter} (field db p :principal/adapters))]
           p)))

(defn projects-of
  "[{:name :adapter :owner :path}] p can read, by name. The harness mount is the world's own source,
   not a project; `release/` trees of a self-hosting world are its mounts too."
  [db p]
  (->> (objects/objects-of-class db :adapter)
       (keep (fn [a]
               (let [n (world-name db a)
                     l (field db a :adapter/locator)]
                 (when (and (= :filesystem (field db a :adapter/binds)) (string? l) n
                            (not= n "harness") (not (objects/retired? db a))
                            (resolve/resolves? db p (str "/" a) :read))
                   (when-let [o (owner-of db a)]
                     {:name (str/replace n #"\.fs$" "") :adapter a :owner o :path (locator-dir l)})))))
       (sort-by :name)
       vec))

(defn about
  "What the project says it is: the first heading and the first line of prose of its README."
  [^String dir]
  (when-let [^File f (first (filter #(.isFile ^File %) (map #(io/file dir %) ["README.md" "README" "README.txt" "readme.md"])))]
    (let [lines (with-open [r (io/reader f)] (doall (take 40 (line-seq r))))
          text (remove #(or (str/blank? %) (re-find #"^\s*(<!--|\[!|!\[)" %)) lines)
          heading (first (filter #(str/starts-with? % "#") text))
          prose (first (remove #(str/starts-with? % "#") text))]
      (clip (str/join ": " (remove nil? [(some-> heading (str/replace #"^#+\s*" "")) prose]))))))

(defn- git-of [^String dir]
  (try
    (let [log (harness/git dir "log" "-1" "--format=%h %s (%cr)")]
      (when (zero? (long (:exit log)))
        {:branch (str/trim (:out (harness/git dir "rev-parse" "--abbrev-ref" "HEAD")))
         :last-commit (clip (:out log))
         :uncommitted (count (remove str/blank? (str/split-lines (:out (harness/git dir "status" "--porcelain")))))}))
    (catch Exception _ nil)))

(defn- top-level [^String dir]
  (->> (.listFiles (io/file dir))
       (remove #(str/starts-with? (.getName ^File %) "."))
       (sort-by #(.getName ^File %))
       (map #(str (.getName ^File %) (when (.isDirectory ^File %) "/")))
       (take 12) vec))

(defn- by-seq [db ids] (sort-by #(or (:object/created-seq (objects/entity db %)) 0) ids))

(defn- last-said
  "The newest Message the Principal sent as an answer, clipped."
  [db owner]
  (when-let [m (->> (objects/objects-of-class db :message)
                    (filter #(and (= owner (field db % :message/from))
                                  (not (#{:approval :status} (some-> (field db % :message/kind) name keyword)))))
                    (by-seq db) last)]
    (clip (field db m :message/text))))

(defn- last-run
  "The newest command the Principal's Tasks record, with its exit."
  [db owner]
  (when-let [t (->> (objects/objects-of-class db :task)
                    (filter #(and (= owner (field db % :task/requested-by))
                                  (some? (field db % :task/exit))))
                    (by-seq db) last)]
    {:command (clip (or (field db t :task/command) (field db t :task/summary)))
     :exit (field db t :task/exit)}))

(defn- shell-of [db owner dir]
  (first (for [a (field db owner :principal/adapters)
               :when (and (= :host (field db a :adapter/binds)) (= dir (locator-dir (field db a :adapter/locator))))]
           a)))

(defn- state-of [db owner]
  (let [stalled (str (field db owner :loop/stalled))]
    (cond (seq stalled) (str "stopped: " (clip stalled))
          (pos? (long (or (field db owner :loop/iteration) 0))) "working"
          :else "idle")))

(defn status
  "{:projects [...] :next ...}: each project's facts, from its files and the record. A mount made before
   test commands were recorded gets its command read from the project here."
  [db p]
  {:projects
   (vec (for [{:keys [name owner path]} (projects-of db p)
              :let [shell (shell-of db owner path)
                    tests (or (when shell (field db shell :adapter/test)) (code/test-command path))
                    git (git-of path)
                    said (last-said db owner)
                    ran (last-run db owner)
                    what (about path)]]
          (cond-> {:name name :path path :owner (world-name db owner) :state (state-of db owner)
                   :contents (top-level path)}
            what (assoc :about what)
            git (assoc :git git)
            tests (assoc :tests tests)
            ran (assoc :last-run ran)
            said (assoc :last-said said))))
   :next "answer from these facts. Each is on the record or in the project; to go deeper, ask the project's Principal."})

(defn line
  "The one line in the prompt that says what p's projects are. nil when there is nothing to say."
  [db p can-add?]
  (let [ps (projects-of db p)]
    (cond
      (seq ps) (str "Your projects: " (str/join ", " (map #(str (:name %) " (" (:path %) ")") ps))
                    ". For status across them call `projects`, then answer from what it returns.")
      can-add? "No projects yet: a directory a person names is add_project {name, path}, before any read or shell.")))

(defn source-line
  "Where p's own source is, when it can read it. Asked about the harness it runs in, root ran a shell at
   a path with the mount's name joined on, hid the errors, and told a person there was no code."
  [db p]
  (when-let [a (objects/lookup db (objects/world-id db) "harness")]
    (when (and (= :filesystem (field db a :adapter/binds)) (resolve/resolves? db p (str "/" a) :read))
      "Your own source is the mount `harness`: read `harness/README.md`, `harness/docs/<file>` by name, no shell.")))

(defn head-of-readme
  "The first lines of a project's README, for whoever just added it to write a charter from."
  [^String dir]
  (when-let [^File f (first (filter #(.isFile ^File %) (map #(io/file dir %) ["README.md" "README" "README.txt"])))]
    (let [t (with-open [r (io/reader f)] (str/join "\n" (doall (take 30 (line-seq r)))))]
      (if (> (count t) 1500) (subs t 0 1500) t))))
