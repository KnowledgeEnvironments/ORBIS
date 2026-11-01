(ns knowledge.world.resolve.mount
  "Mounts (spec 6.5). A mount is an fs Adapter (adapter/binds :filesystem, adapter/locator a
   directory) bound at a name; `<name>/<path>` denotes the file at that path. The name may also be
   a Principal's: then its workspace, the first fs Adapter in its principal/adapters, is the mount
   (`atlas/README.md` is the file in atlas's workspace; decisions 2026-10-30 'named actions'). Resolution finds the
   mount by its first segment in the Principal's namespace chain, checks the Principal's Grants on
   the prefix (or on the Adapter itself) before the Adapter is asked, and on a successful `resolve!`
   materializes the file as an Object: `created` with the file/* Fields and object/backing the
   Adapter, `bound` under the full name in the world namespace, both as the resolving Principal
   under the permitting Grant. A later resolution whose disk state differs appends field-set events.
   The log stays the truth: a file's Object is a record of what was read, when, by whom."
  (:require [clojure.string :as str]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]))

(defn- adapter-fields [db id]
  (let [s (objects/state db id)]
    (when (= :adapter (:object/class s))
      (into {} (map (fn [[k f]] [k (:field/value f)])) (:object/fields s)))))

(defn mount-of
  "For a name with a slash: the fs Adapter bound at its first segment in one of namespaces, with
   the relative path. {:adapter :locator :rel :name :root} or nil."
  [db namespaces n]
  (when-let [i (and (not (str/starts-with? n "/")) (str/index-of n "/"))]
    (let [root (subs n 0 i) rel (subs n (inc i))]
      (when (and (seq rel) (not (str/includes? rel "..")))
        (when-let [found (some #(objects/lookup db % root) namespaces)]
          (let [adapter (or (when (adapter-fields db found) found)
                            ;; a Principal's name: its workspace, the first fs Adapter in principal/adapters
                            ;; (workspace.mount binds the Adapter itself <name>.fs; decisions 2026-10-30)
                            (some (fn [a] (when (= :filesystem (:adapter/binds (adapter-fields db a))) a))
                                  (get-in (objects/fields db found) [:principal/adapters :field/value])))
                a (when adapter (adapter-fields db adapter))]
            (when (and a (= :filesystem (:adapter/binds a)) (string? (:adapter/locator a)))
              {:adapter adapter :locator (:adapter/locator a) :rel rel :name n :root root})))))))

(defn exists? [{:keys [locator rel]}] (some? (fs/stat locator rel)))

(defn backing-mount
  "When object was materialized from a mount, the mount record for its full name, else nil."
  [db namespaces object-id]
  (when-let [backing (:object/backing (objects/entity db object-id))]
    (when (adapter-fields db backing)
      (some (fn [[_ n]] (when-let [m (mount-of db namespaces n)] (when (= backing (:adapter m)) m)))
            (objects/names db object-id)))))

(defn- changed?
  "Does the disk differ from the Object's recorded size and mtime?"
  [db object-id {:keys [locator rel]}]
  (let [f (objects/fields db object-id)
        st (fs/stat locator rel)]
    (or (nil? st)
        (not= (:size st) (get-in f [:file/size :field/value]))
        (not= (some-> st :mtime .getTime) (some-> (get-in f [:file/mtime :field/value]) .getTime)))))

(defn refresh!
  "Append field-set events for a materialized Object whose file changed on disk (or vanished:
   file/kind :missing). Returns the number of Fields set."
  [log db p grant object-id mount]
  (if-not (changed? db object-id mount)
    0
    (let [desc (or (fs/describe (:locator mount) (:rel mount)) {:file/kind :missing})
          current (objects/fields db object-id)
          sets (for [[k v] (dissoc desc :file/path)
                     :when (not= v (get-in current [k :field/value]))]
                 [k v])]
      (doseq [[k v] sets]
        (log/append!! log (log/pending :field-set object-id p {:name k :type (fs/field-type k v) :value v :reader :both}
                                       :delegation grant)))
      (count sets))))

(defn materialize!
  "The Object for a mounted path, created if the world namespace has none. Returns its id or nil
   when the path does not exist."
  [log db p grant world-ns {:keys [adapter locator rel name] :as mount}]
  (if-let [existing (objects/lookup db world-ns name)]
    (do (refresh! log db p grant existing mount) existing)
    (when-let [desc (fs/describe locator rel)]
      (let [id (random-uuid)]
        (log/append!! log (log/pending :created id p {:class :object :fields (assoc desc :object/backing adapter)} :delegation grant))
        (log/append!! log (log/pending :bound world-ns p {:namespace world-ns :name name :object id} :streams #{id} :delegation grant))
        id))))
