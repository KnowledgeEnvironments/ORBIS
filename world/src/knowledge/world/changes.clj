(ns knowledge.world.changes
  "When someone else changes a Principal's Fields (decisions 2026-10-31). The prompt is rebuilt on every
   wake, so an edit reaches the model by itself; what it lacks is knowing that anything moved. So the
   first turn of a wake carries one line naming what changed since the last wake that saw a change,
   loop/seen-seq marks them seen, and the listener tells the person the same thing: applies at the next
   wake, then seen. field.diff gives the detail on demand: a model cannot ask about a change it has not
   heard of, so the line is automatic and the detail is not."
  (:require [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve]))

(def seen-field :loop/seen-seq)
(def max-named "Fields a note names before it counts the rest." 5)
(def max-diff-lines 200)

(def ^:private noise-namespaces
  "Running figures, and settings a person flips: neither is news to the agent."
  #{"cost" "view" "evaluation" "notification" "harness" "topic" "action" "loop" "principal" "policy" "serialize" "model"})

(defn- about-the-principal?
  "Is this Field something a Principal is told about? Not its running figures."
  [k]
  (let [ns* (namespace k) n (name k)]
    (cond
      (noise-namespaces ns*) false
      (#{"hint" "comment"} n) false
      :else true)))

(defn pending
  "Fields of p that someone else set after its seen mark, oldest first: [{:field :by :seq}]. A world's own
   writes are not news, and a Principal that never woke has no mark and nothing to catch up on."
  [db p]
  (let [seen (get-in (objects/fields db p) [seen-field :field/value])
        world (objects/world-id db)]
    (when (number? seen)
      (->> (objects/fields db p)
           (keep (fn [[k f]]
                   (let [by (:field/author f) s (:field/set-seq f)]
                     (when (and (about-the-principal? k) (some? s) (> (long s) (long seen))
                                (not= by p) (not= by world))
                       {:field k :by by :seq (long s)}))))
           (sort-by :seq)
           vec))))

(defn note
  "The one line for a wake, or nil."
  [db p changed name-of]
  (when (seq changed)
    (let [by (distinct (map (comp name-of :by) changed))
          named (map #(subs (str (:field %)) 1) (take max-named changed))
          rest* (- (count changed) (count named))]
      (str "Changed since your last wake, by " (str/join ", " by) ": " (str/join ", " named)
           (when (pos? rest*) (str " and " rest* " more"))
           ". field.diff {object, field} shows what."))))

(defn see!
  "A wake takes in what changed: the note for its first turn, or nil. Marks them seen, and writes nothing
   when there is nothing to mark, so a quiet wake costs no event."
  [world p name-of]
  (let [db (act/db world)
        marked? (number? (get-in (objects/fields db p) [seen-field :field/value]))
        changed (pending db p)]
    (when (or (not marked?) (seq changed))
      (act/append! world (log/pending :field-set p p {:name seen-field :type :number
                                                      :value (long (log/head (:log world))) :reader :both})))
    (note db p changed name-of)))

;; ---------------------------------------------------------------- field.diff

(defn- text-of [v] (cond (nil? v) "" (string? v) v :else (pr-str v)))

(defn- lcs-table [a b]
  (let [n (count a) m (count b)
        t (make-array Long/TYPE (inc n) (inc m))]
    (doseq [i (range (dec n) -1 -1) j (range (dec m) -1 -1)]
      (aset ^longs (aget ^"[[J" t i) j
            (long (if (= (a i) (b j))
                    (inc (aget ^longs (aget ^"[[J" t (inc i)) (inc j)))
                    (max (aget ^longs (aget ^"[[J" t (inc i)) j)
                         (aget ^longs (aget ^"[[J" t i) (inc j)))))))
    t))

(defn line-diff
  "A line diff of two texts, the changed lines only, `- ` and `+ `, longest common subsequence."
  [before after]
  (let [lines (fn [s] (if (= "" s) [] (vec (str/split-lines s))))
        a (lines before) b (lines after)
        t (lcs-table a b)
        n (count a) m (count b)]
    (loop [i 0 j 0 out []]
      (cond
        (and (< i n) (< j m) (= (a i) (b j))) (recur (inc i) (inc j) out)
        (and (< i n) (or (= j m) (>= (aget ^longs (aget ^"[[J" t (inc i)) j)
                                     (aget ^longs (aget ^"[[J" t i) (inc j)))))
        (recur (inc i) j (conj out (str "- " (a i))))
        (< j m) (recur i (inc j) (conj out (str "+ " (b j))))
        :else out))))

(defn- sets-of
  "The field-set events of a Field, oldest first."
  [log object k]
  (loop [after 0 out []]
    (let [events (log/read-stream log object after 1000)
          out (into out (filter #(and (= :field-set (:event/kind %)) (= k (get-in % [:event/body :name])))) events)]
      (if (< (count events) 1000) out (recur (:event/seq (peek events)) out)))))

(defn diff!
  "field.diff {object field since?}: what the last change did, or everything after seq `since`. It needs
   read on the Object, and a Field with its own visibility stays hidden."
  [world p {:keys [object field since]}]
  (let [db (act/db world)
        id (or (parse-uuid (str object)) (resolve/lookup db p (str object)))
        k (keyword (let [s (str field)] (if (str/starts-with? s ":") (subs s 1) s)))]
    (if-not (and id (resolve/resolves? db p (str "/" id) :read)
                 (or (not (contains? (objects/fields db id) k))
                     (contains? (resolve/visible-fields db p (select-keys (objects/fields db id) [k]) :read) k)))
      act/unresolved
      (let [events (sets-of (:log world) id k)
            after (if since (filterv #(> (long (:event/seq %)) (long since)) events) (vec (take-last 1 events)))]
        (if (empty? after)
          {:ok {:result {:object id :field k :changed false}}}
          (let [first-ev (first after) last-ev (peek after)
                before (text-of (get-in first-ev [:event/body :previous]))
                now (text-of (get-in last-ev [:event/body :value]))
                lines (line-diff before now)]
            {:ok {:result (cond-> {:object id :field k :changed true :changes (count after)
                                   :by (:event/actor last-ev) :seq (:event/seq last-ev)
                                   :diff (vec (take max-diff-lines lines))}
                            (> (count lines) max-diff-lines) (assoc :clipped (count lines)))}}))))))
