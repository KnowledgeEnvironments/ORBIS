(ns knowledge.world.gateway.bluebubbles
  "BlueBubbles over its REST API: ping, message query, text send. The password is a query parameter
   there, so nothing here may log a URL."
  (:require [jsonista.core :as j]
            [knowledge.world.gateway.transport :as transport])
  (:import [java.net URI URLEncoder]
           [java.net.http HttpClient HttpRequest HttpRequest$BodyPublishers HttpResponse$BodyHandlers]
           [java.nio.charset StandardCharsets]
           [java.time Duration]))

(def timeout-s 10)
(def page 100)

(defn- url [{:keys [url password]} path]
  (str (.replaceAll (str url) "/+$" "") "/api/v1/" path "?password="
       (URLEncoder/encode (str password) StandardCharsets/UTF_8)))

(defn- status-of [code]
  (cond (<= 200 code 299) :ok
        (#{401 403} code) :unauthenticated
        :else :unreachable))

(defn- call
  "{:status word :body parsed-or-nil}. Any failure is a word: an exception can quote the URL."
  [conn path body]
  (try
    (let [b (-> (HttpRequest/newBuilder (URI/create (url conn path)))
                (.timeout (Duration/ofSeconds timeout-s)))
          req (.build (if body
                        (-> b (.header "Content-Type" "application/json")
                            (.POST (HttpRequest$BodyPublishers/ofString (j/write-value-as-string body))))
                        (.GET b)))
          resp (.send (HttpClient/newHttpClient) req (HttpResponse$BodyHandlers/ofString))
          status (status-of (.statusCode resp))]
      {:status status
       :body (when (= :ok status)
               (try (j/read-value (.body resp) j/keyword-keys-object-mapper) (catch Exception _ nil)))})
    (catch InterruptedException e (throw e))
    (catch Exception _ {:status :unreachable})))

(defmethod transport/ping :bluebubbles [conn] (:status (call conn "ping" nil)))

(defn- message
  "A row of the query as the interface's message. A row with no sender or no text is nobody's."
  [{:keys [guid text isFromMe dateCreated handle chats]}]
  (when (and guid (string? text) (number? dateCreated))
    {:id (str guid) :text text :at (long dateCreated) :from-me? (boolean isFromMe)
     :handle (some-> (:address handle) str) :chat (some-> (first chats) :guid str)}))

(defmethod transport/fetch :bluebubbles [conn {:keys [after]}]
  (let [{:keys [status body]} (call conn "message/query"
                                    {:limit page :offset 0 :sort "ASC" :after (long (or after 0))
                                     :with ["chat" "handle"]})]
    (cond-> {:status status}
      (= :ok status) (assoc :messages (vec (keep message (:data body)))))))

(defmethod transport/push :bluebubbles [conn {:keys [handle chat text]}]
  (select-keys (call conn "message/text"
                     {:chatGuid (or chat (str "iMessage;-;" handle)) :tempGuid (str (random-uuid))
                      :message text :method "apple-script"})
               [:status]))
