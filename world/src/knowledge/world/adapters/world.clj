(ns knowledge.world.adapters.world
  "An Adapter of kind :world (spec 6.5, 11.5): another world, reached over its own
   WebSocket protocol as a Principal there. adapter/locator is the remote ws URL; the credential is
   the environment variable adapter/key-env names, or the file adapter/token-file names (never a
   Field). `send-and-await!` attaches, resolves the recipient by name on the remote world, picks a
   Route both ends share, sends, and waits for the answer in-reply-to; the bridge Principal
   `bridge.<name>` relays it here as a Message. Minimal by design: one round trip, no retries; a
   failure is a reason in the result and on the log, never a crash."
  (:require [clojure.string :as str]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.protocol.codec :as codec]
            [knowledge.world.protocol.frames :as frames]
            [taoensso.timbre :as timbre])
  (:import [java.net URI]
           [java.net.http HttpClient WebSocket WebSocket$Listener]
           [java.util.concurrent CompletableFuture LinkedBlockingQueue TimeUnit]))

(def default-timeout-ms 120000)
(def connect-timeout-s 10)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn secret-of
  "The remote credential an Adapter names: $adapter/key-env, else the contents of adapter/token-file."
  [db adapter]
  (or (some-> (field db adapter :adapter/key-env) System/getenv str/trim not-empty)
      (some-> (field db adapter :adapter/token-file) slurp str/trim not-empty)))

;; ---------------------------------------------------------------- a small client

(defn- ws-url [locator]
  (let [u (str locator)]
    (cond (str/includes? u "format=") u
          (str/includes? u "?") (str u "&format=transit")
          :else (str u "?format=transit"))))

(defn connect
  "A JDK WebSocket to a world, Transit form. {:ws :queue :pending :ids :closed}."
  [locator]
  (let [queue (LinkedBlockingQueue.)
        pending (atom {})
        buf (StringBuilder.)
        closed (promise)
        listener (reify WebSocket$Listener
                   (onOpen [_ ws] (.request ws 1))
                   (onText [_ ws data last?]
                     (.append buf data)
                     (when last?
                       (let [s (str buf)]
                         (.setLength buf 0)
                         (let [frame (try (codec/decode :transit s) (catch Exception e {::undecodable (ex-message e)}))
                               frame (if-let [{:keys [op]} (and (contains? frame :ok) (get @pending (:id frame)))]
                                       (update frame :result #(codec/decode-result :transit op %))
                                       frame)]
                           (.put queue frame)
                           (when-let [{:keys [p]} (and (contains? frame :ok) (get @pending (:id frame)))]
                             (deliver p frame)))))
                     (.request ws 1)
                     nil)
                   (onClose [_ _ status reason] (deliver closed {:status status :reason reason}) nil)
                   (onError [_ _ t] (deliver closed {:error (ex-message t)}) nil))
        ws (-> (HttpClient/newHttpClient)
               (.newWebSocketBuilder)
               (.buildAsync (URI. (ws-url locator)) listener)
               (.get connect-timeout-s TimeUnit/SECONDS))]
    {:ws ws :queue queue :pending pending :ids (atom 0) :closed closed}))

(defn request!!
  "Send {id op args} and wait for the response frame."
  ([c op args] (request!! c op args 10000))
  ([{:keys [^WebSocket ws pending ids]} op args timeout-ms]
   (let [id (str "w" (swap! ids inc))
         p (promise)]
     (swap! pending assoc id {:p p :op op})
     (.get ^CompletableFuture (.sendText ws (codec/encode :transit {:id id :op op :args args}) true) 5 TimeUnit/SECONDS)
     (let [r (deref p timeout-ms ::timeout)]
       (when (= r ::timeout) (throw (ex-info (str "remote world did not answer " (name op)) {:op op})))
       r))))

(defn- next-push
  [{:keys [^LinkedBlockingQueue queue]} timeout-ms]
  (loop [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (let [left (- deadline (System/currentTimeMillis))]
      (when (pos? left)
        (if-let [f (.poll queue left TimeUnit/MILLISECONDS)]
          (if (= :push (frames/kind f)) f (recur deadline))
          nil)))))

(defn close! [{:keys [^WebSocket ws]}]
  (try (.get ^CompletableFuture (.sendClose ws WebSocket/NORMAL_CLOSURE "bye") 2 TimeUnit/SECONDS)
       (catch Exception _ nil)))

;; ---------------------------------------------------------------- the round trip

(defn- ok-or-throw [r what]
  (if (:ok r) (:result r) (throw (ex-info (str what ": " (get-in r [:err :code]) " " (get-in r [:err :message])) {:err (:err r)}))))

(defn- remote-id
  "A remote Principal or Route from an id string or a name the remote session resolves."
  [c v op]
  (if-let [id (and (string? v) (re-matches #"[0-9a-f-]{36}" v) (parse-uuid v))]
    id
    (:object (ok-or-throw (request!! c :resolve {:name (str v) :op op}) (str "resolve " v)))))

(defn- routes-of [c principal]
  (let [r (request!! c :read {:object principal})]
    (when (:ok r)
      (vec (get-in r [:result :object/fields :principal/routes :field/value])))))

(defn send-and-await!
  "Attach to the remote world as the Adapter's credential, send `text` to `to` (a remote name or id,
   default its root)
   on `route` (a remote name or id; default a Route the two share, else the sender's first), and
   wait up to timeout-ms for a Message in reply. Returns {:remote-principal :remote-message
   :remote-route :reply {:message :from :text} | nil :elapsed-ms} or {:err {:code :message}}."
  [db adapter {:keys [to route text timeout-ms]}]
  (let [locator (field db adapter :adapter/locator)
        secret (secret-of db adapter)
        started (System/currentTimeMillis)
        timeout-ms (long (or timeout-ms default-timeout-ms))]
    (cond
      (str/blank? (str locator)) {:err {:code :invalid :message "the world Adapter has no locator"}}
      (nil? secret) {:err {:code :unauthenticated :message "no credential: set the variable adapter/key-env names, or adapter/token-file"}}
      :else
      (let [c (try (connect locator) (catch Exception e {:connect-error (ex-message e)}))]
        (if (:connect-error c)
          {:err {:code :unresolved :message (str "cannot reach " locator ": " (:connect-error c))}}
          (try
            (let [me (:principal (ok-or-throw (request!! c :attach {:token secret :client-kind :adapter}) "attach"))
                  ;; no `to`: the other world's root, by id from hello, so a rename there changes nothing;
                  ;; a world too old to say answers to the name
                  to-id (if to
                          (remote-id c to :transmit)
                          (or (:root (:result (request!! c :hello {}))) (remote-id c "root" :transmit)))
                  mine (routes-of c me)
                  theirs (routes-of c to-id)
                  route-id (cond route (remote-id c route :transmit)
                                 :else (or (first (filter (set theirs) mine)) (first mine) (first theirs)))
                  _ (when-not route-id (throw (ex-info "no Route shared with the recipient on the remote world" {})))
                  head (:seq (ok-or-throw (request!! c :subscribe {:sub "bridge" :streams #{me} :after 0 :window 256}) "subscribe"))
                  sent (ok-or-throw (request!! c :send {:to #{to-id} :route route-id :text (str text)}) "send")
                  message (:message sent)
                  reply (loop []
                          (let [left (- (+ started timeout-ms) (System/currentTimeMillis))]
                            (when (pos? left)
                              (if-let [push (next-push c left)]
                                (do (.get ^CompletableFuture (.sendText ^WebSocket (:ws c) (codec/encode :transit {:ack "bridge" :seq (or (:event/seq (last (:events push))) head)}) true) 5 TimeUnit/SECONDS)
                                    (or (some (fn [ev]
                                                (when (and (= :delivered (:event/kind ev)) (= me (get-in ev [:event/body :recipient])))
                                                  (let [m (get-in ev [:event/body :message])
                                                        r (request!! c :read {:object m})
                                                        f (get-in r [:result :object/fields])
                                                        v (fn [k] (get-in f [k :field/value]))]
                                                    (when (and (:ok r)
                                                               (or (= message (v :message/in-reply-to))
                                                                   (and (= to-id (v :message/from)) (not= m message))))
                                                      {:message m :from (v :message/from) :text (str (v :message/text))}))))
                                              (:events push))
                                        (recur)))
                                nil))))]
              (cond-> {:remote-principal me :remote-message message :remote-route route-id :reply reply
                       :elapsed-ms (- (System/currentTimeMillis) started)}
                (nil? reply) (assoc :timed-out true)))
            (catch Exception e
              (timbre/warn "world adapter round trip failed" {:locator locator :error (ex-message e)})
              {:err (or (:err (ex-data e)) {:code :invalid :message (or (ex-message e) "remote round trip failed")})})
            (finally (close! c))))))))
