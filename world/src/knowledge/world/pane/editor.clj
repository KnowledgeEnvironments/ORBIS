(ns knowledge.world.pane.editor
  "Editor buffers held in the world. The buffer is a fold of `pane-edit` events over the file at
   open, so every edit has an author on the log and a restart loses nothing, undo included. A
   long-open buffer can outlive the segments holding those events: before the retention sweep drops
   them it writes a `checkpoint`, the whole buffer in a blob, and the fold starts there.
   The holder rule: whoever edits holds the buffer until they save. Another Principal's edit must
   name the current `version`, proving it saw what is there; else `err conflict`. So a person and
   a model never overwrite each other silently.

   An editor can also sit on a Field (pane/target the Object, pane/field the name). Save is then a
   set-field by whoever saves, and a write to that Field from anywhere reloads a buffer nobody holds
   and marks a held one stale, so its holder decides. `reload` in the log records either."
  (:require [clojure.edn :as edn]
            [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.pane.core :as core])
  (:import [java.io File]
           [java.nio.file Files StandardCopyOption]))

(defn- err [code message] (act/err code message))

(defn- apply-edit
  "One edit, the undo stacks moved as it asks. at, del, text are the change to the text."
  [{:keys [text] :as b} {:keys [verb at del] new-text :text :as e}]
  (let [removed (subs text at (+ at del))
        inverse {:at at :del (count new-text) :text removed}
        b (assoc b :text (str (subs text 0 at) new-text (subs text (+ at del)))
                 :version (:version e) :holder (:who e))]
    (case verb
      :undo (-> b (update :undo pop) (update :redo conj inverse))
      :redo (-> b (update :redo pop) (update :undo conj inverse))
      (-> b (update :undo conj inverse) (assoc :redo [])))))

(defn- step
  "A buffer after one pane-edit body."
  [b {:keys [verb sha] :as e}]
  (case verb
    ;; a copy of what the fold already holds, for a fold that starts here
    :checkpoint b
    :save (-> b (assoc :base-sha sha :holder nil :version (:version e) :stale false) (dissoc :saving))
    ;; the whole text, from outside the buffer: nothing here undoes across that
    :reload (assoc b :text (:text e) :base-sha sha :holder nil :version (:version e) :stale false :undo [] :redo [])
    (apply-edit b e)))

(defn- blob-text [world sha]
  (some-> (blob/get (:blobs world) sha) (String. "UTF-8")))

(defn- checkpoint-of
  "The buffer a checkpoint event stands for, or nil when its blob is gone."
  [world {:keys [blob version]}]
  (when-let [text (blob-text world blob)]
    (assoc (edn/read-string text) :version version)))

(defn- what-is-there
  "The text of the file or Field under a pane, for a buffer whose own record has gone, or nil."
  [world id]
  (let [db (act/db world)
        target (core/field db id :pane/target)
        path (core/field db id :pane/path)]
    (cond
      (core/field db id :pane/field)
      (some-> (get-in (objects/fields db target) [(keyword (core/field db id :pane/field)) :field/value]) str)
      path
      (when-let [^File f (fs/safe-file (core/field db target :adapter/locator) path)]
        (when (.isFile f) (slurp f))))))

(defn- fold-buffer
  "A buffer from its pane's log: from the last checkpoint, else from the open. With neither, which takes
   a blob that went for all the sweep's care, it is what the file or Field holds now, unheld: edits
   that were never saved are gone, and the buffer says so, but it opens."
  [world id]
  (let [events (log/read-stream (:log world) id 0 Long/MAX_VALUE)
        edits #(filter (fn [e] (= :pane-edit (:event/kind e))) %)
        opened (:event/body (first (filter #(= :pane-opened (:event/kind %)) events)))
        cp (last (filter #(= :checkpoint (get-in % [:event/body :verb])) (edits events)))
        from-cp (when cp (checkpoint-of world (:event/body cp)))
        from-open (when-let [text (and (nil? cp) (blob-text world (:blob opened)))]
                    {:text text :version 0 :base-sha (:blob opened) :holder nil :undo [] :redo []})
        replay (fn [b es] (reduce (fn [b e] (step b (:event/body e))) b (edits es)))]
    (cond
      from-cp (replay from-cp (drop-while #(<= (long (:event/seq %)) (long (:event/seq cp))) events))
      from-open (replay from-open events)
      :else (let [text (str (what-is-there world id))]
              {:text text :version (long (or (:version (:event/body (last (edits events)))) 0))
               :base-sha (blob/sha256-hex (.getBytes text "UTF-8"))
               :holder nil :undo [] :redo [] :lost-edits true}))))

(declare field-moved!)

(defn- field-of
  "[object field] of a pane on a Field, or nil."
  [db id]
  (when-let [f (core/field db id :pane/field)]
    [(core/field db id :pane/target) (keyword f)]))

(defn- field-text [db [object k]] (str (get-in (objects/fields db object) [k :field/value])))

(defn- on-events!
  "Runs on the log's writer: a save waits for that thread, so the work goes elsewhere."
  [world events]
  (let [watched (into {} (keep (fn [[id {:keys [watch]}]] (when watch [watch id]))) @(:panes world))]
    (when (seq watched)
      (doseq [{:keys [event/kind event/stream event/body event/seq event/actor]} events
              :when (= :field-set kind)
              :let [id (get watched [stream (:name body)])]
              :when id]
        (Thread/startVirtualThread #(field-moved! world id seq actor (str (:value body))))))))

(defn- watch!
  "A pane on a Field follows that Field from now on."
  [world id target]
  (core/live world id)
  (swap! (:panes world) assoc-in [id :watch] target)
  (log/subscribe! (:log world) ::fields #(on-events! world %)))

(defn buffer
  "The buffer atom of an editor pane."
  [world id]
  (or (:buffer (core/live world id))
      (let [a (atom (fold-buffer world id))
            l (get (swap! (:panes world) update id (fn [l] (if (:buffer l) l (assoc l :buffer a)))) id)]
        ;; a Field can have moved while no one was watching: a restart, a pane nobody reopened
        (when-let [target (and (identical? a (:buffer l)) (field-of (act/db world) id))]
          (watch! world id target)
          (field-moved! world id 0 (core/field (act/db world) id :pane/owner) (field-text (act/db world) target)))
        (:buffer l))))

(defmethod core/snapshot :editor [world _ id]
  (let [{:keys [text version holder stale lost-edits]} @(buffer world id)]
    {:state (cond-> {:version version :holder holder}
              stale (assoc :stale true)
              lost-edits (assoc :lost-edits true))
     :data text}))

(defn checkpoint!
  "Put the whole buffer on the record, undo included, so its fold no longer needs the events before now."
  [world id]
  (let [a (buffer world id)]
    (locking a
      (let [{:keys [version] :as b} @a
            bs (.getBytes (pr-str (select-keys b [:text :base-sha :holder :undo :redo :stale])) "UTF-8")
            sha (:blob/sha256 (blob/put! (:blobs world) bs))
            owner (core/field (act/db world) id :pane/owner)]
        (log/append!! (:log world)
                      (log/pending :pane-edit id owner {:pane id :who owner :version version :verb :checkpoint
                                                        :at 0 :del 0 :text "" :blob sha}))
        sha))))

(defn needed-blobs
  "The blobs an open editor's fold starts from: its last checkpoint, else the file as it opened."
  [world id]
  (let [events (log/read-stream (:log world) id 0 Long/MAX_VALUE)
        cp (last (filter #(= :checkpoint (get-in % [:event/body :verb])) events))
        opened (first (filter #(= :pane-opened (:event/kind %)) events))]
    (keep identity [(get-in cp [:event/body :blob]) (get-in opened [:event/body :blob])])))

(defn checkpoint-needed?
  "Would dropping events up to `through` take something this buffer's fold starts from?"
  [world id through]
  (let [events (log/read-stream (:log world) id 0 Long/MAX_VALUE)
        cp (last (filter #(= :checkpoint (get-in % [:event/body :verb])) events))]
    (and (seq events) (<= (long (:event/seq (or cp (first events)))) (long through)))))

;; ---------------------------------------------------------------- positions

(defn- line-start
  "The offset of a 1-based line, clamped to the text."
  [^String text line]
  (loop [i 0 n 1]
    (if (>= n line)
      i
      (let [j (.indexOf text "\n" (int i))]
        (if (neg? j) (count text) (recur (inc j) (inc n)))))))

(defn- offset [text {:keys [at line col]}]
  (cond at (long at)
        line (+ (long (line-start text (long line))) (long (or col 0)))
        :else (count text)))

(defn- line-of [^String text at]
  (inc (count (filter #(= \newline %) (subs text 0 at)))))

(defn- occurrences [^String text ^String needle]
  (when (seq needle)
    (loop [from 0 acc []]
      (let [i (.indexOf text needle (int from))]
        (if (neg? i) acc (recur (+ i (count needle)) (conj acc i)))))))

;; ---------------------------------------------------------------- edits

(defn- conflict
  "Why p may not edit now, or nil. The holder edits freely; anyone else names the version they saw."
  [{:keys [holder version]} p given]
  (cond
    (and (some? given) (not= (long given) (long version)))
    (err :conflict (str "the buffer is at version " version ", you edited version " given
                        "; read it again (" (or holder "no one") " holds it)"))
    (and (nil? given) holder (not= holder p))
    (err :conflict (str holder " holds this buffer; read it and give its `version` to edit"))))

(defn- commit!
  "Log one edit, fold it in, push it."
  [world p id verb {:keys [at del text]}]
  (let [a (buffer world id)
        version (inc (long (:version @a)))
        body {:pane id :who p :version version :verb verb :at at :del del :text text}
        seq (act/append! world (log/pending :pane-edit id p body))]
    (swap! a step body)
    (core/push! world id {:edit (select-keys body [:at :del :text :who :version])})
    {:ok {:result {:pane id :version version :line (line-of (:text @a) (min (+ at (count text)) (count (:text @a))))}
          :seq seq}}))

(defn- edit!
  "An edit of the text, under the holder rule."
  [world p id verb args change]
  (let [a (buffer world id)]
    (locking a
      (or (conflict @a p (:version args))
          (let [c (change @a)]
            (if (:err c) c (commit! world p id verb c)))))))

(defn- insert [args {:keys [text]}]
  (let [at (offset text args)]
    (cond (not (string? (:text args))) (err :invalid-args "insert needs text")
          (not (<= 0 at (count text))) (err :invalid-args "insert position is outside the buffer")
          :else {:at at :del 0 :text (:text args)})))

(defn- replace-text [{:keys [find with all at del] :as args} {:keys [text]}]
  (cond
    (string? find)
    (let [hits (occurrences text find)]
      (cond
        (empty? hits) (err :invalid-args (str "not in the buffer: " find))
        (and (next hits) (not all))
        (err :invalid-args (str (count hits) " matches; say more of the text, or all true"))
        all (let [from (first hits) to (+ (peek hits) (count find))]
              {:at from :del (- to from) :text (str/replace (subs text from to) find (str with))})
        :else {:at (first hits) :del (count find) :text (str with)}))
    (and at del) (if (<= 0 at (+ at del) (count text))
                   {:at at :del del :text (str (or with (:text args)))}
                   (err :invalid-args "replace range is outside the buffer"))
    :else (err :invalid-args "replace needs {find with all?} or {at del with}")))

(defn- undo-redo [stack {:keys [undo redo]}]
  (if-let [top (peek (if (= :undo stack) undo redo))]
    top
    (err :invalid-args (str "nothing to " (name stack)))))

;; ---------------------------------------------------------------- save

(defn- disk-sha [^File f] (when (.exists f) (blob/sha256-hex (Files/readAllBytes (.toPath f)))))

(defn- write-file!
  "The whole file or none of it."
  [^File f ^String text]
  (let [tmp (File. (.getParentFile f) (str "." (.getName f) ".orbis-" (System/nanoTime)))]
    (spit tmp text)
    (Files/move (.toPath tmp) (.toPath f) (into-array java.nio.file.CopyOption [StandardCopyOption/ATOMIC_MOVE
                                                                               StandardCopyOption/REPLACE_EXISTING]))))

(defn save!
  "The buffer to disk: :write on the file, a writable mount, and the file as it was when read,
   unless force says otherwise. Clears the holder."
  [world p id {:keys [force]} locator rel]
  (let [a (buffer world id)
        ^File f (fs/safe-file locator rel)]
    (locking a
      (let [{:keys [base-sha text version]} @a
            on-disk (disk-sha f)]
        (if (and (not force) on-disk (not= on-disk base-sha))
          (err :conflict "the file changed on disk since it was opened; save with force true to replace it")
          (let [_ (write-file! f text)
                sha (:blob/sha256 (blob/put! (:blobs world) (.getBytes ^String text "UTF-8")))
                v (inc (long version))
                body {:pane id :who p :version v :verb :save :at 0 :del 0 :text "" :sha sha}
                seq (act/append! world (log/pending :pane-edit id p body))]
            (swap! a step body)
            (core/push! world id {:state {:version v :holder nil :saved sha}})
            {:ok {:result {:pane id :version v :saved sha :bytes (.length f)} :seq seq}}))))))

;; ---------------------------------------------------------------- a pane on a Field

(defn- sha-of [^String text] (blob/sha256-hex (.getBytes text "UTF-8")))

(defn- reload!
  "The buffer becomes text, as who wrote it, whatever it held: every client gets the whole text."
  [world id who ^String text]
  (let [a (buffer world id)
        sha (:blob/sha256 (blob/put! (:blobs world) (.getBytes text "UTF-8")))
        {:keys [version] old :text} @a
        v (inc (long version))
        body {:pane id :who who :version v :verb :reload :at 0 :del (count old) :text text :sha sha}
        seq (act/append! world (log/pending :pane-edit id who body))]
    (swap! a step body)
    (core/push! world id {:full true :data text :state {:version v :holder nil :stale false}})
    {:ok {:result {:pane id :version v :saved sha} :seq seq}}))

(defn- field-moved!
  "The Field now holds text, as of log seq n. A buffer nobody holds follows it. A held one is left
   alone and marked stale. A save of this buffer lands here too, and changes nothing."
  [world id n who ^String text]
  (let [a (buffer world id)]
    (locking a
      (let [{:keys [base-sha saving holder field-seq]} @a
            sha (sha-of text)]
        (when (or (zero? n) (> (long n) (long (or field-seq 0))))
          (swap! a assoc :field-seq n)
          (cond
            (or (= sha base-sha) (= sha saving)) nil
            (nil? holder) (reload! world id who text)
            :else (do (swap! a assoc :stale true)
                      (core/push! world id {:state {:stale true}}))))))))

(defn watch-field!
  "Called when a pane on a Field opens, so a write before its first reader is not missed."
  [world id]
  (buffer world id)
  nil)

(defn revert!
  "Back to what is really there, the Field or the file, dropping the buffer's edits. Under the holder
   rule: a buffer someone else holds is theirs to give up."
  [world p id args ^String text]
  (let [a (buffer world id)]
    (locking a
      (or (conflict @a p (:version args))
          (reload! world id p text)))))

(defn save-field!
  "The buffer to its Field, by p, under p's own Grants: act/set-field! is the write. Type, reader and
   visibility stay the Field's. A Field that moved since this buffer read it is a conflict unless
   force says otherwise. Clears the holder."
  [world p id {:keys [force]} via]
  (let [a (buffer world id)
        [object k] (field-of (act/db world) id)]
    (locking a
      (let [{:keys [base-sha text version]} @a
            rec (get (objects/fields (act/db world) object) k)
            sha (:blob/sha256 (blob/put! (:blobs world) (.getBytes ^String text "UTF-8")))]
        (cond
          (and (not force) (nil? rec))
          (err :conflict (str (subs (str k) 1) " is gone; save with force true to set it again"))
          (and (not force) (not= (sha-of (str (:field/value rec))) base-sha))
          (err :conflict (str (subs (str k) 1) " changed since this buffer read it; save with force true to replace it"))
          :else
          (let [_ (swap! a assoc :saving sha)
                r (act/set-field! world p (cond-> {:object object :field k :value text
                                                   :type (or (:field/type rec) :string)
                                                   :reader (or (:field/reader rec) :both)
                                                   :via via}
                                            (:field/visibility rec) (assoc :visibility (:field/visibility rec))))]
            (if (:err r)
              (do (swap! a dissoc :saving) r)
              (let [v (inc (long version))
                    body {:pane id :who p :version v :verb :save :at 0 :del 0 :text "" :sha sha}
                    seq (act/append! world (log/pending :pane-edit id p body))]
                (swap! a step body)
                (core/push! world id {:state {:version v :holder nil :saved sha :stale false}})
                {:ok {:result {:pane id :version v :saved sha :bytes (count (.getBytes ^String text "UTF-8"))}
                      :seq seq}}))))))))

;; ---------------------------------------------------------------- frobs

(defn frob!
  "An editor verb in the world; `save` goes through save!."
  [world p id verb args]
  (let [{:keys [text version holder]} @(buffer world id)]
    (case verb
      :goto (let [at (offset text args)
                  n (line-of text (min at (count text)))
                  end (str/index-of text "\n" at)]
              (core/push! world id {:state {:line n}})
              {:ok {:result {:pane id :line n :text (subs text (long (line-start text n)) (or end (count text)))
                             :version version :holder holder}
                    :seq (core/applied-seq world)}})
      :find (let [hits (occurrences text (str (:text args)))]
              {:ok {:result {:pane id :count (count hits) :version version
                             :hits (mapv (fn [i] {:at i :line (line-of text i)}) (take 100 hits))}
                    :seq (core/applied-seq world)}})
      :insert (edit! world p id :insert args (partial insert args))
      :replace (edit! world p id :replace args (partial replace-text args))
      :undo (edit! world p id :undo args (partial undo-redo :undo))
      :redo (edit! world p id :redo args (partial undo-redo :redo))
      (err :invalid-args (str "an editor has no verb " (name verb))))))
