(ns knowledge.world.ws-client
  "A JDK java.net.http.WebSocket client speaking either wire form. Accumulates partial text
   frames until `last`, decodes, queues every frame, and completes a promise per request id.
   `request` sends eagerly and returns the promise; `await-frame` pulls from the queue."
  (:require [knowledge.world.protocol.codec :as codec]
            [knowledge.world.protocol.frames :as frames])
  (:import [java.net URI]
           [java.net.http HttpClient WebSocket WebSocket$Listener]
           [java.util.concurrent CompletableFuture LinkedBlockingQueue TimeUnit]))

(defn connect
  "Open a client to ws://127.0.0.1:port/ws?format=<format>. Returns the client map."
  [port format]
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
                         (let [frame (try (codec/decode format s) (catch Exception e {::undecodable s ::error (ex-message e)}))
                               frame (if-let [{:keys [op]} (and (contains? frame :ok) (get @pending (:id frame)))]
                                       (update frame :result #(codec/decode-result format op %))
                                       frame)]
                           (.put queue frame)
                           (when-let [{:keys [p]} (and (contains? frame :ok) (get @pending (:id frame)))]
                             (deliver p frame)))))
                     (.request ws 1)
                     nil)
                   (onClose [_ _ status reason] (deliver closed {:status status :reason reason}) nil)
                   (onError [_ _ t] (deliver closed {:error t}) nil))
        ws (-> (HttpClient/newHttpClient)
               (.newWebSocketBuilder)
               (.buildAsync (URI. (str "ws://127.0.0.1:" port "/ws?format=" (name format))) listener)
               (.get 5 TimeUnit/SECONDS))]
    {:ws ws :queue queue :pending pending :format format :closed closed :ids (atom 0)}))

(defn send-raw! [{:keys [^WebSocket ws]} ^String s]
  (.get ^CompletableFuture (.sendText ws s true) 5 TimeUnit/SECONDS))

(defn send-frame! [{:keys [format] :as client} frame]
  (send-raw! client (codec/encode format frame)))

(defn request
  "Send {id op args}; returns a promise of the response frame. id is generated when absent."
  ([client op args] (request client (str "r" (swap! (:ids client) inc)) op args))
  ([{:keys [pending] :as client} id op args]
   (let [p (promise)]
     (swap! pending assoc id {:p p :op op})
     (send-frame! client {:id id :op op :args args})
     p)))

(defn request!!
  "request and wait up to timeout-ms (default 5000) for the response."
  ([client op args] (request!! client op args 5000))
  ([client op args timeout-ms]
   (let [r (deref (request client op args) timeout-ms ::timeout)]
     (when (= r ::timeout) (throw (ex-info "no response" {:op op :args args})))
     r)))

(defn ack! [client sub seq] (send-frame! client {:ack sub :seq seq}))

(defn await-frame
  "The next queued frame satisfying pred within timeout-ms, or nil. Non-matching frames are dropped."
  ([client pred] (await-frame client pred 5000))
  ([{:keys [^LinkedBlockingQueue queue]} pred timeout-ms]
   (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
     (loop []
       (let [left (- deadline (System/currentTimeMillis))]
         (when (pos? left)
           (if-let [f (.poll queue left TimeUnit/MILLISECONDS)]
             (if (pred f) f (recur))
             nil)))))))

(defn push? [f] (= :push (frames/kind f)))

(defn await-push
  "Next push for sub within timeout."
  ([client sub] (await-push client sub 5000))
  ([client sub timeout-ms] (await-frame client #(and (push? %) (= sub (:sub %))) timeout-ms)))

(defn collect-pushed-seqs
  "Keep taking pushes for sub until the seqs reach `upto` or `timeout-ms` passes; returns the seqs in order."
  [client sub upto timeout-ms]
  (let [deadline (+ (System/currentTimeMillis) timeout-ms)]
    (loop [seqs []]
      (if (or (>= (or (peek seqs) -1) upto) (> (System/currentTimeMillis) deadline))
        seqs
        (if-let [p (await-push client sub (max 1 (- deadline (System/currentTimeMillis))))]
          (recur (into seqs (map :event/seq (:events p))))
          seqs)))))

(defn abort!
  "Drop the socket without detach."
  [{:keys [^WebSocket ws]}]
  (.abort ws))

(defn close!
  "Polite close."
  [{:keys [^WebSocket ws]}]
  (try (.get ^CompletableFuture (.sendClose ws WebSocket/NORMAL_CLOSURE "bye") 2 TimeUnit/SECONDS)
       (catch Exception _ nil)))
