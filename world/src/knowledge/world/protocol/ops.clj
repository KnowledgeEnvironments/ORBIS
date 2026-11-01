(ns knowledge.world.protocol.ops
  "The ops (protocol.md Ops; spec 14.3) as a multimethod on :op. Every op goes through resolution
   (ground rule 4) and every write is an event (ground rule 3). The default answers
   err not-implemented so the surface is enumerable from day one. A handler returns
   {:ok result} or {:err {:code :message}}; the server adds id and seq.

   The work of the write ops lives in knowledge.world.act.core, knowledge.world.route.core,
   knowledge.world.serialize.core and knowledge.world.evaluate.core, so kernel-zero and the
   resident loop take the same path as a client. ctx is {:world world :conn conn} where conn
   is an atom {:channel :format :session}."
  (:require [clojure.string :as str]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.conversation :as conversation]
            [knowledge.world.act.handlers]
            [knowledge.world.act.history :as history]
            [knowledge.world.act.perform :as perform]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.index.core :as index]
            [knowledge.world.log.core :as log]
            [knowledge.world.refs :as refs]
            [knowledge.world.loop.steer :as steer]
            [knowledge.world.pane.actions]
            [knowledge.world.image-actions]
            [knowledge.world.pane.core :as pane]
            [knowledge.world.protocol.frames :as frames]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.protocol.subscription :as subscription]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]
            [knowledge.world.schedule.core :as schedule]
            [knowledge.world.search.core :as search]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.tokens :as tokens]
            [knowledge.world.auth :as auth]
            [knowledge.world.view.core :as view]
            [taoensso.timbre :as timbre])
  (:import [java.util Date]))

(def err act/err)
(def unresolved act/unresolved)
(def unauthenticated (err :unauthenticated "attach first"))

(defn- db [ctx] (index/db (get-in ctx [:world :index])))
(defn- session-of [ctx] (:session @(:conn ctx)))
(defn- principal-of [ctx] (some-> (session-of ctx) deref :principal))

(defn- append! [ctx event] (log/append!! (get-in ctx [:world :log]) event))

(defmulti handle (fn [_ctx op _args] op))

(defmethod handle :default [_ op _]
  (err :not-implemented (frames/not-implemented-message op)))

(defmacro ^:private with-principal
  "Bind the session principal or answer unauthenticated."
  [[sym ctx] & body]
  `(if-let [~sym (principal-of ~ctx)] (do ~@body) unauthenticated))

;; ---------------------------------------------------------------- session

(defmethod handle :attach [{:keys [world conn] :as ctx} _ {:keys [token principal session subs client-kind]}]
  ;; any unrevoked, unexpired token Object for the Principal, or its legacy
  ;; principal/token-hash; the attached event names the token Object, so the audit says which device
  (let [db (db ctx)
        now (Date.)
        {found :principal token-object :token reason :err} (tokens/authenticate db token now)
        client-kind (or client-kind :test)
        format (:format @conn)]
    (cond
      (nil? found) (err :unauthenticated (if (= reason "token does not authenticate") reason (str "token " reason)))
      (and principal (not= principal found)) (err :unauthenticated "token does not authenticate")
      (objects/retired? db found) (err :unauthenticated "that Principal is retired")
      :else
      (let [registry (:sessions world)
            known (when session (session/lookup registry session))
            resumed? (boolean (and known (= found (:principal @known))
                                   (not (session/expired? known (System/currentTimeMillis)))))
            s (if resumed? known (session/create! registry found format client-kind))]
        (when-let [old (:session @conn)] (when (not= old s) (session/mark-detached! old)))
        (when-let [old-ch (:channel @s)]
          (when (not= old-ch (:channel @conn))
            (try ((requiring-resolve 'org.httpkit.server/close) old-ch) (catch Exception _ nil))))
        (session/attach-channel! s (:channel @conn) format)
        ;; the session remembers its token Object, so revoking the token closes it
        (swap! s assoc :token token-object)
        (when resumed? (session/restore-subs! s (or subs {})))
        (swap! conn assoc :session s)
        (append! ctx (log/pending :attached found found (cond-> {:session (:id @s) :client-kind client-kind :format format}
                                                          token-object (assoc :token token-object))))
        (tokens/touch! world token-object now)
        (subscription/wake! world s)
        {:ok {:session (:id @s) :principal found :resumed resumed? :seq (log/head (:log world))}}))))

;; ---------------------------------------------------------------- before attach

(defn- world-field [ctx k] (get-in (objects/fields (db ctx) (get-in ctx [:world :world-id])) [k :field/value]))

(defmethod handle :hello [{:keys [world] :as ctx} _ _]
  ;; which world is this: id, name, URL, format versions, head; a client shows a picker and checks
  ;; that a saved session belongs here before it reattaches
  {:ok {:world (:world-id world)
        :name (str (or (world-field ctx :world/name) "world"))
        :url (str (or (world-field ctx :world/url)
                      (str "ws://" (or (try (.getHostName (java.net.InetAddress/getLocalHost)) (catch Exception _ nil)) "127.0.0.1")
                           ":" (or (:port world) (some-> (:port* world) (deref 0 nil))) "/ws")))
        :format-versions {:log 1 :protocol "v0" :json 1 :transit 1}
        :head (log/head (:log world))
        :root (objects/root-id (db ctx))}})

(def pair-max-failures 5)
(def pair-window-ms 60000)

(defmethod handle :pair.claim [{:keys [world conn] :as ctx} _ {:keys [pairing-code label]}]
  ;; a device trades a live pairing code for a credential of the pairing's Principal; five wrong
  ;; codes within a minute on one socket end it (the socket is closed after the answer)
  (let [now (System/currentTimeMillis)
        fails (vec (filter #(> % (- now pair-window-ms)) (:pair-fails @conn)))]
    (if (>= (count fails) pair-max-failures)
      (do (swap! conn assoc :pair-fails fails :close-after-reply true)
          (err :rate-limited "too many wrong pairing codes on this socket"))
      (if-let [r (auth/claim! world {:code pairing-code :label label})]
        (do (swap! conn assoc :pair-fails [])
            (timbre/info "pairing claimed" {:principal (:principal r) :token (:token r)})
            {:ok r})
        (do (swap! conn assoc :pair-fails (conj fails now))
            (err :unauthenticated "no live pairing matches that code"))))))

(defmethod handle :detach [{:keys [world conn] :as ctx} _ _]
  (if-let [s (session-of ctx)]
    (let [p (:principal @s)]
      (append! ctx (log/pending :detached p p {:session (:id @s) :reason "detach"}))
      (session/remove! (:sessions world) (:id @s))
      (swap! s assoc :channel nil)
      (swap! conn dissoc :session)
      {:ok {}})
    unauthenticated))

(defn socket-closed!
  "The socket went away without detach: append detached, keep the session for its TTL."
  [world conn]
  (when-let [s (:session @conn)]
    (when (= (:channel @s) (:channel @conn))
      (let [p (:principal @s)]
        (session/mark-detached! s)
        (try (pane/drop-session! world s)
             (catch Exception e (timbre/warn e "pane detach failed")))
        (try (log/append!! (:log world) (log/pending :detached p p {:session (:id @s) :reason "socket-closed"}))
             (catch Exception e (timbre/warn e "detached event failed")))))))

;; ---------------------------------------------------------------- subscriptions

(defmethod handle :subscribe [{:keys [world] :as ctx} _ args]
  (if-let [s (session-of ctx)]
    (let [head (log/head (:log world))
          sub (subscription/create args head)]
      (swap! s assoc-in [:subs (:sub args)] sub)
      (subscription/wake! world s)
      {:ok {:sub (:sub args) :as-of head}})
    unauthenticated))

(defmethod handle :unsubscribe [ctx _ {:keys [sub]}]
  (if-let [s (session-of ctx)]
    (do (swap! s update :subs dissoc sub) {:ok {}})
    unauthenticated))

;; ---------------------------------------------------------------- reads

(defmethod handle :resolve [{:keys [world] :as ctx} _ {:keys [name op]}]
  ;; spec 6.1: the Object or nothing; a miss and a refusal are the same nothing
  (with-principal [p ctx]
    (let [db (db ctx)]
      (if-let [{:keys [object]} (resolve/resolve! world db p name op)]
        {:ok {:object object :class (:object/class (objects/state db object))}}
        {:ok nil}))))

(defmethod handle :read [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx] (act/read! world p args)))

(defmethod handle :history [{:keys [world] :as ctx} _ {:keys [stream after limit] :as args}]
  (with-principal [p ctx]
    (if-not stream
      ;; the whole log, event by event through resolution, and the snapshot read
      (history/history! world p args)
      (let [db (db ctx) limit (min history/max-limit (or limit 256))]
        (if (resolve/resolve! world db p (act/id-name stream) :read)
          (let [events (log/read-stream (:log world) stream (or after 0) limit)]
            {:ok (cond-> {:events (mapv #(refs/visible-event db p %) events)}
                   (= (count events) limit) (assoc :next (:event/seq (peek events))))})
          unresolved)))))

;; search over what the principal may read, one resolved summary event
(defmethod handle :search [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx] (search/search! world p args)))

;; ---------------------------------------------------------------- writes

;; via never comes from the wire: a session has none; the resident loop's ctx carries the Evaluation
;; its ops come from (spec 9.8 step 6), so Fields set and Messages sent by a model cite it.
(defn- person-session?
  "A socket session of a person-driven Principal: never a loop, an approved op or a process's session."
  [ctx p]
  (authority/person-session? (act/db (:world ctx)) p (not (:loop? ctx))))

(defmethod handle :set-field [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx]
    (or (authority/write-refusal (:field args) (person-session? ctx p))
        (act/set-field! world p (assoc args :field (authority/field-key (:field args)) :via (:via ctx))))))

(defmethod handle :move [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx] (act/move! world p (assoc args :via (:via ctx)))))

(defmethod handle :bind [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx] (act/bind! world p args)))

(defmethod handle :unbind [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx] (act/unbind! world p args)))

(defmethod handle :grant [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx] (act/grant! world p args)))

(defmethod handle :revoke [{:keys [world] :as ctx} _ args]
  ;; a person's standing goes only by a person's hand: their own session, or their own card
  (with-principal [p ctx]
    (let [db (act/db world)]
      (if (and (authority/person-grant? db (:grant args))
               (not (authority/by-a-person? db p (not (:loop? ctx)) (:approved ctx))))
        (err :person-only "a Grant a person holds or issued is revoked only by a person")
        (act/revoke! world p args)))))

;; ---------------------------------------------------------------- triggers

(defmethod handle :arm [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx] (schedule/arm! world p args)))

(defmethod handle :disarm [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx] (schedule/disarm! world p args)))

;; ---------------------------------------------------------------- messages, serialization, evaluation

(defn- client-of
  "Which client a session's Message came from: its session id and its token's label (none on a
   legacy token). A loop stands in for a session and has no client. Decided here, never taken
   from the op's args, so a sender cannot name another device."
  [ctx]
  (when-not (:loop? ctx)
    (when-let [s (session-of ctx)]
      (let [{:keys [id token]} @s
            label (when token (get-in (objects/fields (db ctx) token) [:token/label :field/value]))]
        (cond-> {:session id} (string? label) (assoc :device label))))))

(defn- with-client [ctx args] (merge (dissoc args :device :session) (client-of ctx)))

(defmethod handle :send [{:keys [world] :as ctx} _ args]
  (with-principal [p ctx]
    (let [a (assoc (with-client ctx args) :via (:via ctx) :hop (:hop ctx) :thread (:thread ctx))
          ;; a person's @ mention: what the spans name is what the Message tags
          a (if (and (:refs a) (not (:loop? ctx)))
              (update a :tags #(vec (distinct (into (vec %) (keep :object) (:refs a)))))
              a)
          ;; a directive is the kernel's whatever the sender asked: never queued, never steered to a model
          steer? (and (:steer a) (not (route/directive-line? (act/db world) p (:text a))))
          a (dissoc a :steer)
          r (if steer? (steer/send! world p a) (route/send! world p a))]
      ;; a directive goes to the kernel, never to a model (decisions 2026-10-31)
      (when-let [d (get-in r [:ok :directive])] (conversation/handle-directive! world p d))
      (update r :ok #(when % (dissoc % :directive))))))

(defmethod handle :present [{:keys [world] :as ctx} _ args]
  ;; a view Serialization sent as a Message of kind :view; the client renders it
  (with-principal [p ctx] (view/present! world p (assoc (with-client ctx args) :via (:via ctx) :hop (:hop ctx)))))

(defmethod handle :serialize [{:keys [world] :as ctx} _ {:keys [working-set model depth]}]
  ;; spec 14.3: a dry run; the Model must resolve for :evaluate like an Evaluation's would
  (with-principal [p ctx]
    (let [db (db ctx)]
      (if-let [{:keys [grant]} (resolve/resolve! world db p (act/id-name model) :evaluate)]
        (let [{:keys [ok err]} (ser/serialize! world {:principal p :working-set working-set :model model :depth depth :grant grant})]
          (if err {:err err} {:ok (select-keys ok [:serialization :size :hash])}))
        unresolved))))

(defmethod handle :evaluate [{:keys [world] :as ctx} _ {:keys [working-set model route policy depth]}]
  (with-principal [p ctx]
    (let [{:keys [ok err]} (evaluate/evaluate! world {:principal p :working-set working-set :model model
                                                      :route route :policy policy :depth depth})]
      (if err {:err err} {:ok {:evaluation (:evaluation ok)}}))))

;; ---------------------------------------------------------------- act

(def creatable act/creatable)
(def schema-name act/schema-name)

(defmethod handle :act [{:keys [world] :as ctx} _ {:keys [class object action args]}]
  ;; ADR 0008: create is the action on a class; an action on an Object (an Adapter's run or index,
  ;; an Evaluation's cancel) goes through knowledge.world.act.perform; an action with neither class
  ;; nor object is a named action type (plan §19.1 W3), knowledge.world.act.actions. A Principal's
  ;; token is returned only to a session, never into a loop's results (decisions 2026-10-30).
  (with-principal [p ctx]
    (let [session? (not (:loop? ctx))]
      (cond (and (nil? class) (nil? object) (#{:scratch.begin :scratch.send :scratch.stop :scratch.end} action))
            ((requiring-resolve 'knowledge.world.scratch.core/perform!) ctx p action (or args {}))
            ;; a push stream belongs to a socket, so a loop has none: it reads with pane.read
            (and (nil? class) (nil? object) (#{:pane.subscribe :pane.unsubscribe} action))
            (if-let [s (when-not (:loop? ctx) (some-> (:conn ctx) deref :session))]
              (if (= :pane.subscribe action)
                (pane/subscribe! world s p (or args {}))
                (pane/unsubscribe! world s (or args {})))
              (err :session-only "a stream is for a session; a model reads with pane.read"))
            (= action :create) (act/create! world p class args {:return-token? session?})
            object (perform/perform! world p {:object object :action action :args (or args {}) :via (:via ctx)})
            action (actions/perform-action! world p action (or args {}) {:via (:via ctx) :session? session?
                                                                         :message (:message ctx) :route (:route ctx)})
            :else (err :not-implemented "act needs an action")))))

;; ---------------------------------------------------------------- a session's ops and the gate

(defn- session-gate
  "What a loop's op meets, a session's op meets (decisions 2026-10-31): a token held by a process is
   no way round a gate. A person's own session is a person acting, so it passes; so does the
   operator's key. Answers as handle would, or nil when the op goes on."
  [{:keys [world conn] :as ctx} op args]
  (when-let [p (principal-of ctx)]
    (let [db (act/db world)
          args (gates/pane-form op args)]
      (when-not (authority/passes-session-gate? db p {:scratch? (:scratch world) :session? (not (:loop? ctx))
                                                      :token (some-> conn deref :session deref :token)})
        (cond
          (and (= :act op) (map? args) (:action args) (nil? (:object args)) (actions/preflight world db p args))
          (actions/preflight world db p args)
          (and (= :grant op) (map? args) (not (act/grant-resolves? db p args)))
          act/unresolved
          (gates/gated? db p op args)
          (if-let [rule (gates/admission db p op args nil)]
            (gates/admit! world p rule {:op op :args args})
            ;; gate! answers the card, or {:err} when none could be made: either way the op did not run
            (let [e (gates/gate! world p {:op op :args args})]
              (if (:err e)
                e
                {:err (assoc e :data {:task (str (:task e)) :summary (:summary e)})}))))))))

(defn request
  "An op from a socket: the gate, then the handler. The loop calls handle, having met the gate itself."
  [ctx op args]
  (or (session-gate ctx op args) (handle ctx op args)))

(def implemented (set (keys (methods handle))))
