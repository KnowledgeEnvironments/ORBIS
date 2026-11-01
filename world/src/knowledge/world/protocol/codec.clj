(ns knowledge.world.protocol.codec
  "Both wire forms (protocol.md, ADR 0002). Transit-JSON is native; the JSON form goes through
   knowledge.schema.wire. decode validates against :ke/Frame and throws ex-info on anything else."
  (:require [clojure.walk :as walk]
            [cognitect.transit :as transit]
            [jsonista.core :as j]
            [knowledge.schema.registry :as r]
            [knowledge.schema.wire :as wire]
            [malli.core :as m])
  (:import [java.io ByteArrayInputStream ByteArrayOutputStream]
           [java.time Instant]
           [java.util Base64 Date]))

(defn transit-read [^String s]
  (transit/read (transit/reader (ByteArrayInputStream. (.getBytes s "UTF-8")) :json)))

(defn transit-write ^String [v]
  (let [out (ByteArrayOutputStream. 1024)]
    (transit/write (transit/writer out :json) v)
    (.toString out "UTF-8")))

(defn- humanize [k v] ((requiring-resolve 'malli.error/humanize) (r/explain k v)))

(defn decode
  "String -> validated frame map for format (:transit | :json). Throws ex-info {:explain ..}."
  [format ^String s]
  (case format
    :json (wire/decode :ke/Frame s)
    :transit (let [v (transit-read s)]
               (if (and (map? v) (r/valid? :ke/Frame v))
                 v
                 (throw (ex-info "invalid frame" {:explain (when (map? v) (humanize :ke/Frame v))}))))))

(defn plain
  "A value in the JSON form for schema positions typed :map or :any (read and act results):
   keywords and uuids to strings, instants to ISO-8601, bytes to base64, sets to vectors."
  [v]
  (walk/postwalk
    (fn [x]
      (cond (keyword? x) (subs (str x) 1)
            (uuid? x) (str x)
            (instance? Date x) (str (Instant/ofEpochMilli (.getTime ^Date x)))
            (bytes? x) (.encodeToString (Base64/getEncoder) ^bytes x)
            (set? x) (vec x)
            :else x))
    v))

(defn- json-ready
  "Responses whose result schema is untyped carry their result through `plain` first."
  [frame]
  (if (and (contains? frame :ok) (map? (:result frame)))
    (update frame :result plain)
    frame))

(defn encode
  "Frame map -> string for format."
  ^String [format frame]
  (case format
    :json (wire/encode :ke/Frame (json-ready frame))
    :transit (transit-write frame)))

(defn decode-result
  "Client side of the JSON form: a response result is :any in the Response schema, so a client
   that knows which op it sent decodes the result by that op's result schema (:ke.result/<op>)."
  [format op result]
  (if (and (= format :json) op (contains? r/named (keyword "ke.result" (name op))))
    (m/decode (r/schema (keyword "ke.result" (name op))) result wire/transformer)
    result))

(defn raw-id
  "Best-effort request id out of a string that did not decode as a frame, for `err invalid`."
  [format ^String s]
  (try (let [m (case format :json (j/read-value s j/keyword-keys-object-mapper) :transit (transit-read s))
             id (when (map? m) (or (:id m) (get m "id")))]
         (when (string? id) id))
       (catch Exception _ nil)))
