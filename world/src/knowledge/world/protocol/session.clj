(ns knowledge.world.protocol.session
  "Sessions (protocol.md Session; spec 14.2). A session is transport state keyed by id, held in
   memory: principal, format, channel, subscriptions, and the time its socket closed. A closed
   session survives `ttl-ms` for reattach; `attached`/`detached` are the events, the record is not."
  (:require [knowledge.world.log.blob :as blob])
  (:import [java.security MessageDigest SecureRandom]
           [java.util Base64]))

(def ttl-ms 300000)

(defn registry [] (atom {}))

(defn new-token
  "A bearer token: 32 random bytes, base64url."
  []
  (let [bs (byte-array 32)]
    (.nextBytes (SecureRandom.) bs)
    (.encodeToString (.withoutPadding (Base64/getUrlEncoder)) bs)))

(defn token-hash ^String [^String token] (blob/sha256-hex (.getBytes token "UTF-8")))

(defn hash= [^String a ^String b]
  (and a b (MessageDigest/isEqual (.getBytes a "UTF-8") (.getBytes b "UTF-8"))))

(defn new-id [] (str "s-" (subs (str (random-uuid)) 0 13)))

(defn create!
  "A fresh session atom registered under a new id."
  [registry principal format client-kind]
  (let [id (new-id)
        s (atom {:id id :principal principal :format format :client-kind client-kind
                 :channel nil :subs {} :detached-at nil
                 :pump (atom {:scheduled? false :last-run 0})})]
    (swap! registry assoc id s)
    s))

(defn lookup [registry id] (get @registry id))
(defn remove! [registry id] (swap! registry dissoc id))

(defn attach-channel! [session channel format]
  (swap! session assoc :channel channel :format format :detached-at nil))

(defn mark-detached! [session]
  (swap! session assoc :channel nil :detached-at (System/currentTimeMillis)))

(defn attached? [session] (some? (:channel @session)))

(defn restore-subs!
  "Reattach: each named sub's cursor and acked go to the client's acked seq (ADR 0004)."
  [session acked-by-sub]
  (swap! session update :subs
         (fn [subs]
           (reduce-kv (fn [m sub-id acked]
                        (if (contains? m sub-id)
                          (update m sub-id assoc :cursor acked :pending [] :lagging? false)
                          m))
                      subs acked-by-sub))))

(defn expired?
  [session now]
  (when-let [t (:detached-at @session)] (> (- now t) ttl-ms)))

(defn expire!
  "Drop sessions whose socket closed more than ttl ago. Returns the dropped ids."
  [registry]
  (let [now (System/currentTimeMillis)
        dead (for [[id s] @registry :when (expired? s now)] id)]
    (doseq [id dead] (swap! registry dissoc id))
    (vec dead)))

(defn attached-sessions [registry]
  (filter attached? (vals @registry)))
