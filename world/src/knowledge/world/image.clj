(ns knowledge.world.image
  "Images on Messages (decisions 2026-10-31). The bytes live in the blob store by sha256 and a Message
   names them in message/images, so the log stays small and an image is stored once. A Serialization
   keeps an image_ref; the bytes go to the Model only at the call, so the record never holds them.
   model/images says whether a Model takes images. One that does not gets a line of text instead, so
   nothing silently disappears, and evaluation/images records what happened for a person to see."
  (:require [clojure.string :as str]
            [clojure.walk :as walk]
            [jsonista.core :as j]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.blob :as blob])
  (:import [java.util Base64]))

(def max-bytes "The largest image a Message carries." (* 8 1024 1024))
(def tokens-per-image "What the budget charges for one image: the size of a large image on common Models." 1000)

(defn- starts? [^bytes bs & want]
  (and (>= (alength bs) (count want))
       (every? true? (map-indexed (fn [i b] (= (unchecked-byte b) (aget bs i))) want))))

(defn mime-of
  "The image type of the bytes themselves, or nil. A name can lie; the first bytes cannot."
  [^bytes bs]
  (cond (starts? bs 0x89 0x50 0x4e 0x47) "image/png"
        (starts? bs 0xff 0xd8 0xff) "image/jpeg"
        (starts? bs 0x47 0x49 0x46 0x38) "image/gif"
        (and (starts? bs 0x52 0x49 0x46 0x46) (> (alength bs) 11)
             (= "WEBP" (String. bs 8 4 "ISO-8859-1"))) "image/webp"
        :else nil))

(defn- clean-name [n]
  (let [s (-> (str n) (str/replace #"[\r\n\t\[\]]" " ") str/trim)
        base (last (str/split s #"/"))]
    (subs (or base "") 0 (min 80 (count (or base ""))))))

(defn store!
  "Bytes into the blob store -> {:ok attachment} or {:err {:code :message}}."
  [world ^bytes bs n]
  (let [mime (mime-of bs)]
    (cond (> (alength bs) max-bytes)
          {:err {:code :too-large :message (str "an image is at most " (quot max-bytes (* 1024 1024)) " MiB")}}
          (nil? mime) {:err {:code :not-an-image :message "not a PNG, JPEG, GIF or WebP image"}}
          :else (let [{:blob/keys [sha256 size]} (blob/put! (:blobs world) bs)]
                  {:ok {:sha256 sha256 :size size :mime mime :name (clean-name n)}}))))

(defn- sha-of [v]
  (cond (string? v) v
        (map? v) (or (:sha256 v) (get v "sha256"))
        :else nil))

(defn attachments
  "What a sender named (a sha256 or an attachment map) as attachments read from the bytes themselves,
   or {:err}. The sender cannot claim a type or a size."
  [world images]
  (reduce (fn [out v]
            (let [hex (sha-of v)
                  bs (when (string? hex) (blob/get (:blobs world) hex))
                  mime (when bs (mime-of bs))
                  n (when (map? v) (or (:name v) (get v "name")))]
              (if mime
                (conj out {:sha256 hex :size (alength ^bytes bs) :mime mime :name (clean-name n)})
                (reduced {:err {:code :invalid-args :message (str "no stored image " hex ": image.put it first")}}))))
          [] images))

(defn accepts?
  "Does the Model take images? A person says so in model/images."
  [db model]
  (true? (get-in (objects/fields db model) [:model/images :field/value])))

(defn of-message [db message]
  (when message (vec (get-in (objects/fields db message) [:message/images :field/value]))))

(defn shown-in
  "Images this wake's Evaluations asked to see: results of image.put and image.show carry :shown-image."
  [db evaluations]
  (->> evaluations
       (mapcat (fn [e] (let [v (get-in (objects/fields db e) [:evaluation/results :field/value])]
                         (when (coll? v) (tree-seq coll? seq v)))))
       (keep #(when (map? %) (:shown-image %)))
       (filter map?)
       distinct
       vec))

(defn note
  "The line a Model that cannot see images gets instead of one."
  [{:keys [name mime size]}]
  (str "[image" (when (seq name) (str " " name)) ", " mime ", " (quot (long (or size 0)) 1024)
       " KiB: this model cannot see images, so say so rather than guess at it]"))

(defn parts
  "Content parts for a user turn: its text, then each image by reference."
  [text images]
  (into (if (str/blank? (str text)) [] [{:type "text" :text (str text)}])
        (map (fn [{:keys [sha256 mime name]}] {:type "image_ref" :sha256 sha256 :mime mime :name name}))
        images))

(defn expand
  "The chat messages JSON with each image_ref as an image_url data URL, for the wire only."
  [world ^String text]
  (if-not (and (string? text) (str/includes? text "image_ref"))
    text
    (let [encode (fn [{:keys [sha256 mime]}]
                   (when-let [bs (blob/get (:blobs world) sha256)]
                     {:type "image_url"
                      :image_url {:url (str "data:" mime ";base64," (.encodeToString (Base64/getEncoder) ^bytes bs))}}))
          swap (fn [x] (if (and (map? x) (= "image_ref" (:type x)))
                         (or (encode x) {:type "text" :text "[an image that is no longer stored]"})
                         x))]
      (j/write-value-as-string (walk/postwalk swap (j/read-value text j/keyword-keys-object-mapper))))))

(defn piece
  "A base64 chunk of bytes from offset, the shape pane.read answers."
  [^bytes bs offset limit]
  (let [size (alength bs)
        from (min size (max 0 (long (or offset 0))))
        n (min (- size from) (max 1 (long limit)))]
    {:size size :offset from
     :base64 (.encodeToString (Base64/getEncoder) (java.util.Arrays/copyOfRange bs (int from) (int (+ from n))))
     :more (< (+ from n) size)}))
