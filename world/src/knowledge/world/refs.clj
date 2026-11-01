(ns knowledge.world.refs
  "References, one implementation (decisions 2026-10-31). A reference is structure beside the text,
   never markup in it: text a person reads holds the display text, and `<field>/refs` beside it holds
   spans {:start :end :kind :object :field :seq :name}, offsets in Unicode code points of the text,
   kind :object, :field, :seq, or :path (a file on the mount :object, at :path, cited at :line): a
   path is marked where the world found the file, knowledge.world.code/cited-paths. `message/text` keeps `message/refs` by name (refs-key is the one rule).

   In a prompt every id is a handle: the shortest hex prefix unique within the Serialization, two
   characters at least, longer on collision (handles). A wake reuses its table, so a handle means
   one thing from the first iteration to the last and nothing after it.

   On the way in, parse turns what a model wrote into display text and spans through the table of
   the Evaluation that wrote it: [[a1]], [[a1|anything]], [[a1.field]], [[seq:680]]; an unknown handle
   stays as it was written and is listed. Fenced and inline code are literal. On the way out, render
   turns stored spans back into references, ids in full, which the Serialization then shortens to
   the current table; a retired or unreadable target renders as its plain name. Nothing that reads
   a Field or an event for a reader hands over a span the reader cannot resolve (visible-*): the
   display text was already visible, the link is not."
  (:require [clojure.string :as str]
            [datascript.core :as d]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.resolve.core :as resolve]))

(def ^:private uuid-pattern "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
(def uuid-re (re-pattern uuid-pattern))
(def whole-uuid-re (re-pattern (str "^" uuid-pattern "$")))
(def handle-like-re "What might be a handle where an id belongs." #"^[0-9a-f]{2,12}$")
(def min-handle 2)

;; ---------------------------------------------------------------- the convention

(defn refs-key
  "The Field that holds the spans of a text Field: one rule in one place."
  [k]
  (if (= :message/text k) :message/refs (keyword (namespace k) (str (name k) "/refs"))))

(defn refs-field?
  "Is k a Field of spans, or the list of what did not resolve? They render through their text."
  [k]
  (or (= :message/refs k) (= :message/unresolved-refs k) (str/ends-with? (name k) "/refs")))

;; ---------------------------------------------------------------- handles

(defn- hex [id] (str/replace (str id) "-" ""))

(defn- common-prefix [^String a ^String b]
  (loop [i 0] (if (and (< i (count a)) (< i (count b)) (= (.charAt a i) (.charAt b i))) (recur (inc i)) i)))

(defn handles
  "table {handle id-string} extended by ids. A handle in the table keeps its meaning; a new id takes
   the shortest prefix of its hex that no other id here shares, min-handle at least."
  [table ids]
  (let [table (or table {})
        fresh (remove (set (vals table)) (distinct (map str ids)))
        all (vec (sort-by hex (distinct (concat (vals table) fresh))))
        at (zipmap all (range))
        len (fn [id]
              (let [i (at id) h (hex id)
                    near (keep #(when (<= 0 % (dec (count all))) (all %)) [(dec i) (inc i)])]
                (max min-handle (inc (reduce max 0 (map #(common-prefix h (hex %)) near))))))]
    (into table (map (fn [id] [(subs (hex id) 0 (len id)) id])) fresh)))

(defn ids-in
  "The ids in a text that name Objects of this world: anything else that looks like a uuid is data."
  [db text]
  (->> (re-seq uuid-re (str text)) distinct (filter #(objects/exists? db (parse-uuid %)))))

(defn shorten
  "Every id of the table in text as its handle."
  [text table]
  (when text
    (let [back (into {} (map (fn [[h id]] [id h])) table)]
      (str/replace text uuid-re #(java.util.regex.Matcher/quoteReplacement (get back % %))))))

(defn handle-for
  "What a handle or a full id stands for: {:id uuid} or nil. A prefix is never enough."
  [table s]
  (let [s (str s)]
    (cond (contains? table s) {:id (parse-uuid (table s))}
          (re-matches whole-uuid-re s) {:id (parse-uuid s)})))

(defn could-mean
  "The entries of a table an unknown handle might have been meant for."
  [table s]
  (let [s (str s)]
    (vec (sort (for [[h id] table
                     :when (or (str/starts-with? h s) (str/starts-with? s h) (str/starts-with? (hex id) s))]
                 (str h " is " id))))))

;; ---------------------------------------------------------------- names

(defn- readable? [db reader id]
  (and (objects/exists? db id) (not (objects/retired? db id))
       (boolean (resolve/resolves? db reader (str "/" id) :read))))

(defn display-name
  "What a person sees in place of a reference: the Object's name as the reader knows it, else any
   name, else its class and the head of its id."
  [db reader id]
  (let [nss (set (resolve/namespace-chain db reader))
        names (sort-by second (objects/names db id))]
    (or (some (fn [[ns n]] (when (nss ns) n)) names)
        (second (first names))
        (str (some-> (objects/state db id) :object/class name) " " (subs (str id) 0 8)))))

;; ---------------------------------------------------------------- in

(def ^:private scan-re
  ;; a fence (closed or running off the end), inline code, or a reference
  #"(?s)```.*?(?:```|\z)|`[^`\n]*`|\[\[([^\[\]\n]{1,200})\]\]")

(defn- cp-len ^long [^CharSequence s] (Character/codePointCount s 0 (count s)))

(defn- by-prefix
  "The one Object whose id starts with h, or nil. A handle from another wake's table, or a table a steer
   left behind, is still that id's prefix; two matches name nothing."
  [db ^String h]
  (let [hits (filter #(str/starts-with? (str %) h) (d/q '[:find [?id ...] :where [_ :object/id ?id]] db))]
    (when (= 1 (count hits)) (first hits))))

(defn- target
  "A reference's body as {:kind :object :field :seq :name :text}, or nil when it resolves to nothing."
  [db author table ^String body]
  (if-let [[_ n] (re-matches #"seq:(\d+)" body)]
    {:kind :seq :seq (parse-long n) :text (str "seq:" n)}
    (let [[head & others] (str/split body #"\|")
          [_ h f] (re-matches #"([0-9a-f-]{2,36})(?:\.([\w/.\-]+))?" head)
          id (or (when h (:id (handle-for table h)))
                 ;; a name the author can read is a reference too
                 (when-not (str/starts-with? head "/") (resolve/lookup db author head))
                 (some #(resolve/lookup db author %) others)
                 (when h (by-prefix db h)))]
      (when (and id (readable? db author id))
        (let [n (display-name db author id)]
          (cond-> {:kind (if f :field :object) :object id :name n :text (if f (str n "." f) n)}
            f (assoc :field f)))))))

(defn parse
  "What a model wrote -> {:text display :refs [spans] :unresolved [as written]}. table is the handle
   table of the Evaluation that wrote it. Text without references comes back unchanged."
  [db author table text]
  (let [text (str text)
        m (re-matcher scan-re text)
        sb (StringBuilder.)
        refs (transient [])
        bad (transient [])]
    (loop [pos 0 cp 0]
      (if (.find m)
        (let [whole (.group m)
              body (.group m 1)
              t (when body (target db author table body))
              ;; a model writes the name and then a reference to it: the name once (seen live: `shell.local shell.local`)
              before (let [b (subs text pos (.start m))
                           said (when t (str (:text t) " "))]
                       (if (and said (str/ends-with? b said))
                         (subs b 0 (- (count b) (count said)))
                         b))
              cp (+ cp (cp-len before))
              ;; nothing resolved: what it was called, never the brackets (seen live: [[0d|person|principal]])
              out (cond t (:text t)
                        (and body (str/includes? body "|")) (second (str/split body #"\|"))
                        :else whole)
              n (cp-len out)]
          (.append sb before)
          (.append sb ^String out)
          (cond t (conj! refs (-> (dissoc t :text) (assoc :start cp :end (+ cp n))))
                body (conj! bad whole))
          (recur (.end m) (+ cp n)))
        (.append sb (subs text pos))))
    {:text (str sb) :refs (persistent! refs) :unresolved (vec (distinct (persistent! bad)))}))

(defn valid-spans
  "An err string for spans that do not fit text or name what author cannot read, else nil."
  [db author text refs]
  (let [n (cp-len (str text))]
    (some (fn [{:keys [start end kind object] :as r}]
            (cond (not (and (nat-int? start) (nat-int? end) (<= start end n))) (str "offsets outside the text: " (pr-str r))
                  (not (#{:object :field :seq :path} (some-> kind name keyword))) (str "unknown kind: " (pr-str r))
                  (and (not= :seq (keyword (name kind)))
                       (not (and (uuid? object) (readable? db author object)))) (str "does not resolve: " object)))
          refs)))

;; ---------------------------------------------------------------- out

(defn- span-text
  "A span for a prompt: ids in full, shortened with everything else. nil keeps the display text."
  [db reader {:keys [kind object field seq name]}]
  (case (some-> kind clojure.core/name keyword)
    :seq (str "[[seq:" seq "]]")
    :object (when (readable? db reader object) (str "[[" object "|" name "]]"))
    :field (when (readable? db reader object) (str "[[" object "." field "|" name "]]"))
    nil))

(defn render
  "Stored text and spans back into references for reader. Old text has no spans and comes back as
   it is; a span that no longer fits is left out."
  [db reader ^String text refs]
  (if (or (empty? refs) (not (string? text)))
    text
    (let [n (cp-len text)
          sb (StringBuilder.)
          end (reduce (fn [pos {:keys [start end] :as r}]
                        (if (and (nat-int? start) (nat-int? end) (<= pos start end n))
                          (let [a (.offsetByCodePoints text 0 (int start))
                                b (.offsetByCodePoints text 0 (int end))]
                            (.append sb (subs text (.offsetByCodePoints text 0 (int pos)) a))
                            (.append sb (or (span-text db reader r) (subs text a b)))
                            (long end))
                          pos))
                      0 (sort-by :start refs))]
      (.append sb (subs text (.offsetByCodePoints text 0 (int end))))
      (str sb))))

(defn text-of
  "A Field's text as reader should see it in a prompt, from an Object's fields map."
  [db reader fields k]
  (let [v (get-in fields [k :field/value])]
    (render db reader v (get-in fields [(refs-key k) :field/value]))))

;; ---------------------------------------------------------------- what a reader is handed

(defn visible-spans
  "The spans reader can resolve. A seq names an event, no Object."
  [db reader refs]
  (filterv (fn [{:keys [object]}] (or (nil? object) (let [o (if (string? object) (parse-uuid object) object)]
                                                      (and (uuid? o) (readable? db reader o)))))
           refs))

(defn- span-field? [k]
  (and (not= :message/unresolved-refs k) (refs-field? k)))

(defn visible-fields
  "An Object's fields map with every span Field cut to what reader can resolve."
  [db reader fields]
  (reduce-kv (fn [m k f] (if (and (span-field? k) (sequential? (:field/value f)))
                           (assoc m k (update f :field/value #(visible-spans db reader %)))
                           m))
             fields fields))

(defn visible-event
  "An event with its span values cut for reader: a created Object's fields, or a field-set."
  [db reader {:event/keys [body] :as event}]
  (let [cut (fn [v] (if (sequential? v) (visible-spans db reader v) v))
        k (:name body)]
    (cond
      (map? (:fields body))
      (if (some span-field? (keys (:fields body)))
        (assoc event :event/body
               (update body :fields #(reduce-kv (fn [m k v] (assoc m k (if (span-field? k) (cut v) v))) {} %)))
        event)
      (and (keyword? k) (span-field? k) (contains? body :value)) (assoc event :event/body (update body :value cut))
      :else event)))
