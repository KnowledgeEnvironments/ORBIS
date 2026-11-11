(ns knowledge.world.manual
  "The built-in manual (decisions 2026-10-31): a part of the world that stores documentation, for people
   and agents alike, modelled on the Lisp machine's document examiner. Topics are records, not files. Each is
   an Object in the world's manual collection, its events on the collection's stream, so the manual is
   versioned in the log and a world's copy upgrades with its kernel.

   A topic is seeded from what ships: docs/ split at its headings (resources/.../manual/sources.edn),
   concept topics, and generated topics for every action, class and Field, so these cannot drift from
   the kernel. The world is the author. A topic no longer shipped is marked topic/retired, not deleted.

   Every Principal reads it, through the public doc.* actions: doc.show, doc.candidates, doc.overview,
   doc.contents. No manual text is in any prompt: an agent gets one pointer line, and asks."
  (:require [clojure.edn :as edn]
            [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.schema.classes :as classes]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]))

(def collection-field :world/manual)
(def section-limit "A section longer than this splits at its `###` headings." 6000)
(def default-limit 12)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- resource [path] (some-> (io/resource (str "knowledge/world/manual/" path)) slurp))

(defn- page
  "A registered page's text. A page that is not there stops the manual: B3 told a person its page was
   registered, and the book it named shipped nothing."
  [book file]
  (or (resource file)
      (throw (ex-info (str "manual: book " book " registers " file ", which does not exist") {:book book :file file}))))

;; ---------------------------------------------------------------- what ships

(defn slug [s]
  (-> (str/lower-case (str s)) (str/replace #"`" "") (str/replace #"[^a-z0-9]+" "-") (str/replace #"^-|-$" "")))

(defn split-at-headings
  "A Markdown document's parts at heading level: [{:heading :lines}], the part before the first heading
   with :heading nil. A heading inside a code fence is not one."
  [lines level]
  (let [marker (str (apply str (repeat level "#")) " ")]
    (first
      (reduce (fn [[parts fence?] line]
                (let [fence? (if (str/starts-with? line "```") (not fence?) fence?)]
                  (if (and (not fence?) (str/starts-with? line marker))
                    [(conj parts {:heading (str/trim (subs line (count marker))) :lines []}) fence?]
                    [(update-in parts [(dec (count parts)) :lines] conj line) fence?])))
              [[{:heading nil :lines []}] false]
              lines))))

(defn- body-of [lines] (str/trim (str/join "\n" lines)))

(defn- sections
  "The topics of one document."
  [{:keys [book title file]}]
  (let [text (page book file)
        lines (str/split-lines text)
        lead (first (filter #(str/starts-with? % "# ") lines))
        parts (split-at-headings (remove #(= % lead) lines) 2)
        topic (fn [n t lines] {:name n :title t :type "section" :format "markdown" :book book :body (body-of lines)})
        expand (fn [{:keys [heading lines]}]
                 (let [n (str book "/" (slug heading))]
                   (if (<= (count (str/join "\n" lines)) section-limit)
                     [(topic n heading lines)]
                     (let [[intro & subs*] (split-at-headings lines 3)]
                       (into [(topic n heading (:lines intro))]
                             (map (fn [{sub :heading sl :lines}] (topic (str n "/" (slug sub)) (str heading ": " sub) sl)))
                             subs*)))))]
    (into [(topic book title (:lines (first parts)))] (mapcat expand) (rest parts))))

(defn- concepts [{:keys [book concepts]}]
  (for [{:keys [name file format]} concepts
        :let [text (page book file)
              lines (str/split-lines text)
              heading (first lines)
              titled? (str/starts-with? (str heading) "# ")]]
    {:name name :type "concept" :format (or format "markdown") :book book
     :title (if titled? (subs heading 2) (str/capitalize (str/replace (last (str/split name #"/")) "-" " ")))
     :body (body-of (if titled? (rest lines) lines))}))

(defn- action-topic [{:keys [name hint schema instructions gated public]}]
  {:name (str "action/" name) :title name :type "action" :format "markdown" :book "actions"
   :body (str hint ".\n\n" instructions "\n\n"
              "Called with `act {action \"" name "\", args {...}}`.\n\n"
              (when gated "Gated: it waits for a person's card.\n\n")
              (when public "Every Principal may call it.\n\n")
              "Arguments:\n\n```\n" (pr-str schema) "\n```")})

(def class-schemas
  {"principal" classes/Principal "group" classes/Group "message" classes/Message "task" classes/Task
   "evaluation" classes/Evaluation "trigger" classes/Trigger "model" classes/Model "route" classes/Route
   "grant" classes/Grant "adapter" classes/Adapter "serialization" classes/Serialization})

(defn- entries
  "A class schema's Fields, [name form]. A class is a :map or a merge of them."
  [form]
  (when (sequential? form)
    (cond
      (= :map (first form)) (for [e (rest form) :when (and (vector? e) (qualified-keyword? (first e)))]
                              [(first e) (last e)])
      (= :merge (first form)) (mapcat entries (rest form))
      :else nil)))

(defn- class-topics []
  (for [[c schema] class-schemas
        :let [fields (entries schema)]]
    {:name (str "class/" c) :title c :type "class" :format "markdown" :book "classes"
     :body (str "The class " c ". Its Fields:\n\n"
                (str/join "\n" (for [[k form] fields] (str "- `" (subs (str k) 1) "`: `" (pr-str form) "`"))))}))

(defn- field-topics [catalogue]
  (let [from-classes (for [[c schema] class-schemas [k form] (entries schema)]
                       [(subs (str k) 1) (str "A Field of the class " c ", of type `" (pr-str form) "`.")])
        described (into (into {} from-classes) catalogue)]
    (for [[n text] described]
      {:name (str "field/" n) :title n :type "field" :format "markdown" :book "fields" :body text})))

(defn- ordered
  "Book order: docs and concepts in their sources order, then actions, classes, Fields."
  [topics]
  (vec (map-indexed (fn [i t] (assoc t :order i)) topics)))

(defn book-topics
  "A book's topics: its document's and its concept pages'. A book may have both."
  [b]
  (concat (when (:file b) (sections b)) (concepts b)))

(def shipped
  "Every topic this kernel ships, with its hash."
  (delay
    (let [{:keys [books fields]} (edn/read-string (resource "sources.edn"))
          docs (mapcat book-topics books)
          generated (concat (sort-by :name (map action-topic actions/definitions))
                            (sort-by :name (class-topics))
                            (sort-by :name (field-topics fields)))]
      {:books (mapv #(select-keys % [:book :title]) (concat books [{:book "actions" :title "Actions"}
                                                                   {:book "classes" :title "Classes"}
                                                                   {:book "fields" :title "Fields"}]))
       :topics (mapv (fn [t] (assoc t :hash (blob/sha256-hex (.getBytes (str (:title t) "\n" (:format t) "\n" (:body t)) "UTF-8"))))
                     (ordered (concat docs generated)))})))

;; ---------------------------------------------------------------- in the world

(defn collection [db] (field db (objects/world-id db) collection-field))

(defn topics-in
  "The live topic Objects, {name id}."
  [db]
  (into {} (for [id (objects/objects-with-field db :topic/name)]
             [(field db id :topic/name) id])))

(defn- fields-of [{:keys [name title type format book order body hash]}]
  {:topic/name name :topic/title title :topic/type type :topic/format format :topic/book book
   :topic/order order :topic/body body :topic/hash hash})

(defn- field-type [k] (if (= :topic/order k) :number :string))

(defn ensure!
  "The world's manual matches this kernel's: a new topic is created, a changed one rewritten, one no
   longer shipped marked retired. The world is the actor. Returns {:created :changed :retired}."
  [{:keys [log world-id] :as world}]
  (let [db (act/db world)
        coll (collection db)
        have (topics-in db)
        ship (:topics @shipped)
        names (set (map :name ship))
        put! (fn [id k v] (log/append! log (log/pending :field-set id world-id
                                                        {:name k :type (field-type k) :value v :reader :both}
                                                        :streams #{coll})))
        creates (remove #(have (:name %)) ship)
        changes (for [t ship :let [id (have (:name t))]
                      :when (and id (or (not= (:hash t) (field db id :topic/hash))
                                        (not= (:order t) (field db id :topic/order))
                                        (field db id :topic/retired)))]
                  [id t])
        retires (for [[n id] have :when (and (not (names n)) (not (field db id :topic/retired)))] id)]
    (doseq [t creates]
      (log/append! log (log/pending :created (random-uuid) world-id {:class :object :fields (fields-of t)} :streams #{coll})))
    (doseq [[id t] changes [k v] (fields-of t) :when (not= v (field db id k))]
      (put! id k v))
    (doseq [[id _] changes :when (field db id :topic/retired)]
      (log/append! log (log/pending :field-set id world-id {:name :topic/retired :type :boolean :value false :reader :both}
                                    :streams #{coll})))
    (doseq [id retires]
      (log/append! log (log/pending :field-set id world-id {:name :topic/retired :type :boolean :value true :reader :both}
                                    :streams #{coll})))
    ;; the group commit has it all once this one lands
    (when (or (seq creates) (seq changes) (seq retires))
      (log/append!! log (log/pending :field-set coll world-id {:name :manual/version :type :string
                                                               :value (blob/sha256-hex (.getBytes (str/join (map :hash ship)) "UTF-8"))
                                                               :reader :both})))
    {:created (count creates) :changed (count changes) :retired (count retires)}))

;; ---------------------------------------------------------------- reading

(defn- row [db id]
  {:topic (field db id :topic/name) :title (field db id :topic/title) :type (field db id :topic/type)
   :book (field db id :topic/book)})

(defn- live [db]
  (->> (topics-in db)
       (remove (fn [[_ id]] (field db id :topic/retired)))
       (sort-by (fn [[_ id]] (long (or (field db id :topic/order) 0))))))

(defn- words [s] (remove str/blank? (str/split (str/lower-case (str s)) #"[^a-z0-9]+")))

(defn- snippet
  "The line of a body the text best shows in."
  [body ts]
  (let [lines (remove str/blank? (str/split-lines (str body)))
        hit (first (filter (fn [l] (let [low (str/lower-case l)] (some #(str/includes? low %) ts))) lines))
        l (str/trim (str (or hit (first lines))))]
    (if (> (count l) 120) (str (subs l 0 120) "...") l)))

(defn- score
  "How well a topic answers the words. The name and title count far above the body."
  [db id ts]
  (let [n (str/lower-case (field db id :topic/name))
        t (str/lower-case (str (field db id :topic/title)))
        b (str/lower-case (str (field db id :topic/body)))
        hits (fn [hay w] (count (re-seq (re-pattern (java.util.regex.Pattern/quote w)) hay)))]
    (reduce + 0 (for [w ts]
                  (+ (if (str/includes? n w) 8 0)
                     (if (str/includes? t w) 12 0)
                     (min 6 (hits b w)))))))

(defn candidates
  "Show Candidates: the topics that answer text, best first. {:candidates [...] :total n}."
  [db {:keys [text type limit]}]
  (let [ts (vec (words text))
        scored (for [[_ id] (live db)
                     :when (or (nil? type) (= (str type) (field db id :topic/type)))
                     :let [s (if (seq ts) (score db id ts) 1)]
                     :when (pos? s)
                     ;; every word matters: a topic with all of them comes before one with most
                     :let [all? (every? #(pos? (score db id [%])) ts)]]
                 [(if all? 1 0) s id])
        best (sort-by (fn [[a s id]] [(- a) (- s) (long (or (field db id :topic/order) 0))]) scored)]
    {:candidates (mapv (fn [[_ _ id]] (assoc (row db id) :snippet (snippet (field db id :topic/body) ts)))
                       (take (long (or limit default-limit)) best))
     :total (count best)}))

(defn find-topic
  "The topic id a name means: exact, then by title, then under action/, field/, class/, or nil."
  [db topic]
  (let [t (str/trim (str topic))
        all (live db)
        by-name (into {} all)]
    (or (by-name t)
        (by-name (str "action/" t)) (by-name (str "field/" t)) (by-name (str "class/" t))
        ;; a guess at the kind, such as actions/token.issue
        (let [bare (last (str/split t #"/"))] (or (by-name (str "action/" bare)) (by-name (str "class/" bare))))
        (some (fn [[_ id]] (when (= (str/lower-case t) (str/lower-case (str (field db id :topic/title)))) id)) all))))

(defn- neighbours
  "The topics either side of one in its book, for reading on."
  [db id]
  (let [book (field db id :topic/book)
        rows (vec (map second (filter (fn [[_ i]] (= book (field db i :topic/book))) (live db))))
        at (.indexOf ^java.util.List rows id)]
    (cond-> {}
      (pos? at) (assoc :previous (field db (rows (dec at)) :topic/name))
      (< -1 at (dec (count rows))) (assoc :next (field db (rows (inc at)) :topic/name)))))

(defn- missing
  "No such topic is an answer, not a failure: what is closest."
  [db topic]
  {:ok {:result {:found false :topic (str topic) :candidates (:candidates (candidates db {:text topic :limit 5}))}}})

(defn show
  "Show Documentation: a topic whole."
  [world _p {:keys [topic]}]
  (let [db (act/db world)]
    (if-let [id (find-topic db topic)]
      {:ok {:result (merge (row db id) {:format (field db id :topic/format) :body (field db id :topic/body)}
                           (neighbours db id))}}
      (missing db topic))))

(defn- first-paragraph [body]
  (first (remove str/blank? (str/split (str body) #"\n\s*\n"))))

(defn overview
  "Show Overview: a topic in a paragraph, or the whole manual as its books."
  [world _p {:keys [topic]}]
  (let [db (act/db world)]
    (cond
      (nil? topic)
      {:ok {:result {:books (vec (for [{:keys [book title]} (:books @shipped)]
                                   {:book book :title title
                                    :topics (count (filter (fn [[_ id]] (= book (field db id :topic/book))) (live db)))}))}}}
      :else
      (if-let [id (find-topic db topic)]
        {:ok {:result (merge (row db id) {:overview (first-paragraph (field db id :topic/body))} (neighbours db id))}}
        (missing db topic)))))

(defn contents
  "Show Table of Contents: every topic of a book, or the books."
  [world p {:keys [book]}]
  (let [db (act/db world)]
    (if (nil? book)
      (overview world p {})
      {:ok {:result {:book (str book)
                     :topics (vec (for [[_ id] (live db) :when (= (str book) (field db id :topic/book))]
                                    (row db id)))}}})))
