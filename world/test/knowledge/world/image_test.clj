(ns knowledge.world.image-test
  "Images on Messages: stored once, typed from their bytes, sent to a Model that takes them and only
   then, with the record a person can read (decisions 2026-10-31)."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.image :as image]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws])
  (:import [java.util Base64]))

(def png (byte-array (map unchecked-byte [0x89 0x50 0x4e 0x47 0x0d 0x0a 0x1a 0x0a 1 2 3 4])))
(defn- b64 [^bytes bs] (.encodeToString (Base64/getEncoder) bs))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- result [r] (let [x (:result r)] (if (contains? x :result) (:result x) x)))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))

(deftest bytes-say-what-they-are
  (is (= "image/png" (image/mime-of png)))
  (is (= "image/jpeg" (image/mime-of (byte-array (map unchecked-byte [0xff 0xd8 0xff 0xe0 0])))))
  (is (nil? (image/mime-of (.getBytes "<svg onload=alert(1)>" "UTF-8")))))

(deftest a-client-stores-an-image-in-pieces
  (fx/with-world [{:keys [root world]}]
    (let [[a b] (split-at 5 png)
          r1 (act! root "image.put" {:base64 (b64 (byte-array a)) :upload "u1" :more true})
          r2 (act! root "image.put" {:base64 (b64 (byte-array b)) :upload "u1" :name "dir/pic.png"})
          att (result r2)]
      (is (:more (result r1)) (pr-str r1))
      (is (= "image/png" (:mime att)) (pr-str r2))
      (is (= "pic.png" (:name att)))
      (is (= 12 (:size att)))
      (is (blob/exists? (:blobs world) (:sha256 att)))))
  (fx/with-world [{:keys [root]}]
    (is (= :not-an-image (code (act! root "image.put" {:base64 (b64 (.getBytes "hello" "UTF-8"))}))))))

(deftest a-sender-cannot-name-an-image-that-is-not-there
  (fx/with-world [{:keys [world]}]
    (is (:err (image/attachments world ["00ff"])))))

(defn- until [f ms]
  (let [end (+ (System/currentTimeMillis) ms)]
    (loop [] (or (f) (when (< (System/currentTimeMillis) end) (Thread/sleep 50) (recur))))))

(defn- named [root n] (get-in (ws/request!! root :resolve {:name n :op :read}) [:result :object]))

(defn- sends-image [{:keys [world root witness]} accepts?]
  (let [root-id (:principal root)
        route (named root "root.route")
        model (named root "root.model")
        w (fx/attach-as world (:token witness))
        _ (fx/grant! root (:id witness) #{route} #{:transmit})
        _ (when accepts?
            (ws/request!! root :set-field {:object model :field :model/images :type :boolean :value true :reader :both}))
        att (result (act! root "image.put" {:base64 (b64 png) :name "pic.png"}))
        r (ws/request!! w :send {:to #{root-id} :route route :text "what is this" :images [(:sha256 att)]})]
    {:att att :message (get-in r [:result :message]) :r r :w w :root-id root-id}))

(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))

(defn- evaluation-of [world message]
  (until #(let [db (act/db world)]
            (some (fn [e] (when (and (= message (field world e :evaluation/message)) (field world e :evaluation/images)) e))
                  (objects/objects-with-field db :evaluation/message)))
         8000))

(defn- reply-to [world message]
  (until #(some (fn [m] (when (= message (field world m :message/in-reply-to)) (field world m :message/text)))
                (objects/objects-with-field (act/db world) :message/in-reply-to))
         8000))

(deftest a-model-that-takes-images-gets-them-and-the-record-keeps-a-reference
  (fx/with-world [{:keys [world] :as ctx}]
    (let [{:keys [message att r]} (sends-image ctx true)
          _ (is message (pr-str r))
          ev (evaluation-of world message)
          rec (field world ev :evaluation/images)]
      (is (= [(:sha256 att)] (mapv :sha256 rec)))
      (is (true? (:sent (first rec))))
      (is (str/includes? (str (reply-to world message)) "I see 1 image")))))

(deftest a-reference-becomes-bytes-only-at-the-call
  (fx/with-world [{:keys [world root]}]
    (let [att (result (act! root "image.put" {:base64 (b64 png) :name "pic.png"}))
          stored (j/write-value-as-string [{:role "user" :content (image/parts "look" [att])}])
          wire (image/expand world stored)]
      (is (str/includes? stored "image_ref"))
      (is (not (str/includes? stored (b64 png))) "the record holds no bytes")
      (is (str/includes? wire (str "data:image/png;base64," (b64 png)))))))

(deftest a-model-that-does-not-gets-a-line-instead
  (fx/with-world [{:keys [world] :as ctx}]
    (let [{:keys [message]} (sends-image ctx false)
          ev (evaluation-of world message)]
      (is (false? (:sent (first (field world ev :evaluation/images)))))
      (is (not (str/includes? (str (reply-to world message)) "I see"))))))

(deftest a-message-is-what-grants-an-image
  (fx/with-world [{:keys [world root] :as ctx}]
    (let [{:keys [message w]} (sends-image ctx false)
          r (act! w "image.read" {:message (str message)})
          other (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name "other"}})
                        [:result :result :token])
          stranger (fx/attach-as world other)]
      (is (= (b64 png) (:base64 (result r))) (pr-str r))
      (is (= :image (keyword (:kind (result (act! w "image.open" {:message (str message)}))))))
      (is (= :unresolved (code (act! stranger "image.read" {:message (str message)})))))))

(deftest an-agent-reads-a-file-as-an-image-and-sends-one-on
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (named root "root.route")
          model (named root "root.model")
          w (fx/attach-as world (:token witness))
          _ (fx/grant! root (:id witness) #{route} #{:transmit})
          _ (ws/request!! root :set-field {:object model :field :model/images :type :boolean :value true :reader :both})
          db (act/db world)
          shell (some #(when (= :host (field world % :adapter/binds)) %) (objects/objects-of-class db :adapter))
          f (io/file (field world shell :adapter/locator) "agent-picture.png")
          sha (blob/sha256-hex png)
          _ (with-open [o (io/output-stream f)] (.write o ^bytes png))
          block (fn [op] (str "```ops\n" (j/write-value-as-string op) "\n```"))
          r (ws/request!! w :send {:to #{root-id} :route route
                                   :text (block {:op "act" :args {:action "image.put" :args {:path (.getCanonicalPath f)}}})})
          message (get-in r [:result :message])
          answer (until #(some (fn [m] (let [t (str (field world m :message/text))] (when (str/includes? t "I see 1 image") t)))
                               (objects/objects-of-class (act/db world) :message))
                        10000)
          _ (.delete f)
          [out] (lops/execute! world root-id nil [{:op :send :args {:to [(:id witness)] :route route :text "here it is" :images [sha]}}])
          sent (until #(some (fn [m] (when (and (= root-id (field world m :message/from)) (seq (field world m :message/images))) m))
                             (objects/objects-of-class (act/db world) :message))
                      10000)]
      (is message (pr-str r))
      (is sent (pr-str out))
      (is (= sha (:sha256 (first (field world sent :message/images)))))
      (is answer "the image it read came back on its next turn"))))
