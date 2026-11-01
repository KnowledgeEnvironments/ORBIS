(ns knowledge.world.pane.actions
  "The pane.* and file.open named actions (docs/panes.md). Grants apply to everyone here, a person
   as much as a model; the approval gate is the loop's (act/gates)."
  (:require [knowledge.world.act.unsafe :as unsafe]
            [clojure.string :as str]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.authority :as authority]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.pane.core :as core]
            [knowledge.world.pane.editor :as editor]
            [knowledge.world.pane.files :as files]
            [knowledge.world.pane.pty :as pty]
            [knowledge.world.pane.render :as render]
            [knowledge.world.pane.screen :as scr]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.view.core :as view])
  (:import [java.io File]
           [java.nio.file Files]))

(defn- fail! [code message] (throw (ex-info message {:err {:code code :message message}})))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))

(defn- result [world m] {:ok {:result m :seq (core/applied-seq world)}})

;; ---------------------------------------------------------------- targets

(defn- resolve-target
  "An Object p may use by id or name, with the Grant; else err unresolved, as for anything unseen."
  [world p target op]
  (let [db (act/db world)
        n (if-let [id (->uuid target)] (act/id-name id) (str target))
        r (resolve/resolve! world db p n op)]
    (if (:object r) r (throw (ex-info "unresolved" {:err (:err act/unresolved)})))))

(defn- place
  "{:adapter :locator :rel :grant} of a target: an fs Adapter, or a file Object under one."
  [world p target op rel]
  (let [{:keys [object grant]} (resolve-target world p target op)
        db (act/db world)
        backing (:object/backing (objects/entity db object))
        adapter (if (= :adapter (:object/class (objects/state db object))) object backing)
        rel (or rel (core/field db object :file/path) "")]
    (when-not adapter (fail! :invalid-args "that is neither an fs Adapter nor a file under a mount"))
    (when-not (= :filesystem (core/field db adapter :adapter/binds))
      (fail! :invalid-args "that Adapter is not a filesystem mount"))
    {:adapter adapter :locator (core/field db adapter :adapter/locator) :rel rel :grant grant}))

(defn- path-place
  "A host path as a place inside a mount or shell root p can read, or err outside-mount. Mounts first:
   a shell's root is a place too, and a path an agent prints from it should open."
  [world p path]
  (let [db (act/db world)
        adapters (objects/objects-of-class db :adapter)
        binds? (fn [kind] #(= kind (core/field db % :adapter/binds)))
        own (concat (filter (binds? :filesystem) adapters) (filter (binds? :host) adapters))
        readable (filter #(resolve/resolves? db p (act/id-name %) :read) own)
        file (File. (str path))
        within (fn [a]
                 (let [locator (core/field db a :adapter/locator)
                       root (fs/safe-file locator ".")
                       rp (.getPath root)
                       f (if (.isAbsolute file) (.getCanonicalFile file) (fs/safe-file locator (str path)))
                       fp (when f (.getPath f))]
                   (when (and fp (or (= fp rp) (str/starts-with? fp (str rp "/"))) (.exists (File. fp)))
                     {:adapter a :locator locator
                      :rel (if (= fp rp) "" (subs fp (inc (count rp))))
                      :grant (resolve/resolves? db p (act/id-name a) :read)})))]
    (or (some within readable)
        (fail! :outside-mount (str path " is in none of your mounts; workspace.mount its directory first")))))

(defn file-bytes
  "{:bytes :name :grant} of a file p may read, by host `path` or a mounted `name`. limit refuses a larger one."
  [world p {:keys [path name mount]} limit]
  (let [{:keys [locator rel grant]} (cond mount (place world p mount :read (str path))
                                          name (place world p name :read nil)
                                          :else (path-place world p path))
        ^File f (fs/safe-file locator rel)]
    (when-not (and f (.isFile f)) (fail! :invalid-args (str (or name path) " is not a file")))
    (when (> (.length f) (long limit)) (fail! :too-large (str (or name path) " is too large")))
    {:bytes (Files/readAllBytes (.toPath f)) :name (.getName f) :grant grant}))

(defn- kind-of-file
  "The pane kind for a file."
  [locator rel]
  (let [^File f (fs/safe-file locator rel)]
    (cond
      (nil? f) (fail! :outside-mount (str rel " is outside this mount"))
      (not (.exists f)) (fail! :invalid-args (str rel " does not exist"))
      (.isDirectory f) :files
      :else
      (let [bs (Files/readAllBytes (.toPath f))
            mime (fs/mime-of f bs)]
        (cond
          (str/ends-with? (str/lower-case (.getName f)) ".tex") :latex
          (= "text/markdown" mime) :markdown
          (and mime (str/starts-with? mime "image/")) :image
          (fs/text-mime? mime) :editor
          :else (fail! :invalid-args (str (.getName f) " is " mime ": no pane shows that")))))))

;; ---------------------------------------------------------------- delivery

(defn- asker
  "The Message's sender a model is answering, or nil."
  [db {:keys [message]}]
  (when message (core/field db message :message/from)))

(defn- deliver!
  "A pane a model opened, put in front of the person who asked: a view Message of kind pane, and
   that person is now a party to it. A session already has the id in its answer."
  [world p {:keys [pane kind title]} {:keys [session? route message via] :as opts}]
  (let [db (act/db world)
        who (asker db opts)]
    (when (and (not session?) who (not= who p))
      (core/set-field! world pane :pane/asker [:ref :principal] who)
      (when route
        (view/present! world p {:to #{who} :route route :kind :pane :title (or title (name kind))
                                :data {:pane (str pane) :kind (name kind)}
                                :about [pane] :in-reply-to message :via via})))))

;; ---------------------------------------------------------------- open

(defn- open-file-pane!
  "A pane on a file or directory at a place."
  [world p kind {:keys [adapter locator rel grant]} {:keys [title via]}]
  (let [^File f (fs/safe-file locator rel)
        title (or title (if (str/blank? rel) (.getName f) rel))
        common {:title title :grant grant :via via}]
    (when-not (and f (.exists f)) (fail! :invalid-args (str rel " does not exist")))
    (case kind
      :files (do (when-not (.isDirectory f) (fail! :invalid-args (str rel " is not a directory")))
                 (core/create! world p :files adapter (assoc common :fields {:pane/cwd rel :pane/selection []})))
      :editor (let [bs (Files/readAllBytes (.toPath f))
                    {:keys [blob/sha256]} (blob/put! (:blobs world) bs)]
                (when-not (fs/text-mime? (fs/mime-of f bs)) (fail! :invalid-args (str rel " is not text")))
                (core/create! world p :editor adapter (assoc common :blob sha256 :fields {:pane/path rel})))
      (:markdown :latex) (core/create! world p kind adapter (assoc common :fields {:pane/path rel :pane/scroll 1}))
      :image (let [bs (Files/readAllBytes (.toPath f))
                   receipt (blob/put! (:blobs world) bs)]
               (core/create! world p :image adapter
                             (assoc common :fields {:pane/path rel :pane/image receipt :pane/mime (fs/mime-of f bs)}))))))

(defn- open-field-pane!
  "An editor on a text Field: the Grant to read it opens, the Grant to write it saves. A Field with
   its own visibility stays out, or its text would be on the pane's stream for anyone who reads that."
  [world p {:keys [object field title]} {:keys [via]}]
  (let [{:keys [object grant]} (resolve-target world p object :read)
        db (act/db world)
        k (authority/field-key field)
        rec (get (objects/fields db object) k)
        value (:field/value rec)
        fname (subs (str k) 1)]
    (cond
      (nil? rec) (fail! :invalid-args (str "no Field " fname " on that Object"))
      (:field/visibility rec) (fail! :invalid-args (str fname " has its own visibility, so it does not open in a pane"))
      (not (string? value)) (fail! :invalid-args (str fname " is not text: only a string Field opens in an editor")))
    (let [{:keys [blob/sha256]} (blob/put! (:blobs world) (.getBytes ^String value "UTF-8"))
          opened (core/create! world p :editor object
                               {:title (or title (str (view/name-of db object) " \u00b7 " fname))
                                :grant grant :via via :blob sha256 :fields {:pane/field fname}})]
      (editor/watch-field! world (:pane opened))
      opened)))

(defn- open-terminal!
  [world p {:keys [target title cols rows cwd]} {:keys [via session?]}]
  (let [{:keys [object grant]} (resolve-target world p target :evaluate)
        db (act/db world)
        locator (core/field db object :adapter/locator)
        ;; the box is for agents: a person's own terminal runs as they do, and no agent can reach it
        own? (authority/person-session? db p session?)]
    (when-not (= :host (core/field db object :adapter/binds))
      (fail! :invalid-args "a terminal needs a shell Adapter"))
    (let [dir (fs/safe-file locator (or (not-empty cwd) (core/field db object :adapter/cwd) "."))]
      (when-not dir (fail! :outside-mount "cwd is outside the Adapter's locator"))
      (let [{:keys [box err]} (when-not own? (unsafe/box-for world object))]
        (when err (fail! (:code err) (:message err)))
        (core/open-terminal! world p {:adapter object :dir (.getPath dir) :title (or title "terminal") :box box
                                      :cols cols :rows rows :grant grant :via via
                                      :fields (when own? {:pane/person true})})))))

(defmethod actions/handle! :pane.open
  [_ world p {:keys [kind adapter path title] :as args} opts]
  (let [kind (keyword kind)
        ;; task.run names its shell `adapter`, so a model does here too
        target (or (:target args) adapter)
        args (assoc args :target target)
        _ (when (and (= :terminal kind) (nil? target))
            (fail! :invalid-args "a terminal needs target, a shell Adapter (shell.local, <you>.shell)"))
        _ (when (and (or (:object args) (:field args))
                     (not (and (= :editor kind) (:object args) (:field args))))
            (fail! :invalid-args "a Field opens as kind editor, with both object and field"))
        {:keys [pane seq]} (cond
                             (:field args) (open-field-pane! world p args opts)
                             (= :terminal kind) (open-terminal! world p args opts)
                             :else (open-file-pane! world p kind (if target
                                                                   (place world p target :read path)
                                                                   (path-place world p path))
                                                    opts))]
    (deliver! world p {:pane pane :kind kind :title title} opts)
    {:ok {:result {:pane pane :kind kind} :object pane :seq seq}}))

(defmethod actions/handle! :file.open
  [_ world p {:keys [path name as title]} opts]
  (when-not (or path name) (fail! :invalid-args "file.open needs path or name"))
  (let [where (if name (place world p name :read nil) (path-place world p path))
        kind (if as (keyword as) (kind-of-file (:locator where) (:rel where)))
        {:keys [pane seq]} (open-file-pane! world p kind where (assoc opts :title title))]
    (deliver! world p {:pane pane :kind kind :title (or title (:rel where))} opts)
    {:ok {:result {:pane pane :kind kind :path (:rel where)} :object pane :seq seq}}))

;; ---------------------------------------------------------------- the rest of the table

(defn- pane! [world p pane allowed?]
  (let [id (core/pane-id world pane)]
    (if (and id (allowed? (act/db world) p id))
      id
      (throw (ex-info "unresolved" {:err (:err act/unresolved)})))))

(defn- summary [db id]
  (let [f (partial core/field db id)]
    (cond-> {:pane id :kind (f :pane/kind) :state (f :pane/state) :owner (f :pane/owner)}
      (f :pane/title) (assoc :title (f :pane/title))
      (f :pane/size) (assoc :size (f :pane/size))
      (f :pane/target) (assoc :target (f :pane/target)))))

(defmethod actions/handle! :pane.list
  [_ world p {:keys [principal]} _]
  (let [db (act/db world)
        who (when principal (or (->uuid principal) (resolve/lookup db p (str principal))))
        ids (->> (objects/objects-with-field db :pane/kind)
                 (remove #(objects/retired? db %))
                 (filter #(core/may-read? db p %))
                 (filter #(or (nil? who) (= who (core/field db % :pane/owner)))))]
    (result world {:panes (mapv #(summary db %) (sort-by str ids))})))

(defmethod actions/handle! :pane.close
  [_ world p {:keys [pane]} _]
  (let [id (pane! world p pane core/may-drive?)]
    (core/close! world id)
    (result world {:pane id :closed true})))

(defmethod actions/handle! :pane.resize
  [_ world p {:keys [pane cols rows]} _]
  (let [id (pane! world p pane core/may-drive?)]
    (core/resize! world id cols rows)
    (result world {:pane id :cols cols :rows rows})))

(defn- read-file-text [world id]
  (let [db (act/db world)
        locator (core/field db (core/field db id :pane/target) :adapter/locator)]
    (slurp (fs/safe-file locator (core/field db id :pane/path)))))

(defn- line-range
  "1-based inclusive lines of a seq of lines."
  [lines [from to]]
  (let [n (count lines)
        from (max 1 (long (or from 1)))
        to (min n (long (or to n)))]
    (if (> from to) [] (subvec (vec lines) (dec from) to))))

(defn- text-result [world id text range]
  (let [lines (str/split-lines text)]
    (result world (cond-> {:pane id :text (if range (str/join "\n" (line-range lines range)) text)
                           :lines (count lines)}
                    range (assoc :range range)))))

(def bytes-chunk "What one pane.read of `bytes` carries, under the socket's frame limit." (* 1024 1024))

(defn- read-bytes
  "A binary Field of a pane, base64, a chunk at a time: `field` (pane/image by default) from `offset`.
   `more` says there is another chunk, so a phone need not hold a whole page in one frame."
  [world id {:keys [field offset limit]}]
  (let [db (act/db world)
        k (keyword (or field "pane/image"))
        receipt (when (= "pane" (namespace k)) (core/field db id k))
        bs (when (blob/receipt? receipt) (blob/get (:blobs world) (:blob/sha256 receipt)))]
    (if-not bs
      (act/err :invalid-args (str "this pane holds no bytes in " (subs (str k) 1)))
      (let [size (alength ^bytes bs)
            from (min size (max 0 (long (or offset 0))))
            n (min (- size from) (max 1 (min bytes-chunk (long (or limit bytes-chunk)))))
            chunk (java.util.Arrays/copyOfRange ^bytes bs (int from) (int (+ from n)))]
        (result world {:pane id :mime (core/field db id :pane/mime) :size size :offset from
                       :base64 (.encodeToString (java.util.Base64/getEncoder) chunk)
                       :more (< (+ from n) size)})))))

(defmethod actions/handle! :pane.read
  [_ world p {:keys [pane what range] :as args} _]
  (let [id (pane! world p pane core/may-read?)
        db (act/db world)
        kind (core/field db id :pane/kind)
        what (keyword (or what (case kind :terminal :screen :files :listing :buffer)))]
    (if (= :bytes what)
      (read-bytes world id args)
    (case [kind what]
      [:terminal :screen]
      (let [s (core/terminal-screen world id)]
        (result world {:pane id :text (scr/text s) :cursor (scr/cursor s) :cols (:cols s) :rows (:rows s)
                       :state (core/field db id :pane/state)
                       :alternate (some? (:alt s))
                       :exit (core/field db id :pane/exit)}))
      [:terminal :scrollback]
      (let [lines (vec (reverse (drop-while str/blank? (reverse (core/terminal-lines world id)))))
            picked (if range (line-range lines range) (vec (take-last 500 lines)))]
        (result world {:pane id :lines picked :total (count lines)}))
      [:files :listing]
      (result world (assoc (files/read-listing world id) :pane id))
      [:editor :buffer]
      (let [{:keys [text version holder]} @(editor/buffer world id)]
        (update-in (text-result world id text range) [:ok :result] assoc :version version :holder holder))
      ([:markdown :buffer] [:latex :buffer])
      (text-result world id (read-file-text world id) range)
      [:image :buffer]
      (result world {:pane id :mime (core/field db id :pane/mime) :image (core/field db id :pane/image)})
      (fail! :invalid-args (str "a " (name kind) " pane cannot read " (name what)))))))

;; ---------------------------------------------------------------- send

(defn- no-pane
  "Refuse before the gate: a person should never approve a send that cannot arrive."
  [world pane]
  (when-not (core/pane-id world pane)
    (act/err :invalid-args (str pane " is not a pane: give the `pane` id that pane.open answered, not its Adapter"))))

(defmethod actions/preflight! :pane.send [_ world _ {:keys [pane]}] (no-pane world pane))
(defmethod actions/preflight! :pane.frob [_ world _ {:keys [pane]}] (no-pane world pane))

(def key-bytes
  "Named keys, as a terminal sends them."
  (merge {"enter" "\r" "tab" "\t" "esc" "\u001b" "backspace" "\u007f" "space" " " "delete" "\u001b[3~"
          "up" "\u001b[A" "down" "\u001b[B" "right" "\u001b[C" "left" "\u001b[D" "home" "\u001b[H"
          "end" "\u001b[F" "pgup" "\u001b[5~" "pgdn" "\u001b[6~"}
         (into {} (for [c "abcdefghijklmnopqrstuvwxyz"]
                    [(str "ctrl-" c) (str (char (inc (- (int c) (int \a)))))]))))

(defn- keys-text [ks]
  (apply str (map (fn [k] (or (key-bytes (str k)) (fail! :invalid-args (str "no key named " k)))) ks)))

(def ^:private send-wait-ms "How long a model's send waits for a program to go quiet, by default." 8000)

(defmethod actions/handle! :pane.send
  [_ world p {:keys [pane text keys wait-ms]} {:keys [session?]}]
  (let [id (pane! world p pane core/may-read?)
        db (act/db world)]
    (when-not (= :terminal (core/field db id :pane/kind)) (fail! :invalid-args "pane.send is for a terminal"))
    (when-not (core/may-evaluate? db p id) (throw (ex-info "unresolved" {:err (:err act/unresolved)})))
    (when-not (core/terminal-live world id) (fail! :invalid-args "that terminal has exited"))
    (let [s (str text (when keys (keys-text keys)))
          bs (.getBytes s "UTF-8")]
      (when (empty? s) (fail! :invalid-args "pane.send needs text or keys"))
      ;; a rule read this terminal as a prompt: anything but a whole admitted line leaves that unknown
      (when-not (core/field db id :pane/dirty)
        (when (or session? keys (not (actions/rule-line? db p id s)))
          (core/set-field! world id :pane/dirty :boolean true)))
      (let [started (System/currentTimeMillis)
            seq (core/send! world p id bs (when-not session? s) (if session? "session" "loop"))]
        ;; a client's keystrokes go straight back; a model's answer carries the screen that came of them
        (when-not session? (core/await-quiet! world id started (min 60000 (long (or wait-ms send-wait-ms)))))
        {:ok {:result (cond-> {:pane id :sent (alength bs)}
                        (not session?) (assoc :screen (scr/text (core/terminal-screen world id))))
              :seq seq}}))))

;; ---------------------------------------------------------------- frob

(defn- may-write-path?
  "A write in a mount: :write on its Adapter, or on the mount's names for this path."
  [db p adapter rel]
  (or (resolve/resolves? db p (act/id-name adapter) :write)
      (some (fn [[_ n]]
              (when (str/ends-with? n ".fs")
                (let [prefix (subs n 0 (- (count n) 3))]
                  ;; a path about to exist has no name yet: the mount's own root stands for it
                  (or (resolve/resolves? db p (str prefix "/" rel) :write)
                      (resolve/resolves? db p (str prefix "/.") :write)))))
            (objects/names db adapter))))

(defn- writable! [db p adapter rels]
  (when (core/field db adapter :adapter/read-only)
    (fail! :not-allowed "this mount is read-only"))
  (when-not (every? #(may-write-path? db p adapter %) rels)
    (throw (ex-info "unresolved" {:err (:err act/unresolved)}))))

(defn- files-frob [world p id verb args opts]
  (let [db (act/db world)
        adapter (core/field db id :pane/target)
        cwd (or (core/field db id :pane/cwd) "")]
    (when (files/writes verb) (writable! db p adapter (files/touched-paths cwd verb args)))
    (let [{:keys [open] :as r} (files/frob! world p id verb args)]
      (if open
        (let [locator (core/field db adapter :adapter/locator)
              kind (if (:as args) (keyword (:as args)) (kind-of-file locator open))
              where {:adapter adapter :locator locator :rel open
                     :grant (:object/created-under (objects/entity db id))}
              {:keys [pane seq]} (open-file-pane! world p kind where opts)]
          (deliver! world p {:pane pane :kind kind :title open} opts)
          {:ok {:result {:pane pane :kind kind :path open} :object pane :seq seq}})
        r))))

(defn- field-frob
  "save and revert on a pane over a Field. The Field's rules are authority/write-refusal and the write's Grant."
  [world p id verb args {:keys [via] :as opts}]
  (let [db (act/db world)
        object (core/field db id :pane/target)
        fname (core/field db id :pane/field)]
    (case verb
      :save (or (authority/write-refusal fname (authority/person-session? db p (:session? opts)))
                (editor/save-field! world p id args via))
      :revert (editor/revert! world p id args (str (core/field db object (keyword fname)))))))

(defn- editor-frob [world p id verb args opts]
  (let [db (act/db world)
        adapter (core/field db id :pane/target)
        rel (core/field db id :pane/path)]
    (cond
      (and (core/field db id :pane/field) (#{:save :revert} verb)) (field-frob world p id verb args opts)
      (= :save verb) (do (writable! db p adapter [rel])
                         (editor/save! world p id args (core/field db adapter :adapter/locator) rel))
      (= :revert verb) (editor/revert! world p id args (slurp (fs/safe-file (core/field db adapter :adapter/locator) rel)))
      :else (editor/frob! world p id verb args))))

(defn- viewer-frob [world p id kind verb args]
  (case verb
    (:scroll :goto)
    (let [line (long (or (:line args) (:to args) 1))]
      (core/set-field! world id :pane/scroll :number line)
      (core/push! world id {:state {:scroll line}})
      (result world {:pane id :scroll line}))
    :render
    (if (= :markdown kind)
      (text-result world id (read-file-text world id) nil)
      (let [db (act/db world)
            locator (core/field db (core/field db id :pane/target) :adapter/locator)
            source (fs/safe-file locator (core/field db id :pane/path))
            {:keys [ok err] :as r} (render/latex! world source)]
        (if err
          r
          (let [{:keys [pdf image log]} ok]
            (when image (core/set-field! world id :pane/image :bytes image))
            (core/set-field! world id :pane/pdf :bytes pdf)
            (core/push! world id {:state {:rendered (:blob/sha256 (or image pdf))}})
            (result world {:pane id :log log
                           :view (if image
                                   {:kind "image" :data {:bytes image :mime "image/png"}}
                                   {:kind "image" :data {:bytes pdf :mime "application/pdf"}})})))))
    (fail! :invalid-args (str "a " (name kind) " pane has no verb " (name verb)))))

(defn- own-terminal!
  "The pane, a live terminal this person opened, or throws."
  [world p pane]
  (let [id (pane! world p pane core/may-read?)
        db (act/db world)]
    (when-not (and (authority/person-terminal? db id) (= p (core/field db id :pane/owner)))
      (fail! :invalid-args "only your own terminal can be shared"))
    id))

(defmethod actions/handle! :pane.share
  [_ world p {:keys [pane principal mode minutes]} opts]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (let [id (own-terminal! world p pane)
          agent (or (->uuid principal) (resolve/lookup db p (str principal)))]
      (when-not (and agent (= :process (:principal/driven-by (objects/entity db agent))))
        (fail! :invalid-args "share with one agent, by name"))
      (when-not (core/terminal-live world id) (fail! :invalid-args "that terminal has exited"))
      (unsafe/share! world p id agent {:mode mode :minutes minutes}))))

(defmethod actions/handle! :pane.unshare
  [_ world p {:keys [pane principal] :as args} opts]
  (let [db (act/db world)]
    (authority/require-person! db p opts)
    (unsafe/unshare! world p (own-terminal! world p pane)
                     (when principal (or (->uuid principal) (resolve/lookup db p (str principal)))))))

(defmethod actions/handle! :pane.frob
  [_ world p {:keys [pane verb args]} opts]
  (let [id (pane! world p pane core/may-drive?)
        kind (core/field (act/db world) id :pane/kind)
        verb (keyword verb)
        args (or args {})]
    (case kind
      :files (files-frob world p id verb args opts)
      :editor (editor-frob world p id verb args opts)
      (:markdown :latex) (viewer-frob world p id kind verb args)
      (fail! :invalid-args (str "a " (name kind) " pane has no verbs")))))
