(ns knowledge.world.tokens
  "Tokens as Objects (spec 5.9, 14.2). A credential is an Object the world records
   when a secret is minted: token/principal (whom it authenticates), token/hash (sha256 of the
   secret; the secret itself is never a Field, never on the log), token/label (which device or
   purpose), token/issued-by, token/issued-at, optional token/expires-at, token/last-used (written at
   most once a minute), and token/revoked-at with who and why. `attach` accepts any unrevoked,
   unexpired token Object for the Principal; the legacy single principal/token-hash still
   authenticates and is turned into a token Object labelled legacy at upgrade. The world is the actor
   of these records, as it is for the other bookkeeping Fields. This namespace depends on nothing in
   act.core so create! can record a token at birth without a cycle."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.index.core :as index]
            [knowledge.world.log.core :as log]
            [knowledge.world.protocol.session :as session]
            [taoensso.timbre :as timbre])
  (:import [java.nio.file Files]
           [java.nio.file.attribute PosixFilePermissions]
           [java.util Date]))

(def types
  {:token/principal [:ref :principal] :token/hash :string :token/label :string
   :token/issued-by [:ref :principal] :token/issued-at :instant :token/expires-at :instant
   :token/last-used :instant :token/revoked-at :instant :token/revoked-by [:ref :principal]
   :token/revoked-reason :string :token/paired-from [:ref :object]})

(def touch-every-ms 60000)

(defn db [world] (index/db (:index world)))
(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- note!
  "A bookkeeping Field on a token Object, the world as actor."
  [world token k v]
  (log/append!! (:log world) (log/pending :field-set token (:world-id world)
                                          {:name k :type (types k) :value v :reader :both})))

(defn record-hash!
  "A token Object for a secret already hashed (the migration path for legacy hashes). Returns the id."
  [world {:keys [principal hash label issued-by issued-at ttl-s paired-from]}]
  (let [id (random-uuid)
        now (or issued-at (Date.))
        fields (cond-> {:token/principal principal :token/hash hash :token/label (or label "token")
                        :token/issued-by (or issued-by (:world-id world)) :token/issued-at now}
                 (and (number? ttl-s) (pos? ttl-s)) (assoc :token/expires-at (Date. (+ (.getTime ^Date now) (* 1000 (long ttl-s)))))
                 paired-from (assoc :token/paired-from paired-from))]
    (log/append!! (:log world) (log/pending :created id (:world-id world) {:class :object :fields fields}
                                            :streams #{principal}))
    id))

(defn record!
  "A token Object for a freshly minted secret. Returns the id. The secret goes to the caller only."
  [world {:keys [secret] :as args}]
  (record-hash! world (-> args (dissoc :secret) (assoc :hash (session/token-hash secret)))))

(defn mint!
  "A new secret and its token Object: {:token id :secret}."
  [world args]
  (let [secret (session/new-token)]
    {:token (record! world (assoc args :secret secret)) :secret secret}))

(defn tokens-of
  "The token Objects of a Principal, oldest first."
  [db principal]
  (->> (objects/objects-with-field db :token/principal)
       (filter #(= principal (field db % :token/principal)))
       (sort-by #(or (some-> ^Date (field db % :token/issued-at) .getTime) 0))
       vec))

(defn status
  "active | expired | revoked, now."
  [db token ^Date now]
  (cond (some? (field db token :token/revoked-at)) :revoked
        (when-let [^Date e (field db token :token/expires-at)] (not (.after e now))) :expired
        :else :active))

(defn- legacy-principal
  "The one Principal whose principal/token-hash matches the hash, else nil."
  [db h]
  (let [matches (for [id (objects/objects-with-field db :principal/token-hash)
                      :let [v (field db id :principal/token-hash)]
                      :when (and (string? v) (session/hash= v h))]
                  id)]
    (when (= 1 (count matches)) (first matches))))

(defn authenticate
  "Whom a secret authenticates, now: {:principal id :token token-object-or-nil}, or {:err reason}
   where reason is \"expired\", \"revoked\" or \"token does not authenticate\". Token Objects are
   tried first; a Principal's legacy principal/token-hash answers with :token nil."
  [db secret ^Date now]
  (let [h (session/token-hash (str secret))
        tokens (filter #(session/hash= (field db % :token/hash) h) (objects/objects-with-field db :token/hash))]
    (if (seq tokens)
      (if-let [t (first (filter #(= :active (status db % now)) tokens))]
        {:principal (field db t :token/principal) :token t}
        {:err (name (status db (first tokens) now))})
      (if-let [p (legacy-principal db h)]
        {:principal p :token nil}
        {:err "token does not authenticate"}))))

(defn touch!
  "token/last-used, at most once a minute (every attach would otherwise be a Field write)."
  [world token ^Date now]
  (when token
    (let [^Date last (field (db world) token :token/last-used)]
      (when (or (nil? last) (> (- (.getTime now) (.getTime last)) touch-every-ms))
        (note! world token :token/last-used now)))))

(defn revoke!
  "token/revoked-at, -by, -reason on a token Object; the world as actor. Returns the seq."
  [world token by reason ^Date now]
  (note! world token :token/revoked-by by)
  (when reason (note! world token :token/revoked-reason (str reason)))
  (note! world token :token/revoked-at now))

(defn revoke-all!
  "Every active token Object of a Principal (a fork or an import re-mints: the parent's secrets must
   not open the child). Returns the ids revoked."
  [world principal by reason]
  (let [db (db world) now (Date.)]
    (vec (for [t (tokens-of db principal) :when (= :active (status db t now))]
           (do (revoke! world t by reason now) t)))))

(defn secret-file!
  "A secret written for a loop that may not receive it: data/tokens/<label>-<short id>.token, 0600.
   Returns the path."
  [world label token secret]
  (let [dir (io/file (:data-dir world) "tokens")
        _ (.mkdirs dir)
        n (str (str/replace (str (or label "token")) #"[^A-Za-z0-9._-]" "-") "-" (subs (str token) 0 8) ".token")
        f (io/file dir n)]
    (spit f secret)
    (try (Files/setPosixFilePermissions (.toPath f) (PosixFilePermissions/fromString "rw-------"))
         (catch Exception _ nil))
    (.getPath f)))

(defn legacy-without-object
  "Principals whose principal/token-hash has no token Object carrying the same hash."
  [db]
  (let [hashes (set (map #(field db % :token/hash) (objects/objects-with-field db :token/hash)))]
    (vec (sort-by str (for [p (objects/objects-with-field db :principal/token-hash)
                            :let [h (field db p :principal/token-hash)]
                            :when (and (string? h) (not (contains? hashes h)))]
                        p)))))

(defn migrate-legacy!
  "A token Object labelled legacy for every Principal whose single hash has none (upgrade of a
   world born before tokens were Objects). Returns {principal token}."
  [world]
  (let [db (db world)]
    (into {} (for [p (legacy-without-object db)]
               (do (timbre/info "token object recorded for a legacy hash" {:principal p})
                   [p (record-hash! world {:principal p :hash (field db p :principal/token-hash) :label "legacy"
                                           :issued-by (:world-id world)
                                           :issued-at (or (get-in (objects/fields db p) [:principal/token-hash :field/set-at]) (Date.))})])))))
