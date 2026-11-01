(ns knowledge.world.pane.field-editor-test
  "An editor pane on a Field: the same buffer, holder rule and stream as a file, and a save that is
   a set-field, so every rule of a Field holds."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.authority :as authority]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.pane.editor :as editor]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- result [r] (let [x (:result r)] (if (contains? x :result) (:result x) x)))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- rec [world id k] (get (objects/fields (act/db world) id) k))

(defn- until
  ([f] (until f 5000))
  ([f ms]
   (let [deadline (+ (System/currentTimeMillis) ms)]
     (loop []
       (or (f) (when (< (System/currentTimeMillis) deadline) (Thread/sleep 20) (recur)))))))

(defn- thing!
  "An Object with the Field k, by root."
  [root k value]
  (let [id (get-in (ws/request!! root :act {:class :object :action :create :args {:fields {}}}) [:result :object])]
    (is (:ok (ws/request!! root :set-field {:object id :field k :type :string :value value :reader :agent})))
    id))

(defn- open! [c id k] (act! c "pane.open" {:kind "editor" :object (str id) :field (subs (str k) 1)}))
(defn- buffer [c pane] (result (act! c "pane.read" {:pane (str pane)})))
(defn- frob! [c pane verb & [args]] (act! c "pane.frob" {:pane (str pane) :verb verb :args (or args {})}))

(deftest a-field-opens-edits-and-saves
  (fx/with-world [{:keys [world root]}]
    (let [id (thing! root :notes/long "line one\nline two")
          {:keys [pane kind]} (result (open! root id :notes/long))]
      (is (= :editor (keyword (name kind))))
      (is (str/ends-with? (:field/value (rec world pane :pane/title)) "· notes/long"))
      (is (= "line one\nline two" (:text (buffer root pane))))
      (is (:ok (frob! root pane "insert" {:at 0 :text "first "})))
      (is (= "line one\nline two" (:field/value (rec world id :notes/long))) "not until saved")
      (let [r (frob! root pane "save")]
        (is (:ok r) (pr-str r))
        (let [f (rec world id :notes/long)]
          (is (= "first line one\nline two" (:field/value f)))
          (is (= :string (:field/type f)))
          (is (= :agent (:field/reader f)) "the reader stays the Field's")))
      (is (nil? (:holder (buffer root pane))) "a save gives the buffer up"))))

(deftest what-does-not-open
  (fx/with-world [{:keys [world root]}]
    (let [id (thing! root :notes/a "x")]
      (is (= :invalid-args (code (open! root id :notes/absent))))
      (is (= :invalid-args (code (act! root "pane.open" {:kind "markdown" :object (str id) :field "notes/a"}))))
      (is (= :invalid-args (code (act! root "pane.open" {:kind "editor" :object (str id)}))))
      (testing "a list is not text"
        (ws/request!! root :set-field {:object id :field :notes/list :type [:list :string] :value ["a"] :reader :both})
        (is (= :invalid-args (code (open! root id :notes/list)))))
      (testing "a Field with its own visibility stays off a pane's stream"
        (is (= :invalid-args (code (open! root (:principal root) :principal/token-hash))))))))

(deftest a-write-from-anywhere-reloads-a-buffer-nobody-holds
  (fx/with-world [{:keys [world root]}]
    (let [id (thing! root :notes/a "before")
          pane (:pane (result (open! root id :notes/a)))]
      (act! root "pane.subscribe" {:pane (str pane)})
      (ws/await-frame root #(and (= pane (:pane %)) (:full %)) 5000)
      (ws/request!! root :set-field {:object id :field :notes/a :type :string :value "after" :reader :agent})
      (let [f (ws/await-frame root #(and (= pane (:pane %)) (:full %)) 5000)]
        (is (= "after" (:data f)) "the whole text, to every client"))
      (is (= "after" (:text (buffer root pane))))
      (testing "a closed pane stops following"
        (is (:ok (act! root "pane.close" {:pane (str pane)})))
        (ws/request!! root :set-field {:object id :field :notes/a :type :string :value "later" :reader :agent})
        (Thread/sleep 200)
        (is (= "after" (:text (buffer root pane))))))))

(deftest a-held-buffer-is-stale-and-its-holder-decides
  (fx/with-world [{:keys [world root]}]
    (let [id (thing! root :notes/a "base")
          pane (:pane (result (open! root id :notes/a)))]
      (frob! root pane "insert" {:at 0 :text "mine "})
      (act! root "pane.subscribe" {:pane (str pane)})
      (ws/request!! root :set-field {:object id :field :notes/a :type :string :value "theirs" :reader :agent})
      (is (ws/await-frame root #(and (= pane (:pane %)) (get-in % [:state :stale])) 5000))
      (is (= "mine base" (:text (buffer root pane))) "nothing of the holder's is lost")
      (is (= :conflict (code (frob! root pane "save"))))
      (is (= "theirs" (:field/value (rec world id :notes/a))))
      (testing "revert takes what is there"
        (is (:ok (frob! root pane "revert")))
        (is (= "theirs" (:text (buffer root pane))))
        (is (nil? (:holder (buffer root pane)))))
      (testing "force replaces it"
        (frob! root pane "insert" {:at 0 :text "mine "})
        (ws/request!! root :set-field {:object id :field :notes/a :type :string :value "again" :reader :agent})
        (is (until #(:stale @(editor/buffer world pane))))
        (is (:ok (frob! root pane "save" {:force true})))
        (is (= "mine theirs" (:field/value (rec world id :notes/a))))))))

(deftest a-save-does-not-reload-itself
  (fx/with-world [{:keys [world root]}]
    (let [id (thing! root :notes/a "one")
          pane (:pane (result (open! root id :notes/a)))]
      (frob! root pane "insert" {:at 0 :text "x"})
      (let [v (:version (result (frob! root pane "save")))]
        (Thread/sleep 300)
        (is (= v (:version (buffer root pane))) "no reload followed the save")
        (is (not (:stale @(editor/buffer world pane))))))))

(deftest a-field-that-moved-unwatched-is-picked-up
  (fx/with-world [{:keys [world root]}]
    (let [id (thing! root :notes/a "old")
          pane (:pane (result (open! root id :notes/a)))]
      ;; what a restart leaves: the log, and no buffer in memory
      (swap! (:panes world) dissoc pane)
      (ws/request!! root :set-field {:object id :field :notes/a :type :string :value "new" :reader :agent})
      (is (= "new" (:text (buffer root pane)))))))

(defn- may-use-panes!
  "witness holds evaluate on the pane actions, as every Principal made since panes does."
  [world root grantee]
  (let [db (act/db world)
        ids (keep #(objects/lookup db (objects/world-id db) (str "action/" %)) ["pane.open" "pane.read" "pane.frob"])]
    (fx/grant! root grantee (set ids) #{:read :evaluate})))

(deftest every-rule-of-a-field-holds
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)]
      (may-use-panes! world root (:id witness))
      (testing "read opens, write saves"
        (let [id (thing! root :notes/a "x")
              _ (fx/grant! root (:id witness) #{id} #{:read})
              w (fx/attach-as world (:token witness))
              pane (:pane (result (open! w id :notes/a)))]
          (is pane)
          (frob! w pane "insert" {:at 0 :text "y"})
          (is (= :unresolved (code (frob! w pane "save"))))
          (is (= "x" (:field/value (rec world id :notes/a))))))
      (testing "policy/* is a person's, from their own session"
        (let [id (thing! root :notes/b "x")
              _ (act/set-field! world root-id {:object id :field :policy/note :type :string :value "p" :reader :both})
              _ (fx/grant! root (:id witness) #{id} #{:read :write})
              pane (:pane (result (open! root id :policy/note)))]
          (frob! root pane "insert" {:at 0 :text "z"})
          (is (= :person-only (code (frob! root pane "save"))) "root's own session")
          (let [[r] (lops/execute! world root-id nil [{:op :act :args {:action :pane.frob
                                                                       :args {:pane (str pane) :verb "save"}}}])]
            (is (false? (:ok r)) (pr-str r)))
          (is (= "p" (:field/value (rec world id :policy/note))))
          (let [w (fx/attach-as world (:token witness))
                mine (:pane (result (open! w id :policy/note)))]
            (is (:ok (frob! w mine "insert" {:at 0 :text "q"})))
            (is (:ok (frob! w mine "save")) "a person's session may")
            (is (= "qp" (:field/value (rec world id :policy/note)))))))
      (testing "a kernel Field is the kernel's"
        (let [id (thing! root :notes/c "x")
              _ (act/set-field! world root-id {:object id :field :world/root :type :string :value "r" :reader :both})
              pane (:pane (result (open! root id :world/root)))]
          (frob! root pane "insert" {:at 0 :text "z"})
          (is (= :conflict (code (frob! root pane "save"))))
          (is (= "r" (:field/value (rec world id :world/root))))))
      (testing "the guard is the set-field op's too"
        (is (some? (authority/write-refusal ":principal/driven-by" true)))
        (is (nil? (authority/write-refusal "instructions/own" false)))))))

