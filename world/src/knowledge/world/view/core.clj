(ns knowledge.world.view.core
  "Views (plan §19 W11; spec 4.5 'a View is a Serialization whose model is a listener', principle
   8 'the client owns legibility'). A view is a Serialization Object with serialization/kind :view,
   view/kind (text | table | tree | graph | image), view/title, view/data (a map; inline when its
   JSON is at most blob/threshold bytes, else a blob receipt of the JSON, as any bytes Field),
   view/about (the Objects it was derived from, for provenance) and view/truncated when the data
   was clipped. `present!` creates it and sends a Message of kind :view naming it on a Route, text
   = the title, so a client that cannot render still sees something. The person is the audience:
   the serializer renders a view to a model as one line (knowledge.world.serialize.core).

   `table-of` and `graph-of` build the two views a model asks for most without typing the data:
   a table of the current Fields of some Objects, and the graph of what an Object references.
   Resolution: :create on schema.Serialization for the view, :transmit on the Route for the
   Message (route/send!), :read on every Object a table or graph draws from (ground rule 4)."
  (:require [clojure.string :as str]
            [clojure.walk :as walk]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.protocol.codec :as codec]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]))

(def kinds #{:text :table :tree :graph :image :tex :pane})
(def max-rows 500)
(def max-nodes 200)
(def max-edges 600)
(def max-image-bytes (* 256 1024))
(def value-cap 200)

(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- kw [v] (cond (keyword? v) v (string? v) (keyword v) :else nil))

(defn- keywordize
  "Map keys as keywords, recursively (the JSON form carries them as strings)."
  [v]
  (walk/postwalk (fn [x] (if (map? x) (into {} (map (fn [[k v]] [(if (string? k) (keyword k) k) v])) x) x)) v))

(defn name-of
  "The name an Object is known by: in the world namespace first, else its own namespace, else the short id."
  [db id]
  (let [wid (objects/world-id db)
        names (objects/names db id)]
    (or (some (fn [[ns n]] (when (= ns wid) n)) (sort-by second names))
        (some (fn [[ns n]] (when (= ns id) n)) (sort-by second names))
        (some (fn [[_ n]] n) (sort-by second names))
        (subs (str id) 0 8))))

;; ---------------------------------------------------------------- data shapes

(defn- fail [code message] (act/err code message))

(defn normalize-data
  "The data of a view in its kind's shape, clipped to the caps. Returns {:data :truncated} or {:err}."
  [kind data]
  (let [d (keywordize data)]
    (case kind
      :text (if (string? (:markdown d))
              {:data {:markdown (:markdown d)} :truncated false}
              (fail :invalid-args "text data is {markdown string}"))
      :table (let [{:keys [columns rows]} d]
               (cond
                 (not (and (sequential? columns) (every? map? columns) (every? :key columns)))
                 (fail :invalid-args "table data is {columns [{key label align?}], rows [[...]]}: columns need a key")
                 (not (and (sequential? rows) (every? sequential? rows)))
                 (fail :invalid-args "table data is {columns [{key label align?}], rows [[...]]}: rows are vectors")
                 :else
                 (let [cols (mapv (fn [c] (cond-> {:key (str (:key c)) :label (str (or (:label c) (:key c)))}
                                            (:align c) (assoc :align (str (:align c)))))
                                  columns)
                       over? (> (count rows) max-rows)
                       rows (mapv (fn [r] (mapv (fn [v] (if (nil? v) "" v)) r)) (take max-rows rows))]
                   {:data (cond-> {:columns cols :rows rows} over? (assoc :total (count (:rows d)))) :truncated over?})))
      :tree (if (and (map? (:root d)) (some? (get-in d [:root :label])))
              {:data {:root (:root d)} :truncated false}
              (fail :invalid-args "tree data is {root {label children [...] ref?}}"))
      :graph (let [{:keys [nodes edges]} d]
               (if (and (sequential? nodes) (every? map? nodes) (every? :id nodes) (sequential? (or edges [])))
                 (let [over? (or (> (count nodes) max-nodes) (> (count edges) max-edges))
                       nodes (vec (take max-nodes nodes))
                       ids (set (map #(str (:id %)) nodes))
                       edges (vec (take max-edges (filter (fn [e] (and (ids (str (:from e))) (ids (str (:to e))))) edges)))]
                   {:data {:nodes nodes :edges edges} :truncated over?})
                 (fail :invalid-args "graph data is {nodes [{id label class? ref?}], edges [{from to label?}]}")))
      :image (cond
               (blob/receipt? (:bytes d)) {:data {:bytes (:bytes d) :mime (str (or (:mime d) "image/png"))} :truncated false}
               (and (string? (:base64 d)) (string? (:mime d)))
               (if (> (count (:base64 d)) (* 4/3 max-image-bytes))
                 (fail :invalid-args "image is larger than 256 KiB; put it in a blob first")
                 {:data {:base64 (:base64 d) :mime (:mime d)} :truncated false})
               :else (fail :invalid-args "image data is {bytes <blob receipt>} or {mime, base64 <= 256 KiB}"))
      ;; TeX source; the client typesets it, so every client shows math with no model on the host
      :tex (if (string? (:source d))
             {:data {:source (:source d)} :truncated false}
             (fail :invalid-args "tex data is {source string}"))
      ;; a live pane delivered to its asker; the client opens it
      :pane (if (string? (:pane d))
              {:data (select-keys d [:pane :kind]) :truncated false}
              (fail :invalid-args "pane data is {pane id, kind}"))
      (fail :invalid-args (str "kind must be one of " (str/join ", " (map name (sort kinds))))))))

;; ---------------------------------------------------------------- the view and its Message

(defn present!
  "Create the view Serialization and send it as a Message of kind :view on route to the recipients.
   args {:to #{ids} :route id :kind :title :data :about [ids] :in-reply-to id :via :hop}.
   Returns {:ok {:serialization id :message id :seq :truncated}} or {:err}."
  [world p {:keys [to route kind title data about in-reply-to via hop device session title-refs data-refs]}]
  (let [db (act/db world)
        kind (kw kind)
        to (set (keep ->uuid (if (coll? to) to [to])))
        route (->uuid route)
        about (vec (keep ->uuid about))]
    (cond
      (not (kinds kind)) (fail :invalid-args (str "kind must be one of " (str/join ", " (map name (sort kinds)))))
      (not (string? title)) (fail :invalid-args "title is a string")
      (nil? (route/route-state db route)) act/unresolved
      (empty? to) (fail :invalid-args "to names at least one Principal")
      :else
      (let [{:keys [grant]} (resolve/resolve! world db p (act/schema-name :serialization) :create)]
        (if-not grant
          act/unresolved
          (let [{:keys [err] :as nd} (normalize-data kind data)]
            (if err
              nd
              (let [{:keys [data truncated]} nd
                    text (j/write-value-as-string (codec/plain data))
                    bs (.getBytes ^String text "UTF-8")
                    stored (if (> (alength bs) blob/threshold) (blob/put! (:blobs world) bs) data)
                    wid (objects/world-id db)
                    model (objects/lookup db wid "transport.listener")
                    id (random-uuid)
                    fields (cond-> {:serialization/kind :view
                                    :view/kind kind
                                    :view/title title
                                    :view/data stored
                                    :view/about about
                                    :serialization/working-set (set about)
                                    :serialization/depth {}
                                    :serialization/format :view
                                    :serialization/bytes (if (blob/receipt? stored) stored text)
                                    :serialization/size {:bytes (alength bs)}
                                    :serialization/hash (blob/sha256-hex bs)
                                    :serialization/as-of {}}
                             model (assoc :serialization/model model)
                             truncated (assoc :view/truncated true)
                             (seq title-refs) (assoc :view/title/refs (vec title-refs))
                             (seq data-refs) (assoc :view/data/refs (vec data-refs)))
                    _ (act/append! world (log/pending :created id p {:class :serialization :fields fields}
                                                      :via via :delegation grant :streams (set about)))
                    {:keys [ok err]} (route/send! world p {:to to :route route :text title :refs title-refs :in-reply-to in-reply-to
                                                           :via via :hop hop :kind :view :view id
                                                           :device device :session session})]
                (if err
                  {:err err}
                  {:ok {:serialization id :message (:message ok) :seq (:seq ok) :truncated (boolean truncated)
                        :kind kind :title title :bytes (alength bs)}})))))))))

;; ---------------------------------------------------------------- the two "show me" builders

(defn- cell
  "A Field value as a table cell: names for references, text clipped, numbers as they are."
  [db v]
  (cond
    (nil? v) ""
    (uuid? v) (name-of db v)
    (and (coll? v) (seq v) (every? uuid? v)) (str/join ", " (map #(name-of db %) (if (set? v) (sort-by str v) v)))
    (keyword? v) (name v)
    (number? v) v
    (boolean? v) v
    (string? v) (if (> (count v) value-cap) (str (subs v 0 value-cap) "…") v)
    (bytes? v) (str "[bytes " (alength ^bytes v) "]")
    (blob/receipt? v) (str "[bytes " (:blob/size v) "]")
    (instance? java.util.Date v) (str (.toInstant ^java.util.Date v))
    :else (let [s (j/write-value-as-string (codec/plain v))] (if (> (count s) value-cap) (str (subs s 0 value-cap) "…") s))))

(defn table-of
  "A table view's data from the current Fields of objects the caller can read: one row per Object,
   columns name, class, then each requested Field. Returns {:data :about :skipped}."
  [db p {:keys [objects fields class]}]
  (let [ids (if (and (empty? objects) class)
              (vec (sort-by str (remove #(objects/retired? db %) (objects/objects-of-class db (keyword (name class))))))
              (vec (keep ->uuid objects)))
        ks (mapv (fn [f] (let [s (str f)] (keyword (if (str/starts-with? s ":") (subs s 1) s)))) fields)
        readable (filterv (fn [o] (and (objects/exists? db o) (resolve/resolves? db p (act/id-name o) :read))) ids)
        rows (vec (for [o readable
                        :let [state (objects/state db o)
                              visible (resolve/visible-fields db p (:object/fields state) :read)]]
                    (into [(name-of db o) (name (:object/class state))]
                          (map (fn [k] (cell db (get-in visible [k :field/value]))) ks))))
        columns (into [{:key "name" :label "name"} {:key "class" :label "class"}]
                      (map (fn [k] {:key (subs (str k) 1) :label (subs (str k) 1)}) ks))]
    {:data {:columns columns :rows rows} :about readable :skipped (vec (remove (set readable) ids))}))

(defn- ref-fields
  "[[field-name target] ...] for every reference a readable Object's Fields hold."
  [db p o]
  (let [visible (resolve/visible-fields db p (:object/fields (objects/state db o)) :read)]
    (for [[k {:keys [field/value]}] (sort-by (fn [[k _]] (str k)) visible)
          :when (not (#{"serialization" "view"} (namespace k)))
          t (cond (uuid? value) [value]
                  (and (coll? value) (every? uuid? value)) (if (set? value) (sort-by str value) value)
                  :else [])
          :when (objects/exists? db t)]
      [k t])))

(defn graph-of
  "A graph view's data: the Object, what it references through ref Fields (edges labelled by the
   Field name), and at depth 2 what those reference in turn. Only Objects the caller can read.
   Returns {:data :about :truncated}."
  [db p {:keys [root depth]}]
  (let [start (->uuid root)
        depth (max 1 (min 2 (long (or depth 1))))
        readable? (fn [o] (and o (objects/exists? db o) (resolve/resolves? db p (act/id-name o) :read)))]
    (if-not (readable? start)
      {:err (:err act/unresolved)}
      (loop [frontier [start] seen #{start} edges [] d 0]
        (if (or (>= d depth) (empty? frontier) (>= (count seen) max-nodes))
          (let [nodes (vec (for [o (sort-by str seen)
                                 :let [s (objects/state db o)]]
                             {:id (str o) :label (name-of db o) :class (name (:object/class s)) :ref (str o)}))
                truncated (or (>= (count seen) max-nodes) (> (count edges) max-edges))]
            {:data {:nodes (vec (take max-nodes nodes)) :edges (vec (take max-edges edges))}
             :about (vec (sort-by str seen)) :truncated truncated})
          (let [step (for [o frontier [k t] (ref-fields db p o) :when (readable? t)] [o k t])
                new-edges (vec (for [[o k t] step] {:from (str o) :to (str t) :label (subs (str k) 1)}))
                targets (distinct (map last step))
                fresh (vec (remove seen targets))]
            (recur (vec (take (- max-nodes (count seen)) fresh))
                   (into seen (take (- max-nodes (count seen)) fresh))
                   (into edges new-edges)
                   (inc d))))))))

;; ---------------------------------------------------------------- for the serializer

(defn summary
  "One line for a view: its kind, title and size, never its data."
  [db state]
  (let [v (fn [k] (get-in state [:object/fields k :field/value]))
        kind (some-> (v :view/kind) name)
        data (v :view/data)
        data (if (blob/receipt? data) nil data)
        size (cond
               (nil? data) (str (get-in state [:object/fields :serialization/size :field/value :bytes]) " bytes")
               (= kind "table") (str (count (:columns data)) " columns × " (count (:rows data)) " rows"
                                     (when-let [t (:total data)] (str " of " t)))
               (= kind "graph") (str (count (:nodes data)) " nodes, " (count (:edges data)) " edges")
               (= kind "text") (str (count (str (:markdown data))) " chars")
               (= kind "tree") "a tree"
               (= kind "image") (str (:mime data))
               :else "")]
    (str "view " kind " \"" (v :view/title) "\": " size
         (when (v :view/truncated) ", truncated")
         (when-let [a (seq (v :view/about))] (str "; about " (str/join ", " (map #(name-of db %) (take 6 a)))
                                                  (when (> (count a) 6) (str " and " (- (count a) 6) " more")))))))
