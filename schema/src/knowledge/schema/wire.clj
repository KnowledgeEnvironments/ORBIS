(ns knowledge.schema.wire
  "The JSON form of the protocol (ADR 0002). Transit is the default form and needs no
   help; JSON needs keyword keys and discriminators restored before malli decodes by
   schema position."
  (:require [knowledge.schema.registry :as r]
            [knowledge.schema.protocol :as p]
            [malli.core :as m]
            [malli.transform :as mt]
            [jsonista.core :as j])
  (:import [java.util Base64 Date]
           [java.time Instant]))

(def ^:private mapper
  (j/object-mapper {:encode-key-fn (fn [k] (if (keyword? k) (subs (str k) 1) (str k)))
                    :decode-key-fn keyword
                    :date-format "yyyy-MM-dd'T'HH:mm:ss.SSSX"}))

(def discriminators
  "Keys whose string values must become keywords before multi dispatch."
  #{:op :event/kind :kind :code :class :action :state :to :from :reader :op-name})

(defn- restore [x]
  (cond
    (map? x) (reduce-kv (fn [acc k v]
                          (assoc acc k (if (and (discriminators k) (string? v)) (keyword v) (restore v))))
                        {} x)
    (sequential? x) (mapv restore x)
    :else x))

(def transformer
  (mt/transformer
    mt/json-transformer
    {:name :ke-json
     :decoders {'inst? (fn [x] (if (string? x) (Date/from (Instant/parse x)) x))
                'bytes? (fn [x] (if (string? x) (.decode (Base64/getDecoder) ^String x) x))
                :uuid (fn [x] (if (string? x) (java.util.UUID/fromString x) x))
                :keyword (fn [x] (if (string? x) (keyword x) x))}
     :encoders {'bytes? (fn [x] (if (bytes? x) (.encodeToString (Base64/getEncoder) ^bytes x) x))
                :keyword (fn [x] (if (keyword? x) (subs (str x) 1) x))}}))

(defn- me-humanize [ex]
  ((requiring-resolve 'malli.error/humanize) ex))

(defn decode
  "JSON string -> validated frame (or value of schema k). Throws ex-info with the explanation on failure."
  ([s] (decode :ke/Frame s))
  ([k s]
   (let [schema (r/schema k)
         raw (restore (j/read-value s mapper))
         v (m/decode schema raw transformer)]
     (if (m/validate schema v)
       v
       (throw (ex-info "invalid frame" {:schema k :explain (me-humanize (m/explain schema v))}))))))

(defn encode
  "Frame (or value of schema k) -> JSON string."
  ([v] (encode :ke/Frame v))
  ([k v]
   (j/write-value-as-string (m/encode (r/schema k) v transformer) mapper)))

(defn frame-kind [m] (p/frame-kind m))
