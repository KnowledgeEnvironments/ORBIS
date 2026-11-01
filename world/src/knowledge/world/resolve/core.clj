(ns knowledge.world.resolve.core
  "Resolution, the one access path (spec 6, ground rule 4):
   (resolves? db principal-id name op) -> grant-id | nil.
   1. Look the name up in the principal's namespace chain: its own namespace, the namespaces of
      the Groups it belongs to (transitively), then the world namespace (spec 6.4 shadowing).
      Name forms: a bound name, or \"/<object-id>\".
   2. Collect the Grants whose grantee is the principal or one of those Groups and whose scope
      covers the Object (a set of ids, a filter {:class :prefix :field-prefix}, or :everything).
   3. Any such Grant that lists op, is unexpired and not revoked resolves; the first one is returned.
   4. For op :read only, the creator of an Object resolves it under the Grant it created it with
      (object/created-under; decisions 2026-10-30): authorship of a record includes reading it.
   5. A name under a mount (spec 6.5, knowledge.world.resolve.mount): `<mount>/<path>` whose first
      segment is a bound fs Adapter resolves when the path exists and a Grant covers the name
      (prefix), the Adapter (set) or everything; `resolve!` then materializes the file's Object.
   6. For op :read only, a recipient of a Message (one of message/to) resolves it under the Grant
      it was sent with (decisions 2026-10-30 'a recipient reads its Message'): delivery is access,
      or a Principal narrower than root could never see what it was asked.
   7. For op :evaluate only, the sender of a Message resolves the Evaluation answering it
      (evaluation/message, decisions 2026-10-30 cancel) under the Grant the Evaluation was created
      with: whoever asked may stop the answer.
   8. For op :read only, a person a notification is addressed to resolves it (knowledge.world.notify).
   9. For op :evaluate only, an action marked action/public resolves for every Principal.
   A miss and a refusal are the same nil (spec 6.1).

   `resolve!` is the event-appending form used by ops: a success appends `resolved` to the
   principal's stream and the Object's stream (one event, two streams). The subscription pump uses
   the pure predicate: a standing filter over an already-authorised subscription appends nothing,
   otherwise every push would append an event that is itself pushed (see CONTRACT-NEEDS.md)."
  (:require [clojure.string :as str]
            [datascript.core :as d]
            [knowledge.world.authority :as authority]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.mount :as mount])
  (:import [java.util Date]))

(defn groups-of
  "Transitive set of Group ids the principal belongs to."
  [db principal-id]
  (loop [frontier #{principal-id} seen #{}]
    (if (empty? frontier)
      seen
      (let [parents (set (d/q '[:find [?gid ...] :in $ [?m ...] :where [?g :group/members ?m] [?g :object/id ?gid]] db frontier))
            new (remove seen parents)]
        (recur (set new) (into seen new))))))

(defn namespace-chain
  "Namespace ids to look a name up in, in shadowing order."
  [db principal-id]
  (let [own (:principal/namespace (objects/entity db principal-id))
        groups (keep #(:principal/namespace (objects/entity db %)) (groups-of db principal-id))
        world (objects/world-id db)]
    (distinct (remove nil? (concat [own] groups [world])))))

(defn lookup
  "Step 1: the Object id a name denotes for a principal, or nil."
  [db principal-id n]
  (if (str/starts-with? n "/")
    (let [id (parse-uuid (subs n 1))]
      (when (objects/exists? db id) id))
    (some #(objects/lookup db % n) (namespace-chain db principal-id))))

(defn- expired? [until ^Date now]
  (and (inst? until) (not (.after ^Date until now))))

(defn active-grants
  "Grant entities held by any of grantee-ids, unrevoked and unexpired at now."
  [db grantee-ids now]
  (for [gid (d/q '[:find [?g ...] :in $ [?p ...] :where [?g :grant/grantee ?p]] db grantee-ids)
        :let [g (d/entity db gid)]
        :when (and (not (:grant/revoked? g)) (not (expired? (:grant/until g) now)))]
    g))

(defn- object-names-in [db object-id namespaces]
  (let [nss (set namespaces)]
    (for [[ns n] (objects/names db object-id) :when (nss ns)] n)))

(defn- filtered-covers?
  "A filtered scope {:class :prefix :field-prefix}: every constraint named holds now, for this Object
   (decisions 2026-10-30). Evaluated at resolution, so an Object that comes to match is covered. A
   scope that names no constraint covers nothing."
  [db {:keys [class prefix field-prefix]} object-id n namespaces]
  (and (or class prefix field-prefix)
       (or (nil? class) (= (keyword (name class)) (:object/class (objects/entity db object-id))))
       (or (nil? prefix) (some #(str/starts-with? % prefix) (cons n (object-names-in db object-id namespaces))))
       (or (nil? field-prefix)
           (some #(str/starts-with? (subs (str %) 1) field-prefix) (keys (objects/fields db object-id))))))

(defn covers?
  "Does the grant's scope cover the Object (reached as name, or by id)?"
  [db grant object-id n namespaces]
  (let [scope (:grant/scope grant)]
    (cond (= scope :everything) true
          (set? scope) (contains? scope object-id)
          (map? scope) (filtered-covers? db scope object-id n namespaces)
          :else false)))

(defn grants-for
  "Active grants of the principal (or its groups) covering object-id for op."
  [db principal-id object-id n op now]
  (let [holders (conj (groups-of db principal-id) principal-id)
        namespaces (namespace-chain db principal-id)]
    (filter #(and (contains? (:grant/ops %) op) (covers? db % object-id n namespaces))
            (active-grants db holders now))))

(defn mount-grant
  "Step 5: for a mounted name with no Object yet, {:mount :grant} when the path exists and a Grant
   covers it, else nil."
  [db principal-id n op now]
  (when-let [m (mount/mount-of db (namespace-chain db principal-id) n)]
    (when (mount/exists? m)
      (when-let [g (first (grants-for db principal-id (:adapter m) n op now))]
        {:mount m :grant (:object/id g)}))))

(defn- recipient?
  "Step 6: is the principal among the Message's message/to?"
  [db object-id principal-id]
  (and (= :message (:object/class (objects/entity db object-id)))
       (contains? (set (get-in (objects/fields db object-id) [:message/to :field/value])) principal-id)))

(defn- carried-to?
  "Step 6b: is the Object the view (message/view) of a Message whose message/to names
   the principal? What a Message carries is read with it, as the Message itself is."
  [db object-id principal-id]
  (boolean (some (fn [m] (let [fs (objects/fields db m)]
                           (and (= object-id (get-in fs [:message/view :field/value]))
                                (contains? (set (get-in fs [:message/to :field/value])) principal-id))))
                 (objects/objects-with-field db :message/view))))

(defn- notified?
  "Step 8: is the Object a notification addressed to the principal? It reads what was sent to it, and
   nothing the notification names (knowledge.world.notify)."
  [db object-id principal-id]
  (contains? (set (get-in (objects/fields db object-id) [:notification/to :field/value])) principal-id))

(defn- public-action?
  "Step 9: an action every Principal may call, action/public on it (notify)."
  [db object-id]
  (true? (get-in (objects/fields db object-id) [:action/public :field/value])))

(defn- answers-own-message?
  "Step 7: is the Object an Evaluation whose evaluation/message the principal sent?"
  [db object-id principal-id]
  (and (= :evaluation (:object/class (objects/entity db object-id)))
       (when-let [m (get-in (objects/fields db object-id) [:evaluation/message :field/value])]
         (= principal-id (get-in (objects/fields db m) [:message/from :field/value])))))

(defn resolves?
  "The predicate of spec 6.1. Returns the permitting Grant's id or nil."
  ([db principal-id n op] (resolves? db principal-id n op (Date.)))
  ([db principal-id n op now]
   (if-let [object-id (lookup db principal-id n)]
     (let [e (objects/entity db object-id)]
       ;; a retired Object resolves for one thing: reading it by id, so links and the audit trail
       ;; still work (decisions 2026-10-31); its names are gone, so a name never reaches here
       ;; a person's terminal does not exist for an agent it was not shared with, root included
       (when (and (not (authority/closed-pane? db object-id principal-id now))
                  (or (not (:object/retired? e)) (and (= op :read) (str/starts-with? n "/"))))
         (or (:object/id (first (grants-for db principal-id object-id n op now)))
             (when (and (= op :read) (= principal-id (:object/created-by e)))
               (:object/created-under e))
             (when (and (= op :read) (recipient? db object-id principal-id))
               (:object/created-under e))
             (when (and (= op :read) (carried-to? db object-id principal-id))
               (:object/created-under e))
             (when (and (= op :evaluate) (answers-own-message? db object-id principal-id))
               (:object/created-under e))
             (when (and (= op :read) (notified? db object-id principal-id))
               (:object/created-under e))
             (when (and (= op :evaluate) (public-action? db object-id))
               (:object/created-under e)))))
     (:grant (mount-grant db principal-id n op now)))))

(defn holds?
  "Is grant-id one of the principal's active grants listing op? Used for Field visibility (6.1 step 4)."
  [db principal-id grant-id op]
  (let [holders (conj (groups-of db principal-id) principal-id)]
    (boolean (some #(and (= grant-id (:object/id %)) (contains? (:grant/ops %) op))
                   (active-grants db holders (Date.))))))

(defn visible-fields
  "Fields of an object the principal may see for op: no visibility Grant, or one it holds."
  [db principal-id fields op]
  (into {} (filter (fn [[_ f]] (let [v (:field/visibility f)] (or (nil? v) (holds? db principal-id v op)))) fields)))

(def public-fields
  "Fields of the world any session reads without a Grant. Only the Field: the world's other events
   name Objects a Principal may have no Grant on."
  #{:world/unsafe :world/notifications :world/release :world/manual})

(defn public-field?
  [db object field]
  (and (contains? public-fields field) (= object (objects/world-id db))))

(defn public-event?
  "A change to a public Field, which the honest filter lets through."
  [db {:keys [event/kind event/stream event/body]}]
  (and (= :field-set kind) (public-field? db stream (:name body))))

(defn- resolved! [log principal-id n object grant op]
  (let [seq (log/append!! log (log/pending :resolved principal-id principal-id
                                           {:name n :object object :grant grant :op op}
                                           :streams #{object} :delegation grant))]
    {:object object :grant grant :seq seq}))

(defn resolve!
  "Resolution as an access: on success appends `resolved` and returns {:object :grant :seq}; else nil.
   A mounted name materializes its Object first (or refreshes it when the disk changed)."
  [{:keys [log]} db principal-id n op]
  (let [now (Date.)]
    (if-let [object (lookup db principal-id n)]
      (when-let [grant (resolves? db principal-id n op now)]
        (when-let [m (mount/backing-mount db (namespace-chain db principal-id) object)]
          (mount/refresh! log db principal-id grant object m))
        (resolved! log principal-id n object grant op))
      (when-let [{:keys [mount grant]} (mount-grant db principal-id n op now)]
        (when-let [object (mount/materialize! log db principal-id grant (objects/world-id db) mount)]
          (resolved! log principal-id n object grant op))))))
