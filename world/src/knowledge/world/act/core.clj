(ns knowledge.world.act.core
  "The write primitives as functions of (world principal args), each through resolution
   (ground rule 4) and each an event (ground rule 3): create!, bind!, set-field!, grant!, read!.
   The wire ops (protocol.ops) call these with the session principal; kernel-zero (spec 17.3) and
   the resident loop (spec 9.8) call them as root. Every function returns {:ok ...} or
   {:err {:code :message}}, the shape the server turns into a response.

   create! (spec 12.2, ADR 0008) knows the create action for object, principal, group, model,
   adapter and route (spec 5.1, 5.6, 5.7, 5.10). A Model of kind :model carries base-url,
   model-id, key-env (the name of the environment variable holding its key; the key itself is
   never stored, spec 11.5 'the Adapter holds any key') and format :openai-chat."
  (:require [clojure.java.io :as io]
            [knowledge.world.memory :as memory]
            [knowledge.world.act.gates-defaults :as gates-defaults]
            [clojure.string :as str]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.index.core :as index]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.refs :as refs]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.tokens :as tokens])
  (:import [java.nio.file Files]
           [java.nio.file.attribute PosixFilePermissions]))

(declare coerce-value)
(defn err [code message] {:err {:code code :message message}})
(def unresolved (err :unresolved "does not resolve"))

(defn db [world] (index/db (:index world)))
(defn id-name [id] (str "/" id))
(defn append! [world event] (log/append!! (:log world) event))

(defn kw [v] (cond (keyword? v) v (string? v) (keyword v) :else nil))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- ids [vs] (into [] (keep ->uuid) vs))

;; ---------------------------------------------------------------- create

(def creatable #{:object :principal :group :model :adapter :route :task :trigger})

(defn schema-name
  "The world-namespace name of a class's schema Object: :principal -> \"schema.Principal\"."
  [class]
  (str "schema." (str/capitalize (name class))))

(defn- initial-fields
  "The class fields of a new Object from the create args (spec 4, 5)."
  [p class {:keys [driven-by kind members to fields format residency base-url model-id key-env adapter
                   binds locator sync transport address input-tags output-format title timeout-s max-tokens
                   reasoning reasoning-kwargs every-s at watch route text armed max-fires unless-empty unless-told
                   price-in price-out context-tokens label name]} id]
  (case class
    :task (cond-> {:task/owner p :task/state :open :task/blocked-on #{} :task/evidence #{}}
            title (assoc :task/title (str title)))
    ;;: a Trigger is owned by its creator; firing sends a Message from the owner
    ;; (to `to`, default itself, on `route`, default its first Route) carrying `text`; born disarmed
    ;; unless armed is true, so a person may inspect it first; arm/disarm are the ops
    :trigger (let [kind (or (#{:interval :at :on-change :on-message} (kw kind)) :interval)
                   armed? (true? armed)]
               (cond-> {:trigger/owner p :trigger/kind kind :trigger/armed armed?
                        :trigger/state (if armed? :armed :disarmed) :trigger/fired-count 0 :trigger/skipped 0
                        :trigger/to (or (->uuid to) p)}
                 (number? every-s) (assoc :trigger/every-s every-s)
                 (string? every-s) (assoc :trigger/every-s (coerce-value :number every-s))
                 (inst? at) (assoc :trigger/at at)
                 (string? at) (assoc :trigger/at (coerce-value :instant at))
                 (map? watch) (assoc :trigger/watch (cond-> {:object (->uuid (:object watch))}
                                                      (:field watch) (assoc :field (kw (:field watch)))))
                 (->uuid route) (assoc :trigger/route (->uuid route))
                 (string? text) (assoc :trigger/text text)
                 (number? max-fires) (assoc :trigger/max-fires (long max-fires))
                 (true? unless-empty) (assoc :trigger/unless-empty true)
                 (true? unless-told) (assoc :trigger/unless-told true)
                 armed? (assoc :trigger/armed-at (java.util.Date.))))
    :object (into {} (filter (fn [[k v]] (and (keyword? k) (some? v)))) fields)
    ;; a process Principal is born with its memory defaults
    :principal (cond-> {:principal/driven-by (or (kw driven-by) :person) :principal/namespace id}
                 (= :process (or (kw driven-by) :person)) (merge memory/defaults gates-defaults/defaults))
    :group {:principal/driven-by :none :principal/namespace id
            :group/kind (or (kw kind) :custom) :group/members (set (ids members))}
    ;; the catalogue Fields, every Model born with them (label, prices 0, status unknown)
    :model (cond-> {:principal/driven-by :none :principal/namespace id
                     :model/kind (or (kw kind) :model) :model/format (or (kw format) :openai-chat)
                     :model/residency (or (kw residency) :external) :model/capacity {} :model/pricing {}
                     :model/label (str (or label name model-id "model"))
                     :model/status :unknown}
              ;; no price Fields unless someone names them; absent means unpriced
              (number? price-in) (assoc :model/price-in price-in)
              (number? price-out) (assoc :model/price-out price-out)
              (number? context-tokens) (assoc :model/context-tokens (long context-tokens))
              base-url (assoc :model/base-url base-url :model/api-mode :chat-completions)
              model-id (assoc :model/id model-id)
              key-env (assoc :model/key-env key-env)
              (number? timeout-s) (assoc :model/timeout-s (long timeout-s))
              (number? max-tokens) (assoc :model/max-tokens (long max-tokens))
              (#{:off :on :auto} (kw reasoning)) (assoc :model/reasoning (kw reasoning))
              (map? reasoning-kwargs) (assoc :model/reasoning-kwargs reasoning-kwargs)
              (->uuid adapter) (assoc :model/adapter (->uuid adapter)))
    :adapter {:principal/driven-by :none :principal/namespace id
              :model/kind (or (kw kind) :custom) :model/format :none :model/residency :local :model/capacity {} :model/pricing {}
              :adapter/binds (or (kw binds) :transport) :adapter/locator (or locator "local://")
              :adapter/sync (or (kw sync) :both) :adapter/objects #{} :adapter/last-seen {}}
    :route (cond-> {:route/address (or address "") :route/input-tags (ids input-tags) :route/hard-rules []
                    :route/output-format (or (kw output-format) :text)
                    ;; explicit membership, from members or to at creation, then maintained
                    :route/members (vec (sort-by str (distinct (ids (or members to)))))}
             (->uuid transport) (assoc :route/transport (->uuid transport)))))

(defn token-file-name
  "data/<name>.token for a created Principal: the name with anything but [A-Za-z0-9._-] replaced."
  [n]
  (str (str/replace (str n) #"[^A-Za-z0-9._-]" "-") ".token"))

(defn write-token-file!
  "The credential of a new Principal as a 0600 file under the data dir (decisions 2026-10-30
   'tokens never travel through a loop'). Returns the path or nil when there is no name or dir."
  [world n token]
  (when (and n (:data-dir world))
    (let [f (io/file (:data-dir world) (token-file-name n))]
      (spit f token)
      (try (Files/setPosixFilePermissions (.toPath f) (PosixFilePermissions/fromString "rw-------"))
           (catch Exception _ nil))
      (.getPath f))))

(defn- typed-field?
  "A create-time Field given with its type: {:type t :value v} (an attachment is one op)."
  [v]
  (and (map? v) (contains? v :type) (contains? v :value) (= 2 (count (select-keys v [:type :value])))))

(defn- typed-value
  "The value of a typed create-time Field in the shape its type names: bytes arrive as base64 (or
   bytes) and go to the blob store above the threshold; everything else through coerce-value."
  [world type v]
  (let [t (if (vector? type) (first type) type)]
    (if (= :bytes (keyword (name t)))
      (let [bs (cond (bytes? v) v
                     (string? v) (try (.decode (java.util.Base64/getDecoder) ^String v)
                                      (catch Exception _ (.getBytes ^String v "UTF-8")))
                     :else (byte-array 0))]
        (if (> (alength ^bytes bs) blob/threshold) (blob/put! (:blobs world) bs) bs))
      (coerce-value type v))))

(declare bind-in-world! bind-roots-name!)

(defn create!
  "The create action on a class (ADR 0008): resolution of the class's schema Object for op
   :create, then `created`, a credential for person and process Principals, a `bound` in the
   creator's namespace when :name is given, and for a Route the template as a reader :agent Field.
   Returns {:ok {:result {:token? :token-file?} :object id :seq}}. The credential is written to
   data/<name>.token when the Principal has a name, and returned in the result only when opts
   :return-token? is true (a session asked; a loop's result would put it on the log)."
  ([world p class args] (create! world p class args {:return-token? true}))
  ([world p class args {:keys [return-token?]}]
  (let [db (db world)]
    (cond
      (not (creatable class))
      (err :not-implemented (str "create for class " (some-> class name) " is not built yet"))

      ;; people are named where everyone can see them, so the name must be free (decisions 2026-10-31)
      (and (= class :principal) (= :person (some-> (:driven-by args) name keyword)) (:name args)
           (objects/lookup db (objects/world-id db) (:name args)))
      (err :conflict (str "the name " (:name args) " is taken"))

      :else
      (if-let [{:keys [grant]} (resolve/resolve! world db p (schema-name class) :create)]
        (let [id (random-uuid)
              ;; typed create-time Fields (an Object's {name {type value}}) are set after `created` with
              ;; their declared type, as a field-set would; the plain {name value} form stays as it was
              typed (when (= class :object) (into {} (filter (fn [[_ v]] (typed-field? v))) (:fields args)))
              args (if (seq typed) (update args :fields #(apply dissoc % (keys typed))) args)
              fields (initial-fields p class args id)
              token (when (and (= class :principal) (#{:person :process} (:principal/driven-by fields))) (session/new-token))
              seq (append! world (log/pending :created id p {:class class :fields fields} :delegation grant))
              seq (if token
                    (append! world (log/pending :field-set id p {:name :principal/token-hash :type :string
                                                                 :value (session/token-hash token) :reader :agent
                                                                 :visibility grant}
                                                :delegation grant))
                    seq)
              seq (if (and (= class :route) (string? (:template args)))
                    (append! world (log/pending :field-set id p {:name :route/template :type :string
                                                                 :value (:template args) :reader :agent}
                                                :delegation grant))
                    seq)
              seq (reduce (fn [_ [k {:keys [type value reader]}]]
                            (let [type (if (string? type) (keyword type) type)
                                  type (if (and (vector? type) (string? (first type))) (mapv #(if (string? %) (keyword %) %) type) type)]
                              (append! world (log/pending :field-set id p {:name (if (string? k) (keyword k) k) :type type
                                                                           :value (typed-value world type value)
                                                                           :reader (or (some-> reader kw) :both)}
                                                          :delegation grant))))
                          seq (sort-by (comp str key) typed))
              seq (if-let [n (:name args)]
                    (append! world (log/pending :bound p p {:namespace p :name n :object id} :streams #{id} :delegation grant))
                    seq)
              ;; a person's name, and anything root names, is the world's: root's namespace is not
              ;; on anyone else's lookup chain, so root.model would be an id to the world's owner
              _ (when-let [n (:name args)]
                  (if (= :person (:principal/driven-by fields))
                    (bind-in-world! world id n)
                    (bind-roots-name! world p id n)))
              token-file (when token (write-token-file! world (:name args) token))
              ;; the credential is an Object too (label: born with the Principal)
              _ (when token (tokens/record! world {:principal id :secret token :issued-by p
                                                   :label (str (or (:name args) "principal") " (at creation)")}))]
          {:ok {:result (cond-> {}
                          (and token return-token?) (assoc :token token)
                          token-file (assoc :token-file token-file))
                :object id :seq seq}})
        unresolved)))))

;; ---------------------------------------------------------------- names, fields
(defn bind-in-world!
  "Bind n to object in the world namespace, the world as actor: a name grants nothing, and every
   Principal must be able to see who is speaking. Does nothing when already bound so."
  [world object n]
  (let [ws (:world-id world)]
    (when (not= object (objects/lookup (db world) ws n))
      (append! world (log/pending :bound ws ws {:namespace ws :name n :object object} :streams #{object})))))

(defn bind-roots-name!
  "What root names is the world's when the name is free there."
  [world p object n]
  (let [d (db world)]
    (when (and (= p (objects/root-id d)) (nil? (objects/lookup d (:world-id world) n)))
      (bind-in-world! world object n))))

(defn rebind!
  "In namespace ns, object's name becomes new-name: `bound` then `unbound`, under grant, a Grant to
   write the object (object.rename: renaming is the object's writer's to do, not the namespace's)."
  [world p grant ns object was new-name]
  (append! world (log/pending :bound ns p {:namespace ns :name new-name :object object} :streams #{object} :delegation grant))
  (append! world (log/pending :unbound ns p {:namespace ns :name was :object object} :streams #{object} :delegation grant)))

(defn drop-name!
  "`unbound` of object's name in ns under grant, a Grant to write the object (object.retire)."
  [world p grant ns object n]
  (append! world (log/pending :unbound ns p {:namespace ns :name n :object object} :streams #{object} :delegation grant)))

(defn restore-name!
  "`bound` of object's name in ns under grant (object.restore)."
  [world p grant ns object n]
  (append! world (log/pending :bound ns p {:namespace ns :name n :object object} :streams #{object} :delegation grant)))

(defn unbound-in-world
  "[[id name] ...]: person and process Principals named somewhere but not in the world namespace,
   and anything named only in root's namespace, whose name is free there (a taken name stays as it is)."
  [db]
  (let [ws (objects/world-id db)
        root (objects/root-id db)
        principals (for [p (objects/objects-of-class db :principal)
                         :when (#{:person :process} (:principal/driven-by (objects/entity db p)))]
                     p)
        roots-own (when root (distinct (vals (objects/bound-names db root))))]
    (vec (for [p (distinct (concat principals roots-own))
               :let [names (objects/names db p)]
               :when (and (seq names) (not-any? (fn [[ns _]] (= ns ws)) names))
               :let [n (first (sort (map second names)))]
               :when (nil? (objects/lookup db ws n))]
           [p n]))))


(defn bind!
  "`bound` in namespace ns (default the principal's own): the object must exist and the
   principal must resolve the namespace holder for :write (decisions 2026-10-30)."
  [world p {:keys [namespace name object]}]
  (let [db (db world) ns (or namespace p)]
    (if-let [{:keys [grant]} (and (objects/exists? db object) (resolve/resolve! world db p (id-name ns) :write))]
      {:ok {:seq (append! world (log/pending :bound ns p {:namespace ns :name name :object object}
                                            :streams #{object} :delegation grant))}}
      unresolved)))

(defn unbind! [world p {:keys [namespace name]}]
  (let [db (db world) ns (or namespace p)
        object (objects/lookup db ns name)]
    (if-let [{:keys [grant]} (and object (resolve/resolve! world db p (id-name ns) :write))]
      {:ok {:seq (append! world (log/pending :unbound ns p {:namespace ns :name name :object object}
                                            :streams #{object} :delegation grant))}}
      unresolved)))

(defn members-of-route
  "The Principals whose principal/routes lists route (spec 5.7)."
  [db route]
  (set (for [p (objects/objects-with-field db :principal/routes)
             :when (contains? (set (get-in (objects/fields db p) [:principal/routes :field/value])) route)]
         p)))

(defn- maintain-route-members!
  "route/members on each Route a Principal's principal/routes gained or lost: the
   kernel keeps the Route side of the relation, as it keeps loop/* bookkeeping, under the same
   delegation the principal/routes write used."
  [world p principal prev-routes routes grant via]
  (let [prev (set (filter uuid? prev-routes)) now (set (filter uuid? routes))
        db (db world)]
    (doseq [route (sort-by str (into (remove prev now) (remove now prev)))
            :let [s (objects/state db route)]
            :when (= :route (:object/class s))]
      (let [members (set (get-in s [:object/fields :route/members :field/value]))
            after (if (contains? now route) (conj members principal) (disj members principal))]
        (append! world (log/pending :field-set route p {:name :route/members :type [:list [:ref :principal]]
                                                        :value (vec (sort-by str after)) :reader :both
                                                        :previous (vec (sort-by str members))}
                                    :delegation grant :via via))))))

(defn coerce-value
  "A Field value in the form its declared type names (spec 4.3, ADR 0002 JSON form): the JSON form
   carries ids, keywords and instants as strings, so a ref becomes a UUID, a keyword a keyword, an
   instant a Date, a number a number, a boolean a boolean, recursively through list and map. A value
   already in shape, or one that does not parse, is returned as it came (decisions 2026-10-30)."
  [type v]
  (let [t (if (vector? type) (first type) type)
        t (when t (keyword (name t)))
        inner (when (vector? type) (second type))]
    (case t
      :ref (or (->uuid v) v)
      :list (if (sequential? v) (mapv #(coerce-value inner %) v) v)
      :map (if (map? v) (into {} (map (fn [[k x]] [k (coerce-value inner x)])) v) v)
      :keyword (if (and (string? v) (seq v)) (keyword v) v)
      :instant (if (string? v) (try (java.util.Date/from (java.time.Instant/parse v)) (catch Exception _ v)) v)
      :number (if (string? v) (try (Long/parseLong v) (catch Exception _ (try (Double/parseDouble v) (catch Exception _ v)))) v)
      :boolean (if (string? v) (= "true" v) v)
      v)))

(declare copy-source)

(defn set-field!
  "`field-set` on object by p (spec 4.3): :write on the object; the value is first coerced to the
   form its declared type names (coerce-value). via is the Evaluation that set it.
   A write to principal/routes also maintains route/members on the Routes gained or lost.
   from {object|name, field?, range?} stores another Field's value instead of value, copied by the
   kernel (copy-source): a model keeps a document whole without retyping it."
  [world p {:keys [object field type value reader visibility via from] :as args}]
  (if from
    (let [{:keys [ok err]} (copy-source world p from via)]
      (if err
        {:err err}
        (set-field! world p (-> args (dissoc :from)
                                (assoc :value (:value ok) :type (or type (:type ok)))))))
  (let [db (db world)
        value (coerce-value type value)]
    (if-let [{:keys [grant]} (resolve/resolve! world db p (id-name object) :write)]
      (let [prev (get-in (objects/fields db object) [field :field/value])
            body (cond-> {:name field :type type :value value :reader (or reader :both)}
                   visibility (assoc :visibility visibility)
                   (and (some? prev) (not (bytes? prev))) (assoc :previous prev))
            seq (append! world (log/pending :field-set object p body :delegation grant :via via))]
        (when (= :principal/routes field)
          (maintain-route-members! world p object prev value grant via))
        ;; a text's spans sit beside it; new text that has none must not keep the old ones
        (when (and (string? value) (or (not-empty (:refs args)) (contains? (objects/fields db object) (refs/refs-key field))))
          (append! world (log/pending :field-set object p {:name (refs/refs-key field) :type [:list :map]
                                                           :value (vec (:refs args)) :reader :both}
                                      :delegation grant :via via)))
        {:ok (cond-> {:seq seq} (not-empty (:unresolved-refs args)) (assoc :unresolved-refs (:unresolved-refs args)))})
      unresolved))))

(defn- text-of
  "A Field value as text: a string, bytes, or a blob receipt fetched from the blob store; else nil."
  [world v]
  (cond (string? v) v
        (bytes? v) (String. ^bytes v "UTF-8")
        (blob/receipt? v) (some-> (blob/get (:blobs world) (:blob/sha256 v)) (String. "UTF-8"))
        :else nil))

(defn pruned-blob?
  "A receipt whose bytes the retention sweep has taken. The receipt stays: hash and size still say what
   was there."
  [world v]
  (and (blob/receipt? v) (not (blob/exists? (:blobs world) (:blob/sha256 v)))))

(def pruned-message "retention pruned these bytes; the hash and size remain")

(def byte-page-cap "Most bytes one read returns; a client pages." (* 64 1024))

(defn- utf8-start
  "i, moved forward past any continuation bytes, so a page never begins inside a character."
  [^bytes b i]
  (loop [i (long i)]
    (if (and (< i (alength b)) (= 0x80 (bit-and (aget b i) 0xC0))) (recur (inc i)) i)))

(defn- byte-page
  "{:text :from :to :size :truncated} for bytes [start end) of text; a one-line Field has no lines
   to page by, a whole Serialization among them. The page ends on a character boundary."
  [^String text [start end]]
  (let [b (.getBytes text "UTF-8")
        size (alength b)
        from (utf8-start b (max 0 (min (long start) size)))
        want (max from (min (long end) size))
        to (utf8-start b (min want (+ from (long byte-page-cap))))]
    {:text (String. b (int from) (int (- to from)) "UTF-8")
     :from from :to to :size size :truncated (< to want)}))

(defn- page-field
  "The Field a range is asked of: the named one, else a file's text or bytes."
  [fields field]
  (cond (and field (contains? fields field)) field
        (contains? fields :file/text) :file/text
        (contains? fields :file/bytes) :file/bytes))

(defn- with-byte-range
  "Fields with one text or bytes Field replaced by a page of its bytes; beside it k-byte-range,
   k-size and, when capped, k-range-truncated."
  [world fields field byte-range]
  (let [k (page-field fields field)
        text (when (and byte-range k) (text-of world (get-in fields [k :field/value])))]
    (if-not text
      fields
      (let [{:keys [text from to size truncated]} (byte-page text byte-range)
            f (fn [k type v] {:field/name k :field/type type :field/value v :field/reader :both})
            sib (fn [suffix] (keyword (namespace k) (str (name k) "-" suffix)))]
        (-> fields
            (assoc k (f k :string text)
                   (sib "byte-range") (f (sib "byte-range") [:list :number] [from to])
                   (sib "size") (f (sib "size") :number size))
            (cond-> truncated (assoc (sib "range-truncated") (f (sib "range-truncated") :boolean true))))))))

(defn- with-range
  "Fields with one text or bytes Field replaced by the requested lines (decisions 2026-10-30; world
   pass 2: any Field, not only a file's): the named field, else file/text or file/bytes. A file keeps
   the names file/text, file/range, file/lines, file/range-truncated; any other Field k gains
   k-range, k-lines and k-range-truncated beside the paged k."
  [world fields field range]
  (let [k (page-field fields field)
        text (when (and range k) (text-of world (get-in fields [k :field/value])))]
    (if-not text
      fields
      (let [{:keys [text from to of truncated]} (fs/lines-range text range)
            f (fn [k type v] {:field/name k :field/type type :field/value v :field/reader :both})
            file? (contains? #{:file/text :file/bytes} k)
            target (if file? :file/text k)
            sib (fn [suffix] (if file? (keyword "file" suffix) (keyword (namespace k) (str (name k) "-" suffix))))]
        (-> fields
            (cond-> file? (dissoc :file/bytes))
            (assoc target (f target :string text)
                   (sib "range") (f (sib "range") [:list :number] [from to])
                   (sib "lines") (f (sib "lines") :number of))
            (cond-> truncated (assoc (sib "range-truncated") (f (sib "range-truncated") :boolean true))))))))

(defn- local-path-err
  "A read of a path on this machine is no name at all, and a bare `unresolved` sent root looking for
   another world: say how to reach it. An absolute path, a ~ path, or <host>/<path> for this host."
  [db p name]
  (when (string? name)
    (let [wf (objects/fields db (objects/world-id db))
          hosts (keep #(get-in wf [% :field/value]) [:world/host :world/name])
          path (cond (or (str/starts-with? name "/") (str/starts-with? name "~/")) name
                     :else (some (fn [h] (when (str/starts-with? name (str h "/")) (str "/" (subs name (inc (count h)))))) hosts))]
      (when path
        (let [pf (objects/fields db p)
              shell (first (filter #(= :host (some-> (get-in (objects/fields db %) [:adapter/binds :field/value]) clojure.core/name keyword))
                                   (get-in pf [:principal/adapters :field/value])))
              shell-name (when shell (or (some second (objects/names db shell)) (str shell)))]
          (err :unresolved
               (if shell
                 (str name " is a path on this machine, not a name. A file under your shell's directory: run {cmd: \"cat " path "\"} through "
                      shell-name ". Anywhere else: mount it first with add_project {path: \"" path "\"}.")
                 (str name " is a path on this machine, not a name, and you have no shell Adapter. Mount it with add_project {path: \"" path "\"}, or ask for a shell."))))))))

(defn read!
  "The read op's work (spec 4.1, 4.3): :read on the object (by id, or by name through resolution;
   a mounted name materializes its Object, spec 6.5), the fold filtered by Field visibility, one
   `field-read` per Field returned (on the object's stream and the reader's). range selects lines
   of a text or bytes Field: the given field, else a file Object's file/text (a Task's
   task/stdout pages the same way)."
  [world p {:keys [object name field seq via range] :as args}]
  (let [db0 (db world)
        field (if (string? field) (keyword field) field) ; a model writes the name as text
        want-bytes? (not (false? (:bytes args)))]
    (if-let [{:keys [grant object]} (cond (resolve/public-field? db0 object field) {:object object}
                                          object (resolve/resolve! world db0 p (id-name object) :read)
                                          name (resolve/resolve! world db0 p name :read))]
      (let [db (db world) ; after a mount materialized, the index has caught up
            ;; a fold of one stream is only the state at seq if the stream's start is still there
            past-pruned? (and seq (< (or (:object/created-seq (objects/entity db object)) 0)
                                     (log/first-seq (:log world)))
                              (> (log/first-seq (:log world)) 1))
            state (cond past-pruned? nil
                        seq (objects/state-at (partial log/read-stream (:log world)) object seq)
                        :else (objects/state db object))
            fields (refs/visible-fields db p (resolve/visible-fields db p (:object/fields state) :read))
            fields (if field (select-keys fields [field]) fields)
            read-keys (keys fields)
            ;; a page of bytes that are gone is an answer of its own, not the receipt as if it were text
            page-pruned? (when (or range (:byte-range args))
                           (when-let [k (page-field fields field)]
                             (pruned-blob? world (get-in fields [k :field/value]))))
            fields (with-range world fields field range)
            fields (with-byte-range world fields field (:byte-range args))
            ;; what the bytes Fields weigh, so a client can skip a large payload; with
            ;; bytes false the payloads stay out and only this summary says what is there
            blobs (into {} (for [[k {:keys [field/value]}] fields
                                 :let [summary (cond (bytes? value) {:size (alength ^bytes value) :sha256 (blob/sha256-hex value) :inline true}
                                                     (blob/receipt? value) (cond-> {:size (:blob/size value) :sha256 (:blob/sha256 value)}
                                                                             (pruned-blob? world value) (assoc :pruned true)))]
                                 :when summary]
                             [k (let [mime (or (get-in fields [(keyword (namespace k) "mime") :field/value])
                                               (get-in fields [:file/mime :field/value]))]
                                  (cond-> summary (string? mime) (assoc :mime mime)))]))
            fields (if want-bytes?
                     fields
                     (into {} (map (fn [[k f]] [k (if (bytes? (:field/value f))
                                                   (assoc f :field/value {:blob/sha256 (blob/sha256-hex (:field/value f)) :blob/size (alength ^bytes (:field/value f))})
                                                   f)])) fields))]
        (cond
          past-pruned? (err :pruned (str "the log no longer goes back to seq " seq ": it starts at " (log/first-seq (:log world))))
          page-pruned? (err :pruned pruned-message)
          (and field (empty? fields)) unresolved
          :else
          (do (when (not-empty read-keys)
                (append! world (log/pending :field-read object p (cond-> {:names (vec read-keys) :by p} via (assoc :via via))
                                            :streams #{p} :delegation grant :via via)))
              {:ok (cond-> (assoc state :object/fields fields)
                     (pos? (count blobs)) (assoc :object/blobs blobs))})))
      (or (local-path-err db0 p name) unresolved))))

(def max-copy-chars "The most a from-copy stores as one Field." (* 256 1024))

(def ^:private default-source-fields [:file/text :file/bytes :task/stdout])

(defn copy-source
  "What set-field's from names, read as p reads anything: {:ok {:value :type}} or {:err}. A text or
   bytes value comes back as a string, a page of it with range; any other value as it stands. Without
   a field, a file's text or a Task's stdout."
  [world p {:keys [object name field range]} via]
  (let [{:keys [ok err]} (read! world p (cond-> {:via via}
                                          object (assoc :object object)
                                          name (assoc :name name)
                                          field (assoc :field field)))]
    (if err
      {:err err}
      (let [fields (:object/fields ok)
            k (or (when (contains? fields field) field)
                  (some #(when (contains? fields %) %) default-source-fields))
            f (get fields k)
            text (when f (text-of world (:field/value f)))
            text (if (and text range) (:text (fs/lines-range text range)) text)]
        (cond
          (nil? f) {:err {:code :unresolved :message (str "no Field " (or field "to copy") " there you can read")}}
          (pruned-blob? world (:field/value f)) (err :pruned pruned-message)
          (and text (> (count text) max-copy-chars))
          {:err {:code :invalid :message (str (count text) " chars is more than one Field holds; copy a range")}}
          text {:ok {:value text :type :string}}
          :else {:ok {:value (:field/value f) :type (:field/type f)}})))))

;; ---------------------------------------------------------------- tasks

(defn move!
  "`task-moved` (spec 5.3): :write on the Task; from is its present state. The event also lands on
   the owner's stream, as `evaluated` lands on an initiator's. Returns {:ok {:seq}}."
  [world p {:keys [task to evidence via]}]
  (let [db (db world)
        s (objects/state db task)]
    (if (not= :task (:object/class s))
      unresolved
      (if-let [{:keys [grant]} (resolve/resolve! world db p (id-name task) :write)]
        (let [from (get-in s [:object/fields :task/state :field/value] :open)
              owner (get-in s [:object/fields :task/owner :field/value])
              body (cond-> {:task task :from from :to to} (seq evidence) (assoc :evidence (set evidence)))
              seq (append! world (log/pending :task-moved task p body :streams (if owner #{owner} #{}) :delegation grant :via via))]
          {:ok {:seq seq}})
        unresolved))))

;; ---------------------------------------------------------------- grants

(defn- filter-within?
  "Is filtered scope a within filtered scope b? Every constraint b names, a names at least as tightly."
  [a b]
  (let [under? (fn [k] (or (nil? (k b)) (and (k a) (str/starts-with? (str (k a)) (str (k b))))))]
    (and (under? :prefix) (under? :field-prefix)
         (or (nil? (:class b)) (= (some-> (:class a) name) (name (:class b)))))))

(defn delegation-for
  "The grantor's delegable grant that covers the requested scope with every requested op
   (plus :delegate), or nil. Spec 5.9, 6.2."
  [db grantor scope ops]
  (let [holders (conj (resolve/groups-of db grantor) grantor)
        wanted (conj (set ops) :delegate)
        candidates (filter #(and (:grant/delegable %) (every? (:grant/ops %) wanted))
                           (resolve/active-grants db holders (java.util.Date.)))
        covers-scope? (fn [g]
                        (let [gs (:grant/scope g)]
                          (cond (= gs :everything) true
                                (= scope :everything) false
                                (set? scope) (and (set? gs) (every? gs scope))
                                (map? scope) (and (map? gs) (filter-within? scope gs))
                                :else false)))]
    (some->> candidates (filter covers-scope?) first :object/id)))

(def grant-ops
  "The Grant vocabulary (spec 5.9): not the wire ops."
  #{:read :reference :transmit :write :delegate :evaluate :create})

(defn invalid-ops
  "err invalid-ops naming the stray words, or nil when every op is a Grant op."
  [ops]
  (let [bad (remove grant-ops (map #(if (keyword? %) % (keyword (str %))) ops))]
    (when (or (empty? ops) (seq bad))
      (err :invalid-ops (str (if (empty? ops) "ops is empty" (str "not Grant ops: " (str/join ", " (map name bad))))
                             "; Grant ops are read, reference, transmit, write, delegate, evaluate, create (wire ops such as history or set-field are not Grant ops)")))))

(defn grant-resolves?
  "Could p issue this Grant now? The loop asks before the gate: a person never approves a Grant that
   cannot run (decisions 2026-10-31)."
  [db p {:keys [grantee scope ops]}]
  (boolean (and (objects/exists? db grantee) (delegation-for db p scope ops))))

(defn grant!
  "A Grant from p to grantee (spec 5.9): `created` for the Grant Object, then `granted` on the
   grantor's, grantee's and the Grant's streams, under p's delegable grant covering the scope.
   Ops outside the Grant vocabulary answer err invalid-ops."
  [world p {:keys [grantee scope ops delegable until]}]
  (let [db (db world)
        delegation (when (objects/exists? db grantee) (delegation-for db p scope ops))]
    (cond
      (invalid-ops ops) (invalid-ops ops)
      (and (map? scope) (not (some scope [:class :prefix :field-prefix])))
      (err :invalid "a filtered scope names class, prefix or field-prefix")
      (not delegation)
      unresolved
      :else
      (let [grant (random-uuid)
            fields (cond-> {:grant/grantor p :grant/grantee grantee :grant/scope scope :grant/ops (set ops)
                            :grant/delegable (boolean delegable)}
                     until (assoc :grant/until until))]
        (append! world (log/pending :created grant p {:class :grant :fields fields} :delegation delegation))
        (let [seq (append! world (log/pending :granted grant p
                                              (cond-> {:grant grant :grantee grantee :ops (set ops) :scope scope
                                                       :delegable (boolean delegable)}
                                                until (assoc :until until))
                                              :streams (hash-set p grantee) :delegation delegation))]
          {:ok {:grant grant :seq seq}})))))

(defn revoke! [world p {:keys [grant reason]}]
  (let [db (db world)
        g (objects/state db grant)]
    (if (or (nil? (:grant/grantee g)) (:grant/revoked g))
      unresolved
      (let [permit (or (when (= p (:grant/grantor g)) grant)
                       (:grant (resolve/resolve! world db p (id-name grant) :delegate)))]
        (if-not permit
          unresolved
          {:ok {:seq (append! world (log/pending :revoked grant p (cond-> {:grant grant} reason (assoc :reason reason))
                                                 :streams #{(:grant/grantor g) (:grant/grantee g)} :delegation permit))}})))))
