(ns knowledge.world.protocol.server
  "http-kit WebSocket server (ADR 0001): ws://host:port/ws?format=transit|json. Every inbound
   text frame is decoded and validated against :ke/Frame; an invalid frame with an id gets
   err invalid, otherwise it is logged and dropped. Requests run on http-kit's worker threads;
   responses carry seq = the log head at reply time. On socket close the session is detached
   (event) and kept for its TTL."
  (:require [knowledge.world.log.core :as log]
            [knowledge.world.protocol.codec :as codec]
            [knowledge.world.protocol.frames :as frames]
            [knowledge.world.protocol.ops :as ops]
            [knowledge.world.protocol.subscription :as subscription]
            [org.httpkit.server :as hk]
            [taoensso.timbre :as timbre]))

(def max-message (* 4 1024 1024))

(defn- format-of [req default]
  (let [q (or (:query-string req) "")]
    (cond (re-find #"(?:^|&)format=json(?:&|$)" q) :json
          (re-find #"(?:^|&)format=transit(?:&|$)" q) :transit
          :else default)))

(defn- send-frame! [conn frame]
  (let [{:keys [channel format]} @conn]
    (hk/send! channel (codec/encode format frame))))

(defn- handle-request! [world conn {:keys [id op args]}]
  (let [ctx {:world world :conn conn}
        head #(log/head (:log world))
        frame (try (let [{:keys [ok err]} (ops/request ctx op args)]
                     (if err
                       (frames/err id (:code err) (:message err) (head) (:data err))
                       (frames/ok id ok (head))))
                   (catch Exception e
                     (timbre/error e "op failed" {:op op})
                     (frames/err id :invalid (or (ex-message e) "op failed") (head))))]
    (send-frame! conn frame)
    ;; a socket that guessed too many pairing codes is closed after the answer
    (when (:close-after-reply @conn)
      (swap! conn dissoc :close-after-reply)
      (try (hk/close (:channel @conn)) (catch Exception _ nil)))))

(defn- on-receive [world conn ^String msg]
  (let [format (:format @conn)
        frame (try (codec/decode format msg)
                   (catch Exception e
                     (timbre/debug "invalid frame" {:error (ex-message e) :explain (:explain (ex-data e))})
                     ::invalid))]
    (if (= frame ::invalid)
      (when-let [id (codec/raw-id format msg)]
        (send-frame! conn (frames/err id :invalid "frame does not validate" (log/head (:log world)))))
      (case (frames/kind frame)
        :request (handle-request! world conn frame)
        :ack (when-let [s (:session @conn)]
               (subscription/ack! world s (:ack frame) (:seq frame)))
        (timbre/debug "ignoring frame kind" {:kind (frames/kind frame)})))))

(defn- ws-handler [world default-format]
  (fn [req]
    (if (and (= "/ws" (:uri req)) (:websocket? req))
      (let [conn (atom {:channel nil :format (format-of req default-format) :session nil})]
        (hk/as-channel req
          {:on-open (fn [ch] (swap! conn assoc :channel ch))
           :on-receive (fn [_ msg]
                         (if (string? msg)
                           (on-receive world conn msg)
                           (timbre/debug "binary frame ignored")))
           :on-close (fn [_ status]
                       (timbre/debug "socket closed" {:status status})
                       (ops/socket-closed! world conn))}))
      {:status 404 :headers {"content-type" "text/plain"} :body "ws endpoint is /ws"})))

(defn start!
  "Run the server on bind (127.0.0.1 unless KE_BIND says otherwise); port 0 picks a free one. Returns
   the server; (port server) tells which."
  [world {:keys [port bind format] :or {format :transit bind "127.0.0.1"}}]
  (hk/run-server (ws-handler world format)
                 {:ip bind :port port :max-ws max-message :legacy-return-value? false :server-header "knowledge-world"}))

(defn port [server] (hk/server-port server))

(defn stop! [server]
  (when server (hk/server-stop! server {:timeout 100})))
