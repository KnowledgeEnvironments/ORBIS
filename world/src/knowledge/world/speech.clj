(ns knowledge.world.speech
  "Speech through a Model: model/stt and model/tts name what an OpenAI-compatible endpoint serves for
   each, /audio/transcriptions and /audio/speech. Audio passes through: a person's session hands it to
   the Model and takes the text back, or the reverse. Nothing here appends an event, so no sound and no
   transcript reaches the log; a transcript becomes part of the record only if a person sends it."
  (:require [clojure.string :as str]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.resolve.core :as resolve])
  (:import [java.io ByteArrayOutputStream]
           [java.net URI]
           [java.net.http HttpClient HttpRequest HttpRequest$BodyPublishers HttpResponse$BodyHandlers]
           [java.nio.charset StandardCharsets]
           [java.time Duration]
           [java.util Base64]))

(def timeout-s 120)
(def max-audio-chars (* 3 1024 1024)) ; base64: under the socket's 4 MiB frame, about 2 minutes of 16 kHz mono
(def max-say-chars 4000)
(def max-said-bytes (* 16 1024 1024))
(def default-voice "alloy")

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))
(defn- fail! [code message] (throw (ex-info message {:err {:code code :message message}})))

(defn- model-for!
  "The Model that serves this capability: the one named, else the first of p's own, else the first p may
   use. A Model p cannot evaluate on is one it cannot speak through."
  [world p capability model]
  (let [db (act/db world)
        usable? #(and % (= :model (:object/class (objects/state db %)))
                      (field db % capability)
                      (resolve/resolves? db p (act/id-name %) :evaluate))
        named (when model (or (->uuid model) (resolve/lookup db p (str model))))
        found (if model
                (when (usable? named) named)
                (first (filter usable? (concat (field db p :principal/models) (objects/objects-of-class db :model)))))]
    (or found (fail! :no-speech-model (str "no Model you may use has " (subs (str capability) 1))))))

(defn- endpoint [db m path] (str (str/replace (str (field db m :model/base-url)) #"/+$" "") path))

(defn- authorized [b db m]
  (if-let [k (some-> (field db m :model/key-env) System/getenv)]
    (.header b "Authorization" (str "Bearer " k))
    b))

(defn- send! [^HttpRequest req handler]
  (try (.send (HttpClient/newHttpClient) req handler)
       (catch InterruptedException e (throw e))
       (catch Exception _ (fail! :unreachable "the Model did not answer"))))

(defn- multipart
  "A multipart body by hand: java.net.http has none."
  ^bytes [boundary fields filename mime ^bytes data]
  (let [out (ByteArrayOutputStream.)
        w (fn [^String s] (.write out (.getBytes s StandardCharsets/UTF_8)))]
    (doseq [[k v] fields]
      (w (str "--" boundary "\r\nContent-Disposition: form-data; name=\"" k "\"\r\n\r\n" v "\r\n")))
    (w (str "--" boundary "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" filename
            "\"\r\nContent-Type: " mime "\r\n\r\n"))
    (.write out data)
    (w (str "\r\n--" boundary "--\r\n"))
    (.toByteArray out)))

(defn transcribe!
  "speech.transcribe {audio (base64) mime? model?} -> {:text}. Goes to the Model and back, unrecorded."
  [world p {:keys [audio mime model]}]
  (when (> (count audio) max-audio-chars) (fail! :too-large "that recording is too long: two minutes at most"))
  (let [db (act/db world)
        m (model-for! world p :model/stt model)
        data (try (.decode (Base64/getDecoder) (str audio))
                  (catch IllegalArgumentException _ (fail! :invalid-args "audio is base64")))
        mime (or mime "audio/wav")
        boundary (str "orbis" (random-uuid))
        body (multipart boundary {"model" (field db m :model/stt) "response_format" "json"} "speech.wav" mime data)
        req (-> (HttpRequest/newBuilder (URI/create (endpoint db m "/audio/transcriptions")))
                (.timeout (Duration/ofSeconds timeout-s))
                (.header "Content-Type" (str "multipart/form-data; boundary=" boundary))
                (authorized db m)
                (.POST (HttpRequest$BodyPublishers/ofByteArray body))
                .build)
        resp (send! req (HttpResponse$BodyHandlers/ofString))]
    (when-not (<= 200 (.statusCode resp) 299) (fail! :unreachable (str "the Model answered " (.statusCode resp))))
    {:text (str (get (try (j/read-value (.body resp)) (catch Exception _ {})) "text"))}))

(defn say!
  "speech.say {text model?} -> {:audio base64 :mime}. Returned to the session that asked, unrecorded."
  [world p {:keys [text model]}]
  (let [db (act/db world)
        m (model-for! world p :model/tts model)
        body (j/write-value-as-string {:model (field db m :model/tts) :input (subs (str text) 0 (min max-say-chars (count (str text))))
                                       :voice (or (field db m :model/voice) default-voice) :response_format "wav"})
        req (-> (HttpRequest/newBuilder (URI/create (endpoint db m "/audio/speech")))
                (.timeout (Duration/ofSeconds timeout-s))
                (.header "Content-Type" "application/json")
                (authorized db m)
                (.POST (HttpRequest$BodyPublishers/ofString body))
                .build)
        resp (send! req (HttpResponse$BodyHandlers/ofByteArray))
        ^bytes bytes (.body resp)]
    (when-not (<= 200 (.statusCode resp) 299) (fail! :unreachable (str "the Model answered " (.statusCode resp))))
    (when (> (alength bytes) max-said-bytes) (fail! :too-large "that speech is too long to hand back"))
    {:audio (.encodeToString (Base64/getEncoder) bytes) :mime "audio/wav"}))
