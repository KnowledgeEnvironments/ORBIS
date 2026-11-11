(ns knowledge.world.search.core
  "Search (spec 14.3 op search, principle 'naming is the single predicate'):
   substring and word match over string Fields, Message texts and bound names, scoped by
   resolution (only Objects the caller reads, only Fields it may see). Case and the separators
   _ - / . are one: `real name` finds real_name, in text, in names and in the name of a Field. What
   holds every word comes first, then the rest, then a loop's own records; newest first in each,
   then by score. The op `search {text limit? kinds? since?}` answers {hits total}; the action
   search.find gives loops the same. One `resolved` event (name search:<text>, op read, count)
   records the read, as the snapshot does. The index is the fold itself: a scan of the Datascript
   db, which holds no fact the log does not (ground rule 3); an inverted index can replace the
   scan later without changing the answer."
  (:require [clojure.string :as str]
            [datascript.core :as d]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.core :as fold]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve]))

(def max-limit 100)
(def default-limit 20)
(def snippet-chars 160)
(def kinds #{:field :message :object})
(def never-fields #{:principal/token-hash :token/hash :pair/code-hash}) ; a credential's or a pairing code's hash is nobody's search result

(def record-fields
  "A loop's own record of what it saw: it repeats everything it was shown, and is newer than all of
   it, so it ranks below what it repeats."
  #{:evaluation/thinking :evaluation/outcome :evaluation/ops :evaluation/results :serialization/bytes})

(def ^:private name-blind
  "Namespaces whose Field names say nothing: `message` must not find every Message."
  #{"message" "evaluation" "serialization" "task" "file" "action" "cost" "loop" "class"})

(defn- norm
  "Lower case with the separators as spaces, one char for one, so an offset here is an offset in the
   original."
  [^String s]
  (str/replace (str/lower-case (str s)) #"[_\-/.]" " "))

(defn- terms
  "The words as typed: only a space splits, so den.owl stays one term and finds den_owl."
  [text]
  (vec (distinct (map norm (remove str/blank? (str/split (str text) #"\s+"))))))

(defn score
  "2 for the whole text found, plus 1 per word found; 0 is no hit."
  [ts ^String hay ^String text]
  (let [low (norm hay)]
    (+ (if (and (seq text) (str/includes? low (norm text))) 2 0)
       (count (filter #(str/includes? low %) ts)))))

(defn snippet
  "Up to snippet-chars of hay around the first match."
  [^String hay ^String text]
  (let [low (norm hay)
        i (.indexOf low (norm text))
        i (if (neg? i) 0 i)
        start (max 0 (- i 40))
        end (min (count hay) (+ start snippet-chars))]
    (str (when (pos? start) "…") (subs hay start end) (when (< end (count hay)) "…"))))

(defn- field-name-hit?
  "The whole text is in this Field's name."
  [k text]
  (and (seq text) (not (name-blind (namespace k)))
       (str/includes? (norm (subs (str k) 1)) (norm text))))

(defn- rank
  "0 holds every word, 1 some of them, 2 a loop's own record."
  [ts text {:keys [field snippet hay]}]
  (cond (record-fields field) 2
        (let [low (norm (or hay snippet))] (or (str/includes? low (norm text)) (every? #(str/includes? low %) ts))) 0
        :else 1))

(defn- string-fields
  "[[object-id field-name value set-seq] ...] for every string-valued Field in the db (the string
   test runs here: Datascript knows no string? predicate)."
  [db]
  (filter (fn [[_ _ v _]] (string? v))
          (d/q '[:find ?id ?k ?v ?s
                 :where [?o :object/fields ?f] [?o :object/id ?id] [?f :field/name ?k] [?f :field/value ?v]
                 [(get-else $ ?f :field/set-seq 0) ?s]]
               db)))

(defn- names [db]
  (for [[key id] (d/q '[:find ?k ?id :where [?o :object/names ?k] [?o :object/id ?id]] db)
        :let [[_ n] (fold/split-name-key key)]
        :when (string? n)]
    [id n]))

(defn search
  "{:text :limit :kinds :since} -> {:hits [{object field? name? seq snippet score kind}] :total}.
   kinds default to all three; since is a seq (hits set at or after it); limit is clamped to
   max-limit. Only what the caller resolves for :read, only Fields it may see."
  [db p {:keys [text limit since] ks :kinds}]
  (let [text (str/trim (str text))
        ts (terms text)
        limit (max 1 (min max-limit (long (or limit default-limit))))
        ks (set (keep #(kinds (keyword (name %))) (or (seq ks) kinds)))
        since (long (or since 0))
        readable (memoize (fn [o] (and (not (objects/retired? db o)) (boolean (resolve/resolves? db p (act/id-name o) :read)))))
        visible? (fn [o k] (contains? (resolve/visible-fields db p (select-keys (objects/fields db o) [k]) :read) k))
        field-hits (when (some ks [:field :message])
                     (for [[o k v s] (string-fields db)
                           :let [msg? (= :message/text k)
                                 kind (if msg? :message :field)]
                           :when (not (never-fields k))
                           :when (not= "topic" (namespace k)) ; the manual has doc.candidates
                           :when (ks kind)
                           :when (>= (long s) since)
                           :let [named? (field-name-hit? k text)
                                 sc (+ (score ts v text) (if named? 3 0))]
                           :when (pos? sc)
                           :when (readable o)
                           :when (visible? o k)]
                       {:object o :field k :seq (long s) :snippet (snippet v text) :score sc :kind kind
                        :hay (if named? (str (subs (str k) 1) " " v) v)}))
        ;; one hit per Object however many namespaces bind the name (the creator's and the world's)
        name-hits (when (ks :object)
                    (->> (for [[o n] (names db)
                               :let [sc (score ts n text)
                                     s (long (or (:object/created-seq (objects/entity db o)) 0))]
                               :when (pos? sc)
                               :when (>= s since)
                               :when (readable o)]
                           {:object o :name n :seq s :snippet n :score (inc sc) :kind :object})
                         (group-by :object)
                         vals
                         (map #(apply max-key :score %))))
        hits (sort-by (juxt #(rank ts text %) (comp - :seq) (comp - :score)) (concat field-hits name-hits))]
    {:hits (mapv #(dissoc % :hay) (take limit hits)) :total (count hits)}))

(defn search!
  "The op: the scoped hits and one `resolved` summary event."
  [world p {:keys [text] :as args}]
  (if (str/blank? (str text))
    (act/err :invalid "search needs text")
    (let [r (search (act/db world) p args)
          shown (let [t (str/trim (str text))] (subs t 0 (min 80 (count t))))]
      (act/append! world (log/pending :resolved p p {:name (str "search:" shown) :op :read :count (:total r)}))
      {:ok r})))
