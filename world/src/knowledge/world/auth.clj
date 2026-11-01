(ns knowledge.world.auth
  "Issuing, revoking and listing tokens, and phone pairing (spec 5.9, 14.2).
   Resolution: issuing or revoking a credential for a Principal needs :delegate or :write on that
   Principal (a person holds write on itself, so it pairs its own phone; root holds everything).
   A secret reaches a socket session only: a loop that issues one gets the token Object's id and
   the path of a 0600 file under data/tokens/ instead (decisions 2026-10-30 'tokens never travel
   through a loop'). Pairing: pair.begin mints a six-digit code (its hash on a pairing Object with
   an expiry); pair.claim {pairing-code label}, accepted on a socket before attach, trades a live code once for a fresh
   token of the pairing's Principal, so a phone needs the world's URL and the code, nothing else."
  (:require [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.tokens :as tokens])
  (:import [java.security SecureRandom]
           [java.util Date]))

(def pair-default-ttl-s 300)
(def pair-types {:pair/principal [:ref :principal] :pair/label :string :pair/code-hash :string
                 :pair/expires-at :instant :pair/issued-by [:ref :principal] :pair/issued-at :instant
                 :pair/claimed-at :instant :pair/token [:ref :object]})

(defn- fail! [code message] (throw (ex-info message {:err {:code code :message message}})))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- principal-arg
  "A Principal from an id or a name the caller resolves, else nil."
  [db p v]
  (let [id (or (->uuid v) (resolve/lookup db p (str v)))]
    (when (and id (= :principal (:object/class (objects/state db id)))) id)))

(defn- may-issue!
  "The Grant under which p may issue or revoke credentials for principal: :delegate or :write on it."
  [world db p principal]
  (or (resolve/resolve! world db p (act/id-name principal) :delegate)
      (resolve/resolve! world db p (act/id-name principal) :write)
      (fail! :unresolved "issuing a credential needs delegate or write on that Principal")))

(defn- name-of [db id]
  (let [wid (objects/world-id db) names (objects/names db id)]
    (or (some (fn [[ns n]] (when (= ns wid) n)) (sort-by second names))
        (some (fn [[_ n]] n) (sort-by second names))
        (subs (str id) 0 8))))

;; ---------------------------------------------------------------- tokens

(defn issue!
  "token.issue {principal label ttl-s?}: a new secret for principal. A session gets the secret in the
   result; a loop gets the file it was written to."
  [world p {:keys [principal label ttl-s]} {:keys [session?]}]
  (let [db (act/db world)
        target (or (principal-arg db p principal) (fail! :unresolved (str "no Principal " principal)))
        driven (:principal/driven-by (objects/state db target))]
    (when-not (#{:person :process} driven) (fail! :invalid-args "only a person or process Principal takes a credential"))
    (may-issue! world db p target)
    (let [{:keys [token secret]} (tokens/mint! world {:principal target :label label :issued-by p :ttl-s ttl-s})
          expires (field (act/db world) token :token/expires-at)]
      {:ok {:result (cond-> {:token token :principal target :label label}
                      expires (assoc :expires-at expires)
                      session? (assoc :secret secret)
                      (not session?) (assoc :secret-written-to (tokens/secret-file! world label token secret)))
            :object token}})))

(defn evict-sessions!
  "every live session that attached with the token gets a final err frame
   `unauthenticated \"token revoked\"` and its socket closed; the session is dropped so a resume with
   its id is refused too. Returns how many were closed. http-kit is reached by name: this namespace
   sits below the protocol layer."
  [world token]
  (let [registry (:sessions world)
        send! (requiring-resolve 'org.httpkit.server/send!)
        close (requiring-resolve 'org.httpkit.server/close)
        encode (requiring-resolve 'knowledge.world.protocol.codec/encode)
        head (log/head (:log world))]
    (count
      (for [[id s] @registry
            :when (= token (:token @s))]
        (let [ch (:channel @s) fmt (:format @s)]
          (when ch
            (try (send! ch (encode fmt {:id "revoked" :ok false :err {:code :unauthenticated :message "token revoked"} :seq head}))
                 (catch Exception _ nil))
            (try (close ch) (catch Exception _ nil)))
          (swap! registry dissoc id)
          id)))))

(defn revoke!
  "token.revoke {token reason?}: the Principal's issuer or writer revokes a token Object; live
   sessions on that token are closed."
  [world p {:keys [token reason]}]
  (let [db (act/db world)
        t (or (->uuid token) (fail! :invalid-args "token is a token Object id"))
        principal (field db t :token/principal)]
    (when-not principal (fail! :unresolved (str "no token " token)))
    (may-issue! world db p principal)
    (when (= :revoked (tokens/status db t (Date.))) (fail! :conflict "token is already revoked"))
    (let [seq (tokens/revoke! world t p reason (Date.))
          closed (evict-sessions! world t)]
      {:ok {:result {:token t :principal principal :revoked true :sessions-closed closed} :object t :seq seq}})))

(defn list-rows
  "The tokens p may see: those of `principal` (id or name) or of every Principal p reads, as table
   data {:columns :rows :about}."
  [world p {:keys [principal]}]
  (let [db (act/db world)
        now (Date.)
        principals (if principal
                     [(or (principal-arg db p principal) (fail! :unresolved (str "no Principal " principal)))]
                     (sort-by str (filter #(resolve/resolves? db p (act/id-name %) :read) (objects/objects-of-class db :principal))))
        fmt (fn [^Date d] (when d (str (.toInstant d))))
        rows (vec (for [pr principals
                        t (tokens/tokens-of db pr)
                        :let [f (objects/fields db t)
                              v (fn [k] (get-in f [k :field/value]))]]
                    [(v :token/label) (name-of db pr) (name (tokens/status db t now)) (fmt (v :token/issued-at))
                     (fmt (v :token/expires-at)) (fmt (v :token/last-used)) (fmt (v :token/revoked-at)) (str t)]))]
    {:columns [{:key "label" :label "label"} {:key "principal" :label "principal"} {:key "status" :label "status"}
               {:key "issued" :label "issued"} {:key "expires" :label "expires"} {:key "last-used" :label "last used"}
               {:key "revoked" :label "revoked"} {:key "token" :label "token"}]
     :rows rows
     :about (vec principals)}))

;; ---------------------------------------------------------------- pairing

(defn- new-code [] (format "%06d" (.nextInt (SecureRandom.) 1000000)))
(defn code-hash ^String [code] (blob/sha256-hex (.getBytes (str code) "UTF-8")))

(defn begin!
  "pair.begin {principal label ttl-s?}: a six-digit code a device trades for a token of principal
   within ttl-s (300). Session only: the code is a secret."
  [world p {:keys [principal label ttl-s]} {:keys [session?]}]
  (when-not session? (fail! :session-only "pairing codes are handed to a person at a listener, never to a loop"))
  (let [db (act/db world)
        target (or (principal-arg db p principal) (fail! :unresolved (str "no Principal " principal)))]
    (when-not (#{:person :process} (:principal/driven-by (objects/state db target)))
      (fail! :invalid-args "only a person or process Principal takes a credential"))
    (may-issue! world db p target)
    (let [code (new-code)
          ttl (if (and (number? ttl-s) (pos? ttl-s)) (long ttl-s) pair-default-ttl-s)
          now (Date.)
          expires (Date. (+ (.getTime now) (* 1000 ttl)))
          id (random-uuid)
          wid (:world-id world)
          fields {:pair/principal target :pair/label (or label "paired device") :pair/code-hash (code-hash code)
                  :pair/expires-at expires :pair/issued-by p :pair/issued-at now}]
      (log/append!! (:log world) (log/pending :created id wid {:class :object :fields fields} :streams #{target}))
      {:ok {:result {:pairing-code code :pairing id :principal target :expires-at expires
                     :world wid :url (field db wid :world/url) :name (field db wid :world/name)}
            :object id}})))

(defn claim!
  "pair.claim {code label?} on a socket before attach: a live, unclaimed pairing whose code hashes
   to the code is consumed and a fresh token for its Principal minted; the secret goes to this socket
   only. Returns the result map or nil when no pairing matches (the caller counts the miss)."
  [world {:keys [code label]}]
  (let [db (act/db world)
        now (Date.)
        h (code-hash (str/trim (str code)))
        live (for [id (objects/objects-with-field db :pair/code-hash)
                   :let [f (objects/fields db id)
                         v (fn [k] (get-in f [k :field/value]))]
                   :when (and (= h (v :pair/code-hash))
                              (nil? (v :pair/claimed-at))
                              (when-let [^Date e (v :pair/expires-at)] (.after e now)))]
               id)]
    (when-let [pairing (first live)]
      (let [principal (field db pairing :pair/principal)
            {:keys [token secret]} (tokens/mint! world {:principal principal :label (or label (field db pairing :pair/label))
                                                        :issued-by (field db pairing :pair/issued-by) :paired-from pairing})
            wid (:world-id world)]
        (log/append!! (:log world) (log/pending :field-set pairing wid {:name :pair/claimed-at :type :instant :value now :reader :both}))
        (log/append!! (:log world) (log/pending :field-set pairing wid {:name :pair/token :type [:ref :object] :value token :reader :both}))
        {:token token :secret secret :principal principal :world wid}))))
