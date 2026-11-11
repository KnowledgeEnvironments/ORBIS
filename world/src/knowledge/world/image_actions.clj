(ns knowledge.world.image-actions
  "image.put, image.show, image.read and image.open (knowledge.world.image). A Message is what grants an
   image: to read one you resolve the Message, so a hash alone opens nothing."
  (:require [knowledge.world.act.actions :as actions]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.core :as act]
            [knowledge.world.image :as image]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.pane.actions :as pane-actions]
            [knowledge.world.pane.core :as pane]
            [knowledge.world.pane.render :as render]
            [knowledge.world.resolve.core :as resolve])
  (:import [java.io ByteArrayOutputStream]
           [java.util Base64]))

(def ^:private uploads "Pieces of a client's image, by Principal and upload id, until the last arrives." (atom {}))
(def ^:private upload-ttl-ms (* 5 60 1000))

(defn- fail! [code message] (throw (ex-info message {:err {:code code :message message}})))

(defn- expire! []
  (let [cutoff (- (System/currentTimeMillis) upload-ttl-ms)]
    (swap! uploads (fn [m] (into {} (remove (fn [[_ {:keys [at]}]] (< at cutoff))) m)))))

(defn- piece!
  "One base64 piece. The whole bytes on the last, or nil while more are coming."
  [p {:keys [base64 upload more]}]
  (expire!)
  (let [bs (try (.decode (Base64/getDecoder) ^String base64)
                (catch IllegalArgumentException _ (fail! :invalid-args "base64 does not decode")))]
    (if (and (nil? upload) (not more))
      bs
      (let [k [p (str upload)]
            ^ByteArrayOutputStream out (or (:out (get @uploads k)) (ByteArrayOutputStream.))]
        (.write out ^bytes bs)
        (when (> (.size out) image/max-bytes)
          (swap! uploads dissoc k)
          (fail! :too-large "an image is at most 8 MiB"))
        (if more
          (do (swap! uploads assoc k {:out out :at (System/currentTimeMillis)}) nil)
          (do (swap! uploads dissoc k) (.toByteArray out)))))))

(defmethod actions/handle! :image.put
  [_ world p {:keys [path name mount base64] :as args} _]
  (cond
    base64 (if-let [bs (piece! p args)]
             (let [{:keys [ok err]} (image/store! world bs name)]
               (if err {:err err} {:ok {:result (assoc ok :shown-image ok)}}))
             {:ok {:result {:more true}}})
    (or path name)
    (let [{:keys [bytes] n :name} (pane-actions/file-bytes world p {:path path :name name :mount (when-not (some-> path (.startsWith "/")) mount)} image/max-bytes)
          {:keys [ok err]} (image/store! world bytes n)]
      (if err {:err err} {:ok {:result (assoc ok :shown-image ok)}}))
    :else (fail! :invalid-args "give path, name or base64")))

(defn- attachment!
  "The image of a Message p may read, with the Grant; else unresolved."
  [world p message index]
  (let [db (act/db world)
        id (or (parse-uuid (str message)) (fail! :invalid-args "message is an id"))
        {:keys [grant]} (or (resolve/resolve! world db p (act/id-name id) :read)
                            (throw (ex-info "unresolved" {:err (:err act/unresolved)})))
        images (image/of-message db id)
        a (nth images (long (or index 0)) nil)]
    (when-not a (fail! :invalid-args "that Message carries no such image"))
    {:attachment a :grant grant}))

(defmethod actions/handle! :image.show
  [_ world p {:keys [message index]} _]
  (let [{:keys [attachment]} (attachment! world p message index)]
    {:ok {:result (assoc attachment :shown-image attachment)}}))

(defmethod actions/handle! :image.read
  [_ world p {:keys [message index offset]} _]
  (let [{:keys [attachment]} (attachment! world p message index)
        bs (or (blob/get (:blobs world) (:sha256 attachment)) (fail! :pruned "retention pruned this image's bytes"))]
    {:ok {:result (assoc (image/piece bs offset pane-actions/bytes-chunk) :mime (:mime attachment))}}))

(defmethod actions/handle! :image.open
  [_ world p {:keys [message index]} {:keys [via]}]
  (let [{:keys [attachment grant]} (attachment! world p message index)
        receipt {:blob/sha256 (:sha256 attachment) :blob/size (:size attachment)}
        {:keys [pane seq]} (pane/create! world p :image nil
                                         {:grant grant :via via :title (let [n (:name attachment)] (if (seq n) n "image"))
                                          :fields {:pane/path (:name attachment) :pane/image receipt :pane/mime (:mime attachment)}})]
    {:ok {:result {:pane pane :kind :image} :object pane :seq seq}}))

;; A client asks for what its math layout cannot draw. A person's own session: a model's text reaches
;; this, but only through the client showing it to a person, and it costs the world a process.
(defmethod actions/handle! :tex.render
  [_ world p {:keys [source]} {:keys [session?]}]
  (if-not (authority/person-session? (act/db world) p session?)
    (act/err :person-only "only a person's own session")
    (render/formula! world source)))

(defmethod actions/handle! :tex.log
  [_ world p {:keys [source]} {:keys [session?]}]
  (if-let [log (and (authority/person-session? (act/db world) p session?) (render/log-of world source))]
    {:ok {:result {:log log}}}
    act/unresolved))

;; A rendered source's first page, as a pane of its own, for a person who wants it larger than a chat.
(defmethod actions/handle! :tex.open
  [_ world p {:keys [source title]} {:keys [session? via]}]
  (let [db (act/db world)
        receipt (and (authority/person-session? db p session?) (render/receipt world source))
        grant (when receipt (:grant (resolve/resolve! world db p (act/id-name p) :read)))]
    (if-not receipt
      act/unresolved
      (let [{:keys [pane seq]} (pane/create! world p :image nil
                                             {:grant grant :via via :title (or (not-empty title) "typeset")
                                              :fields {:pane/path "typeset.png" :pane/image receipt :pane/mime "image/png"}})]
        {:ok {:result {:pane pane :kind :image} :object pane :seq seq}}))))

(defmethod actions/handle! :tex.read
  [_ world p {:keys [source offset]} {:keys [session?]}]
  (if-let [bs (and (authority/person-session? (act/db world) p session?) (render/formula-png world source))]
    {:ok {:result (image/piece bs offset pane-actions/bytes-chunk)}}
    act/unresolved))
