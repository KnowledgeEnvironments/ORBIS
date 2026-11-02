(ns knowledge.world.retention-test
  "Disk retention (decisions 2026-11-01 'retention', docs/log-format.md Retention): one limit over blobs
   and log segments, LRU, and everything that used to assume nothing was ever gone: boot, readers of
   a blob, history, subscriptions, reattach, forks, a self-hosted world's rollback."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [datascript.core :as d]
            [knowledge.world.act.core :as act]
            [knowledge.world.authority :as authority]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.core :as fold]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.image :as image]
            [knowledge.world.index.core :as index]
            [knowledge.world.index.snapshot :as snap]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.log.reader :as reader]
            [knowledge.world.log.segment :as seg]
            [knowledge.world.pane.core :as pane]
            [knowledge.world.pane.editor :as editor]
            [knowledge.world.pane.holder :as holder]
            [knowledge.world.pane.render :as render]
            [knowledge.world.pane.screen :as screen]
            [knowledge.world.retention :as retention]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.system :as system]
            [knowledge.world.tools :as tools]
            [knowledge.world.ws-client :as ws])
  (:import [java.io File]
           [java.nio.file Files]
           [java.nio.file.attribute FileAttribute]
           [java.util Date]))

;; ---------------------------------------------------------------- helpers

(def small-roll 4096)

(defmacro with-small-segments [& body] `(with-redefs [seg/roll-bytes small-roll] ~@body))

(def knobs
  "No grace, the exact limit: a test decides what is old."
  {:grace-ms 0 :low-water 1.0 :every-ms (* 24 3600 1000)})

(defn- boot
  ([dir] (boot dir {}))
  ([dir config] (system/start (merge (fx/config dir 0) {:retention knobs} config))))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- resolved [root n] (get-in (ws/request!! root :resolve {:name n :op :read}) [:result :object]))
(defn- world-field [world k] (get-in (objects/fields (act/db world) (:world-id world)) [k :field/value]))
(defn- gb-of [bytes] (/ (double bytes) 1024.0 1024.0 1024.0))

(defn- used [world]
  (+ (log/own-bytes (:log world)) (reduce + 0 (map :size (blob/inventory (:blobs world))))))

(defn- limited
  "The world under a limit `bytes` above (or below) what it uses now."
  [world delta]
  (assoc-in world [:config :retention-gb] (gb-of (+ (used world) delta))))

(defn- fill!
  "n events of about size bytes each on the world's own stream, enough to roll segments."
  [world n size]
  (let [wid (:world-id world)]
    (dotimes [i n]
      (log/append!! (:log world)
                    (log/pending :field-set wid wid {:name (keyword "test" (str "f" (mod i 7))) :type :string
                                                     :value (apply str (repeat size "x")) :reader :both})))))

(defn- snapshot!
  "A snapshot at the head, as the index writes every 10k events."
  [world]
  (let [idx (:index world) db (index/db idx)]
    (snap/write! (:dir idx) (index/applied-seq idx) db)))

(defn- blob-file ^File [world hex]
  (io/file (:data-dir world) "blobs" "sha256" (subs hex 0 2) hex))

(defn- age!
  "Make a blob's last use look old."
  [world hex minutes]
  (.setLastModified (blob-file world hex) (- (System/currentTimeMillis) (* minutes 60000))))

(defn- put-blob! [world tag size]
  (:blob/sha256 (blob/put! (:blobs world) (.getBytes (str tag (apply str (repeat size "b"))) "UTF-8"))))

(defn- prune-all!
  "Roll past the events so far, snapshot, and sweep the log down to its active segment."
  [world]
  (fill! world 4 3000)
  (snapshot! world)
  (fill! world 1 3000)
  (retention/sweep! (assoc-in world [:config :retention-gb] 1.0e-9)))

(defn- segment-names [dir] (mapv #(.getName ^File %) (seg/segment-files (io/file dir "log"))))

(defn- wipe! [dir]
  (doseq [^File f (reverse (file-seq (io/file dir)))]
    (.setWritable f true true)
    (.delete f)))

(defmacro with-dir [[sym] & body]
  `(let [~sym (fx/temp-dir)]
     (try ~@body (finally (wipe! ~sym)))))

(defn- person!
  "test/witness, a person, who may call `actions`, reads the world. Returns {:client :id}."
  [world root actions]
  (let [witness (fx/create-witness! root)]
    (doseq [a actions]
      (fx/grant! root (:id witness) #{(resolved root (str "action/" a))} #{:read :evaluate}))
    (fx/grant! root (:id witness) #{(:id witness)} #{:read})
    {:id (:id witness) :client (fx/attach-as world (:token witness))}))

;; ---------------------------------------------------------------- blobs

(deftest a-read-marks-use-but-not-on-every-read
  (with-dir [dir]
    (let [store (blob/open dir)
          hex (:blob/sha256 (blob/put! store (.getBytes "bytes" "UTF-8")))
          f (io/file dir "blobs" "sha256" (subs hex 0 2) hex)
          recent (- (System/currentTimeMillis) 60000)
          old (- (System/currentTimeMillis) (* 2 blob/touch-every-ms))]
      (.setLastModified f recent)
      (is (some? (blob/get store hex)))
      (is (= (quot recent 1000) (quot (.lastModified f) 1000)) "a recent mark is left: a read is not a write")
      (.setLastModified f old)
      (blob/get store hex)
      (is (> (.lastModified f) recent) "an old mark moves")
      (testing "putting the same bytes again counts as use, so an eviction cannot beat a new receipt"
        (.setLastModified f old)
        (blob/put! store (.getBytes "bytes" "UTF-8"))
        (is (> (.lastModified f) recent))))))

(deftest an-eviction-leaves-a-blob-used-since-it-looked
  (with-dir [dir]
    (let [store (blob/open dir)
          hex (:blob/sha256 (blob/put! store (.getBytes "bytes" "UTF-8")))
          item (first (blob/inventory store))]
      (Thread/sleep 5)
      (blob/put! store (.getBytes "bytes" "UTF-8"))
      (is (nil? (blob/evict! store item)) "the mtime moved")
      (is (blob/exists? store hex))
      (is (= (:size item) (blob/evict! store (first (blob/inventory store)))))
      (is (nil? (blob/get store hex)) "gone reads as nil, as every reader expects")
      (is (= "bytes" (String. (blob/get store (:blob/sha256 (blob/put! store (.getBytes "bytes" "UTF-8")))) "UTF-8"))
          "and the same bytes can come back"))))

(deftest a-hash-that-is-no-hash-reads-as-nothing
  (with-dir [dir]
    (let [store (blob/open dir)]
      (is (nil? (blob/get store nil)) "an editor whose open event was pruned has no hash to ask for")
      (is (nil? (blob/get store "ab")))
      (is (nil? (blob/get store "../../../etc/passwd"))))))

(deftest a-fork-never-touches-its-parents-blobs
  (with-dir [pdir]
    (with-dir [cdir]
      (let [pstore (blob/open pdir)
            hex (:blob/sha256 (blob/put! pstore (.getBytes "parent" "UTF-8")))
            f (io/file pdir "blobs" "sha256" (subs hex 0 2) hex)
            old (- (System/currentTimeMillis) (* 3 blob/touch-every-ms))]
        (.setLastModified f old)
        (is (some? (blob/get (blob/open cdir [pdir]) hex)))
        (is (= (quot old 1000) (quot (.lastModified f) 1000)))
        (is (empty? (blob/inventory (blob/open cdir [pdir]))) "and does not count them as its own")))))

(deftest blobs-go-least-recently-used-first
  (with-dir [dir]
    (let [world (boot dir)]
      (try
        (let [a (put-blob! world "a" 9000) b (put-blob! world "b" 9000) c (put-blob! world "c" 9000)]
          (age! world a 300) (age! world b 200) (age! world c 100)
          (let [r (retention/sweep! (limited world -5000))]
            (is (= 1 (:blobs r)))
            (is (pos? (:bytes r))))
          (is (nil? (blob/get (:blobs world) a)) "the oldest went")
          (is (some? (blob/get (:blobs world) b)))
          (is (some? (blob/get (:blobs world) c)))
          (testing "a read saves a blob from the next pass"
            (age! world b (* 3 60))
            (age! world c 100)
            (blob/get (:blobs world) b)
            (retention/sweep! (limited world -5000))
            (is (nil? (blob/get (:blobs world) c)))
            (is (some? (blob/get (:blobs world) b))))
          (is (= 1 (log/first-seq (:log world))) "evicting blobs was enough: no event went"))
        (finally (system/stop world))))))

(deftest a-new-blob-has-a-grace-period
  (with-dir [dir]
    (let [world (boot dir {:retention (assoc knobs :grace-ms 600000)})]
      (try
        (let [fresh (put-blob! world "fresh" 9000)]
          (retention/sweep! (limited world -5000))
          (is (some? (blob/get (:blobs world) fresh)) "a blob on its way to the log, with no Field naming it yet"))
        (finally (system/stop world))))))

;; ---------------------------------------------------------------- what a live thing needs

(defn- field! [world object k value]
  (log/append!! (:log world) (log/pending :field-set object (:world-id world)
                                          {:name k :type :bytes :value value :reader :both})))

(deftest a-prompt-goes-and-its-record-stays
  (with-dir [dir]
    (let [world (boot dir)
          root (fx/attach-root world)]
      (try
        (let [root-id (:principal root)
              model (first (objects/objects-of-class (act/db world) :model))
              message-text (apply str (repeat 9000 "p"))
              {:keys [ok]} (ser/serialize! world {:principal root-id :working-set #{root-id} :model model
                                                  :rendered {:body message-text}})
              id (:serialization ok)
              state (objects/state (act/db world) id)
              receipt (get-in state [:object/fields :serialization/bytes :field/value])
              hash (get-in state [:object/fields :serialization/hash :field/value])]
          (is (blob/receipt? receipt))
          (is (= (:text ok) (ser/bytes-of world state)))
          (age! world (:blob/sha256 receipt) 300)
          (retention/sweep! (limited world -1000))
          (is (nil? (blob/get (:blobs world) (:blob/sha256 receipt))))
          (testing "every Field is still there"
            (let [after (objects/state (act/db world) id)]
              (is (= hash (get-in after [:object/fields :serialization/hash :field/value])))
              (is (= (:size ok) (get-in after [:object/fields :serialization/size :field/value])))
              (is (= receipt (get-in after [:object/fields :serialization/bytes :field/value])))
              (is (nil? (ser/bytes-of world after)) "nil, not a throw")
              (is (= {} (ser/ids-of world after)) "an empty table, not a throw")))
          (testing "a read says so"
            (let [r (ws/request!! root :read {:object id :field :serialization/bytes})]
              (is (:ok r) (pr-str r))
              (is (true? (get-in r [:result :object/blobs :serialization/bytes :pruned]))))
            (is (= :pruned (code (ws/request!! root :read {:object id :field :serialization/bytes :byte-range [0 100]}))))
            (is (= :pruned (code (ws/request!! root :read {:object id :field :serialization/bytes :range [1 5]}))))))
        (finally (ws/close! root) (system/stop world))))))

(deftest a-current-field-is-protected-a-superseded-one-is-not
  (with-dir [dir]
    (let [world (boot dir)]
      (try
        (let [id (random-uuid)
              wid (:world-id world)
              _ (log/append!! (:log world) (log/pending :created id wid {:class :object :fields {}}))
              old (byte-array 9000 (byte 1))
              new (byte-array 9000 (byte 2))
              _ (field! world id :test/data old)
              _ (field! world id :test/data new)
              old-hex (blob/sha256-hex old) new-hex (blob/sha256-hex new)]
          (age! world old-hex 300) (age! world new-hex 300)
          (retention/sweep! (limited world -8000))
          (is (nil? (blob/get (:blobs world) old-hex)) "what the Field used to hold is history")
          (is (some? (blob/get (:blobs world) new-hex)) "what it holds now is the world")
          (testing "and on a retired Object it goes too"
            (log/append!! (:log world) (log/pending :retired id wid {:names []}))
            (age! world new-hex 300)
            (retention/sweep! (limited world -8000))
            (is (nil? (blob/get (:blobs world) new-hex)))))
        (finally (system/stop world))))))

(defn- message!
  "A Message from the world with one image on it, its thread its own unless named."
  [world sha ^Date at]
  (let [wid (:world-id world) id (random-uuid)]
    (log/append!! (:log world) (log/pending :created id wid {:class :message :fields {}}))
    (log/append!! (:log world) (log/pending :field-set id wid {:name :message/text :type :string :value "see" :reader :both} :at at))
    (log/append!! (:log world) (log/pending :field-set id wid {:name :message/images :type [:list :map] :reader :both
                                                               :value [{:sha256 sha :size 9000 :mime "image/png" :name "x.png"}]}))
    id))

(deftest an-image-stays-while-its-conversation-is-current
  (with-dir [dir]
    (let [world (boot dir)]
      (try
        (let [now (Date.)
              recent (put-blob! world "recent" 9000)
              stale (put-blob! world "stale" 9000)
              _ (message! world recent now)
              _ (message! world stale (Date. (- (.getTime now) (* 40 24 3600 1000))))]
          (age! world recent 300) (age! world stale 300)
          (retention/sweep! (limited world -5000))
          (is (some? (blob/get (:blobs world) recent)))
          (is (nil? (blob/get (:blobs world) stale)))
          (testing "an old image in a prompt becomes a line for the Model, not a failure"
            (let [text (str "{\"content\":[{\"type\":\"image_ref\",\"sha256\":\"" stale "\",\"mime\":\"image/png\"}]}")]
              (is (str/includes? (image/expand world text) "no longer stored"))))
          (testing "sending it again is refused cleanly"
            (is (= :invalid-args (get-in (image/attachments world [stale]) [:err :code])))))
        (finally (system/stop world))))))

(deftest a-rendered-formula-compiles-again-when-its-blob-is-gone
  (with-dir [dir]
    (let [world (boot dir)]
      (try
        (let [source "x^2"
              hex (put-blob! world "png" 9000)
              memo (#'render/memo-of world source)]
          (io/make-parents memo)
          (spit memo (pr-str {:sha256 hex :width 1 :height 1 :pages 1 :kind :math}))
          (is (:ok (render/formula! world source)) "the memo answers while the bytes are there")
          (age! world hex 300)
          (retention/sweep! (limited world -5000))
          (is (nil? (render/formula-png world source)))
          (is (nil? (render/receipt world source)))
          (with-redefs [render/renderer (constantly nil)]
            (is (= :no-renderer (get-in (render/formula! world source) [:err :code]))
                "it goes on to compile, here on a host with no TeX, and does not answer a sha that leads nowhere"))
          (is (not (.exists memo))))
        (finally (system/stop world))))))

(deftest an-open-terminal-keeps-its-newest-output
  (with-dir [dir]
    (let [world (boot dir {:retention (assoc knobs :terminal-bytes 20000)})]
      (try
        (let [wid (:world-id world)
              {:keys [pane]} (pane/create! world wid :terminal nil {:title "t" :size {:cols 80 :rows 24}})
              out (fn [i tag]
                    (let [{:blob/keys [sha256 size]} (blob/put! (:blobs world) (.getBytes (str tag (apply str (repeat 9000 "o"))) "UTF-8"))]
                      (log/append!! (:log world) (log/pending :pane-output pane wid {:pane pane :n i :blob sha256 :size size :chain (str "c" i)}))
                      sha256))
              hashes (mapv #(out % (str "line" %)) (range 4))]
          (doseq [h hashes] (age! world h 300))
          (retention/sweep! (limited world -100000))
          (is (nil? (blob/get (:blobs world) (first hashes))) "the oldest of a long terminal may go")
          (is (some? (blob/get (:blobs world) (peek hashes))))
          (is (some? (blob/get (:blobs world) (hashes 2))))
          (testing "a replay across the hole does not fail, and says so"
            (let [s (#'pane/replay-screen world pane)]
              (is (some? s))
              (is (some #(str/includes? % "older output pruned")
                        (concat (screen/scrollback-lines s) (screen/screen-lines s)))))))
        (finally (system/stop world))))))

(deftest a-long-quiet-terminal-keeps-its-chain-through-a-prune
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)]
        (try
          (let [wid (:world-id world)
                {:keys [pane]} (pane/create! world wid :terminal nil {:title "t" :size {:cols 80 :rows 24}})
                sha (:blob/sha256 (blob/put! (:blobs world) (.getBytes "hi" "UTF-8")))]
            (log/append!! (:log world) (log/pending :pane-output pane wid {:pane pane :n 7 :blob sha :size 2 :chain "abc" :offset 42}))
            (prune-all! world)
            (is (empty? (filter #(= :pane-output (:event/kind %)) (log/read-stream (:log world) pane 0 100)))
                "its output event is gone from the log")
            (let [asked (atom nil)]
              (with-redefs [holder/attach! (fn [_ _ start] (reset! asked start))]
                (#'pane/revive! world :conn pane {:cols 80 :rows 24}))
              (is (= 42 @asked) "the holder is asked for what came after, not for everything again")
              (let [l (get @(:panes world) pane)]
                (is (= "abc" @(:chain l)))
                (is (= 7 @(:n l))))))
          (finally (system/stop world)))))))

(deftest an-open-editor-keeps-its-buffer-through-a-prune-and-a-restart
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)
            root (fx/attach-root world)
            id (get-in (ws/request!! root :act {:class :object :action :create :args {:fields {}}}) [:result :object])
            _ (ws/request!! root :set-field {:object id :field :notes/long :type :string :value "the file" :reader :agent})
            open (act! root "pane.open" {:kind "editor" :object (str id) :field "notes/long"})
            pane (or (get-in open [:result :result :pane]) (get-in open [:result :pane]))
            _ (is (:ok (act! root "pane.frob" {:pane (str pane) :verb "insert" :args {:at 0 :text "unsaved "}})))
            before @(editor/buffer world pane)]
        (ws/close! root)
        (prune-all! world)
        (is (> (log/first-seq (:log world)) 1))
        (is (empty? (filter #(= :pane-opened (:event/kind %)) (log/read-stream (:log world) pane 0 100)))
            "the event it opened on has gone")
        (system/stop world)
        (let [w2 (boot dir)]
          (try
            (let [after @(editor/buffer w2 pane)]
              (is (= "unsaved the file" (:text after)) "an unsaved edit survives")
              (is (= (:version before) (:version after)))
              (is (= (:undo before) (:undo after)) "undo too")
              (is (= (:base-sha before) (:base-sha after))))
            (finally (system/stop w2))))))))

(deftest an-editor-with-nothing-left-opens-on-what-is-there
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)]
        (try
          (let [wid (:world-id world)
                id (random-uuid)
                _ (log/append!! (:log world) (log/pending :created id wid {:class :object :fields {}}))
                _ (log/append!! (:log world) (log/pending :field-set id wid {:name :notes/a :type :string :value "kept" :reader :both}))
                {:keys [pane]} (pane/create! world wid :editor id {:title "e" :blob (apply str (repeat 64 "0")) :fields {:pane/field "notes/a"}})]
            (let [{:keys [text lost-edits]} (#'editor/fold-buffer world pane)]
              (is (= "kept" text) "a blob that is gone does not fail the buffer")
              (is (true? lost-edits))))
          (finally (system/stop world)))))))

;; ---------------------------------------------------------------- log segments

(deftest log-segments-go-only-when-blobs-were-not-enough-and-never-the-active-one
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)]
        (try
          (let [blob-hex (put-blob! world "evictable" 9000)]
            (age! world blob-hex 300)
            (fill! world 12 3000)
            (snapshot! world)
            (fill! world 3 3000)
            (let [head (log/head (:log world))
                  r (retention/sweep! (limited world -2000))]
              (is (nil? (blob/get (:blobs world) blob-hex)))
              (is (zero? (:segments r)) "2000 bytes came from the blob")
              (is (= 1 (log/first-seq (:log world))))
              (let [r (retention/sweep! (limited world (- (+ 9000 (reduce + 0 (map :size (blob/inventory (:blobs world))))))))
                    floor (log/first-seq (:log world))
                    anchor (snap/anchor-seq dir)]
                (is (pos? (:segments r)))
                (is (> floor 1))
                (is (<= (dec floor) anchor) "wholly at or below the verified snapshot")
                (is (<= head (log/head (:log world))) "the head never moves back")
                (is (= (seg/file-name floor) (first (segment-names dir))))))
            (testing "down to nothing, the active segment and everything above the snapshot stay"
              (retention/sweep! (assoc-in world [:config :retention-gb] 1.0e-9))
              (let [anchor (snap/anchor-seq dir)]
                (is (<= (dec (log/first-seq (:log world))) anchor))
                (is (seq (segment-names dir)))
                (is (every? #(<= (:event/seq %) (log/head (:log world))) (log/read-from (:log world) (log/first-seq (:log world)) 10))))))
          (finally (system/stop world)))))))

(deftest no-verified-snapshot-no-segment-goes
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)]
        (try
          (fill! world 12 3000)
          (let [f (snapshot! world)]
            (fill! world 3 3000)
            (spit f "not a snapshot")
            (let [r (retention/sweep! (assoc-in world [:config :retention-gb] 1.0e-9))]
              (is (= 1 (log/first-seq (:log world))) "a snapshot that does not load holds nothing up")
              (is (zero? (:segments r)))
              (is (str/includes? (str (:blocked r)) "no verified snapshot"))
              (is (nil? (snap/anchor-seq dir)))
              (is (= (:blocked r) (:blocked (world-field world retention/sweep-field)))
                  "a person can see why the disk is over")))
          (finally (system/stop world)))))))

(deftest a-sweep-does-not-hold-up-the-writer
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)]
        (try
          (fill! world 12 3000)
          (snapshot! world)
          (fill! world 3 3000)
          (let [verified snap/verified
                entered (promise) release (promise)]
            (with-redefs [snap/verified (fn [f id] (deliver entered true) @release (verified f id))]
              (let [sweep (future (retention/sweep! (assoc-in world [:config :retention-gb] 1.0e-9)))]
                (is (true? (deref entered 5000 false)) "the sweep is stopped in the middle of its work")
                (let [t (System/nanoTime)]
                  (fill! world 20 3000)
                  (is (< (/ (- (System/nanoTime) t) 1e6) 3000) "appends run past it"))
                (deliver release true)
                @sweep)))
          (is (> (log/first-seq (:log world)) 1))
          (finally (system/stop world)))))))

(deftest a-prune-and-the-writer-do-not-undo-each-other
  (with-small-segments
    (with-dir [dir]
      (let [l (log/open! dir {:fsync :never})
            s (random-uuid) a (random-uuid)
            append #(log/append!! l (log/pending :field-set s a {:name :t/x :type :string :value (apply str (repeat 3000 "x")) :reader :both}))]
        (try
          (dotimes [_ 20] (append))
          (let [writer (future (dotimes [_ 200] (append)))
                pruner (future (loop [n 0]
                                 (if (or (realized? writer) (> n 100000))
                                   n
                                   (do (log/prune-oldest! l (- (log/head l) 5)) (recur (inc n))))))]
            @writer @pruner
            (log/prune-oldest! l (- (log/head l) 5))
            (let [floor (log/first-seq l)
                  on-disk (mapv seg/first-seq-of (seg/segment-files (io/file dir "log")))
                  held (mapv :first-seq (:segments @(:state l)))]
              (is (> floor 1))
              (is (= on-disk held) "a segment the prune took has not come back into the state")
              (is (= floor (first held)))
              (is (= 220 (log/head l)))
              (is (= (range floor 221) (map :event/seq (log/read-from l floor 1000))))))
          (finally (log/close! l)))))))

(deftest a-reader-holding-a-pruned-segment-stops-cleanly
  (with-small-segments
    (with-dir [dir]
      (let [l (log/open! dir {:fsync :never})
            s (random-uuid) a (random-uuid)]
        (try
          (dotimes [_ 8]
            (log/append!! l (log/pending :field-set s a {:name :t/x :type :string :value (apply str (repeat 3000 "x")) :reader :both})))
          (let [stale @(:state l)]
            (is (seq (reader/read-from stale 1 2)) "the reader opens its channel")
            (is (log/prune-oldest! l (log/head l)))
            (let [events (reader/read-from stale 1 100)]
              (is (= (range (log/first-seq l) (inc (log/head l))) (map :event/seq events))
                  "it sees what is left, no exception, and asking log/gap tells it what it missed")
              (is (= (log/first-seq l) (log/gap l 1)))
              (is (seq (reader/read-stream stale s 0 100)))))
          (finally (log/close! l)))))))

(deftest a-hole-in-the-middle-of-the-log-is-damage
  (with-small-segments
    (with-dir [dir]
      (let [l (log/open! dir {:fsync :never})
            s (random-uuid) a (random-uuid)]
        (dotimes [_ 8]
          (log/append!! l (log/pending :field-set s a {:name :t/x :type :string :value (apply str (repeat 3000 "x")) :reader :both})))
        (log/close! l)
        (let [files (seg/segment-files (io/file dir "log"))
              ^File middle (nth files (quot (count files) 2))]
          (.delete middle)
          (.delete (seg/idx-file middle))
          (is (thrown-with-msg? clojure.lang.ExceptionInfo #"missing from the middle" (log/open! dir {:fsync :never}))))))))

;; ---------------------------------------------------------------- boot

(deftest a-pruned-world-boots-from-its-snapshot-whole
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)
            wid (:world-id world)]
        (log/append!! (:log world) (log/pending :field-set wid wid {:name :test/early :type :string :value "before the prune" :reader :both}))
        (let [early-seq (log/head (:log world))]
          (prune-all! world)
          (is (> (log/first-seq (:log world)) early-seq) "the event that set it is gone")
          (let [head (log/head (:log world))]
            (system/stop world)
            (let [w2 (boot dir)]
              (try
                (is (= "before the prune" (world-field w2 :test/early)))
                (is (>= (log/head (:log w2)) head) "no kernel-zero, no lost seqs")
                (is (= wid (:world-id w2)))
                (finally (system/stop w2))))))))))

(deftest a-rollback-that-moves-the-snapshots-aside-leaves-the-anchor
  ;; supervise.sh moves whatever a failed release wrote out of snapshots/, and prune! keeps only three
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)
            wid (:world-id world)]
        (log/append!! (:log world) (log/pending :field-set wid wid {:name :test/early :type :string :value "kept" :reader :both}))
        (prune-all! world)
        (system/stop world)
        (doseq [^File f (snap/snapshot-files (io/file dir "snapshots"))] (.delete f))
        (let [w2 (boot dir)]
          (try
            (is (= "kept" (world-field w2 :test/early)) "the anchor alone is enough")
            (finally (system/stop w2))))))))

(deftest a-pruned-world-with-nothing-to-load-refuses-rather-than-fold-part-of-itself
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)]
        (prune-all! world)
        (let [floor (log/first-seq (:log world))]
          (is (> floor 1))
          (system/stop world)
          (doseq [^File f (concat (snap/snapshot-files (io/file dir "snapshots")) (snap/snapshot-files (snap/anchor-dir dir)))]
            (spit f "corrupt"))
          (let [log-before (segment-names dir)
                thrown (try (boot dir) nil (catch clojure.lang.ExceptionInfo e e))]
            (is (some? thrown) "no world")
            (is (= :log/unrecoverable (:type (ex-data thrown))))
            (is (= floor (:first-seq (ex-data thrown))))
            (is (str/includes? (ex-message thrown) "refusing to start"))
            (is (= log-before (segment-names dir)) "nothing was touched")))))))

(deftest a-snapshot-in-another-format-is-the-same-refusal
  ;; a release that changes schema-version, on a log it can no longer replay
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)]
        (prune-all! world)
        (system/stop world)
        (with-redefs [snap/schema-version 2]
          (is (= :log/unrecoverable
                 (:type (ex-data (try (boot dir) nil (catch clojure.lang.ExceptionInfo e e)))))))
        (testing "and the release that wrote them still boots: this is what a rollback relies on"
          (let [w2 (boot dir)]
            (is (some? (:index w2)))
            (system/stop w2)))))))

(deftest an-unpruned-world-still-owes-nothing-to-its-snapshots
  (with-dir [dir]
    (let [world (boot dir)
          wid (:world-id world)]
      (log/append!! (:log world) (log/pending :field-set wid wid {:name :test/early :type :string :value "v" :reader :both}))
      (system/stop world)
      (doseq [^File f (snap/snapshot-files (io/file dir "snapshots"))] (spit f "corrupt"))
      (let [w2 (boot dir)]
        (try
          (is (= "v" (world-field w2 :test/early)) "a replay from seq 1, as ever")
          (finally (system/stop w2)))))))

(deftest a-snapshot-under-an-older-schema-is-brought-up-to-this-one
  (let [wid (random-uuid)
        old-schema (dissoc fold/schema :grant/grantee)
        db (d/db-with (d/empty-db old-schema) [{:object/id wid :object/class :object}])
        up (fold/current-schema db)]
    (is (= fold/schema (:schema up)))
    (is (= :object (:object/class (d/entity up [:object/id wid]))))
    (is (identical? up (fold/current-schema up)) "a current db is left alone")))

(deftest a-release-that-can-still-be-rolled-back-takes-no-log
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir {:release "bbbb"})]
        (try
          (spit (io/file dir "release.good") "aaaa\n")
          (fill! world 12 3000)
          (snapshot! world)
          (fill! world 3 3000)
          (let [hex (put-blob! world "b" 9000)]
            (age! world hex 300)
            (let [r (retention/sweep! (assoc-in world [:config :retention-gb] 1.0e-9))]
              (is (= 1 (log/first-seq (:log world))) "the good release may not read what this one wrote")
              (is (nil? (snap/anchor-seq dir)))
              (is (str/includes? (str (:blocked r)) "not yet the good one"))
              (is (nil? (blob/get (:blobs world) hex)) "blobs still go: losing one is not losing the world")))
          (testing "once it is the good one the log can go"
            (spit (io/file dir "release.good") "bbbb\n")
            (retention/sweep! (assoc-in world [:config :retention-gb] 1.0e-9))
            (is (> (log/first-seq (:log world)) 1)))
          (finally (system/stop world)))))))

;; ---------------------------------------------------------------- readers of a pruned log

(deftest history-reports-a-gap-and-paging-ends
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)
            root (fx/attach-root world)]
        (try
          (let [root-id (:principal root)
                early (log/head (:log world))
                _ (prune-all! world)
                floor (log/first-seq (:log world))]
            (is (> floor early))
            (testing "a page from before the log begins"
              (let [r (ws/request!! root :history {:after 0 :limit 5})]
                (is (:ok r))
                (is (= floor (get-in r [:result :gap])))
                (is (<= floor (:event/seq (first (get-in r [:result :events])))))))
            (testing "a page inside what is held has no gap"
              (is (nil? (get-in (ws/request!! root :history {:after (log/head (:log world)) :limit 5}) [:result :gap]))))
            (testing "one stream"
              (let [r (ws/request!! root :history {:stream root-id :after 0 :limit 5})]
                (is (:ok r))
                (is (= floor (get-in r [:result :gap])))))
            (testing "prev paging walks down to the first seq and stops"
              (loop [before nil pages 0]
                (let [r (ws/request!! root :history (cond-> {:last 5} before (assoc :before before)))
                      {:keys [prev gap events]} (:result r)]
                  (is (:ok r))
                  (is (< pages 2000) "it ends")
                  (is (every? #(>= (:event/seq %) floor) events))
                  (if prev
                    (recur prev (inc pages))
                    (is (= floor gap))))))
            (testing "below the first seq: an empty page and the gap, at once"
              (let [{:keys [events gap prev]} (:result (ws/request!! root :history {:before 3 :last 5}))]
                (is (empty? events))
                (is (= floor gap))
                (is (nil? prev))))
            (testing "a client's snapshot boot does not depend on the old log at all"
              (let [r (ws/request!! root :history {:snapshot true :limit 10})]
                (is (:ok r))
                (is (pos? (get-in r [:result :snapshot :total])))
                (is (nil? (get-in r [:result :gap])))))
            (testing "hello says where the log starts"
              (is (= floor (get-in (ws/request!! root :hello {}) [:result :first-seq])))))
          (finally (ws/close! root) (system/stop world)))))))

(deftest a-read-at-a-seq-the-log-no-longer-reaches-says-so
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)
            root (fx/attach-root world)]
        (try
          (let [wid (:world-id world)
                old (random-uuid)
                _ (log/append!! (:log world) (log/pending :created old wid {:class :object :fields {}}))
                _ (log/append!! (:log world) (log/pending :field-set old wid {:name :test/a :type :string :value "v" :reader :both}))
                _ (fx/grant! root (:principal root) #{old} #{:read :write})
                at (log/head (:log world))]
            (is (:ok (ws/request!! root :read {:object old :seq at})))
            (prune-all! world)
            (is (= :pruned (code (ws/request!! root :read {:object old :seq at}))))
            (is (:ok (ws/request!! root :read {:object old})) "the state now is all there")
            (testing "an Object born after the first seq keeps its past"
              (let [new (random-uuid)]
                (log/append!! (:log world) (log/pending :created new wid {:class :object :fields {}}))
                (log/append!! (:log world) (log/pending :field-set new wid {:name :test/a :type :string :value "w" :reader :both}))
                (fx/grant! root (:principal root) #{new} #{:read :write})
                (is (:ok (ws/request!! root :read {:object new :seq (log/head (:log world))})))))
            (testing "field.diff on changes that have gone"
              (let [r (act! root "field.diff" {:object (str old) :field "test/a"})]
                (is (:ok r) (pr-str r))
                (is (false? (get-in r [:result :result :changed])))
                (is (pos? (get-in r [:result :result :gap]))))))
          (finally (ws/close! root) (system/stop world)))))))

(deftest a-restored-object-whose-retirement-was-pruned-says-its-names-are-gone
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)
            root (fx/attach-root world)]
        (try
          (let [id (get-in (ws/request!! root :act {:class :object :action :create :args {:fields {}}}) [:result :object])]
            (is (:ok (act! root "object.retire" {:object (str id)})))
            (prune-all! world)
            (let [r (act! root "object.restore" {:object (str id)})]
              (is (:ok r) (pr-str r))
              (is (true? (get-in r [:result :result :names-pruned])))))
          (finally (ws/close! root) (system/stop world)))))))

(deftest a-subscription-behind-the-log-is-told-and-goes-on
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)
            root (fx/attach-root world)]
        (try
          (prune-all! world)
          (let [floor (log/first-seq (:log world))
                sub (ws/request!! root :subscribe {:sub "h" :after 0 :window 1000})]
            (is (= floor (get-in sub [:result :gap])) "the answer says it")
            (let [p (ws/await-push root "h")]
              (is (= floor (:gap p)) "and the sub's first push")
              (ws/ack! root "h" (:event/seq (peek (:events p)))))
            (testing "a first subscribe at the head has no gap"
              (let [r (ws/request!! root :subscribe {:sub "now"})]
                (is (nil? (get-in r [:result :gap])))))
            (testing "a sub with nothing to show past the gap still hears it"
              (let [none (random-uuid)
                    r (ws/request!! root :subscribe {:sub "quiet" :stream none :after 0})
                    p (ws/await-push root "quiet")]
                (is (= floor (get-in r [:result :gap])))
                (is (= floor (:gap p)))
                (is (empty? (:events p)))
                (fill! world 1 10)
                (is (nil? (some-> (ws/await-frame root (fn [f] (and (ws/push? f) (= "quiet" (:sub f)))) 300) :gap))
                    "once"))))
          (finally (ws/close! root) (system/stop world)))))))

(deftest a-reattach-from-below-the-first-seq-reports-the-gap-and-delivers-the-rest
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)
            c (fx/attach-root world)]
        (try
          (let [root-id (:principal c)
                session (:session c)
                _ (ws/request!! c :subscribe {:sub "h" :stream root-id})
                acked (log/head (:log world))]
            (ws/abort! c)
            (Thread/sleep 100)
            (prune-all! world)
            (let [floor (log/first-seq (:log world))
                  _ (is (> floor (inc acked)))
                  c2 (ws/connect (:port world) :transit)
                  r (ws/request!! c2 :attach {:token (fx/root-token world) :session session :subs {"h" acked}})]
              (try
                (is (true? (get-in r [:result :resumed])))
                (is (= floor (get (get-in r [:result :gaps]) "h")))
                (let [p (ws/await-push c2 "h")]
                  (is (= floor (:gap p))))
                (finally (ws/close! c2)))))
          (finally (system/stop world)))))))

(deftest tools-that-fold-from-the-start-say-they-cannot
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir)]
        (prune-all! world)
        (system/stop world)
        (is (thrown-with-msg? clojure.lang.ExceptionInfo #"pruned" (tools/fold-at dir)))))))

(deftest an-export-of-a-pruned-world-boots
  (with-small-segments
    (with-dir [dir]
      (with-dir [other]
        (let [world (boot dir)
              wid (:world-id world)]
          (log/append!! (:log world) (log/pending :field-set wid wid {:name :test/early :type :string :value "v" :reader :both}))
          (prune-all! world)
          (system/stop world)
          (doseq [^File f (snap/snapshot-files (io/file dir "snapshots"))] (.delete f))
          (let [archive (str (io/file other "w.tgz"))
                target (io/file other "imported")]
            (tools/export! dir archive)
            (tools/import! archive target)
            (let [w2 (boot (str target))]
              (try
                (is (= "v" (world-field w2 :test/early)) "the anchor travels with the world")
                (finally (system/stop w2))))))))))

;; ---------------------------------------------------------------- forks

(defn- bare!
  "A closed log-only world of many small segments, with a snapshot at seq `snapshot-at` by name only
   when given, plus a verified anchor when asked."
  [dir n]
  (spit (io/file dir "world.edn") (pr-str {:world/id (random-uuid) :format-version 1}))
  (let [l (log/open! dir {:fsync :never})
        s (random-uuid) a (random-uuid)]
    (dotimes [_ n]
      (log/append!! l (log/pending :field-set s a {:name :t/x :type :string :value (apply str (repeat 3000 "x")) :reader :both})))
    (log/close! l)))

(deftest a-fork-holds-its-parents-log-while-it-lives
  (with-small-segments
    (with-dir [pdir]
      (with-dir [cbase]
        (bare! pdir 20)
        (let [child (io/file cbase "child")
              {:keys [at]} (tools/fork! pdir child :at 10)]
          (is (= 10 (log/fork-ceiling pdir)))
          (testing "a note whose child has gone does not hold the log"
            (wipe! child)
            (is (nil? (log/fork-ceiling pdir)))
            (is (empty? (.listFiles (io/file pdir "forks")))))
          (is (= 10 at)))))))

(deftest a-sweep-stops-at-what-a-fork-reads
  (with-small-segments
    (with-dir [dir]
      (with-dir [cbase]
        (let [world (boot dir)]
          (try
            (fill! world 12 3000)
            (let [at (log/head (:log world))]
              (snapshot! world)
              (log/mark-fork! dir {:fork "f" :child (str (io/file cbase "c")) :seq 3})
              (spit (io/file (doto (io/file cbase "c") (.mkdirs)) "world.edn") (pr-str {:fork/id "f"}))
              (fill! world 8 3000)
              (let [r (retention/sweep! (assoc-in world [:config :retention-gb] 1.0e-9))]
                (is (= 1 (log/first-seq (:log world))) "seq 3 is in the first segment")
                (is (str/includes? (str (:blocked r)) "fork")))
              (is (pos? at)))
            (finally (system/stop world))))))))

(deftest a-fork-keeps-its-own-log
  (with-small-segments
    (with-dir [pdir]
      (with-dir [cbase]
        (let [parent (boot pdir)]
          (snapshot! parent)
          (system/stop parent))
        (let [child (io/file cbase "child")
              _ (tools/fork! pdir child)
              w (boot (str child))]
          (try
            (fill! w 12 3000)
            (snapshot! w)
            (fill! w 3 3000)
            (let [before (segment-names child)
                  r (retention/sweep! (assoc-in w [:config :retention-gb] 1.0e-9))]
              (is (every? (set (segment-names child)) before) "world.diff and world-promote read them")
              (is (str/includes? (str (:blocked r)) "fork keeps")))
            (finally (system/stop w))))))))

(deftest a-fork-is-refused-where-the-parent-has-pruned
  (with-small-segments
    (with-dir [dir]
      (with-dir [cbase]
        (let [world (boot dir)]
          (prune-all! world)
          (let [floor (log/first-seq (:log world))]
            (system/stop world)
            (testing "below the first segment"
              (is (thrown-with-msg? clojure.lang.ExceptionInfo #"no longer holds"
                                    (tools/fork! dir (io/file cbase "low") :at 2))))
            (testing "at the head it works, from the anchor, and the child boots"
              (doseq [^File f (snap/snapshot-files (io/file dir "snapshots"))] (.delete f))
              (let [child (io/file cbase "ok")]
                (tools/fork! dir child)
                (let [w (boot (str child))]
                  (try
                    (is (= floor (log/first-seq (:log w))))
                    (finally (system/stop w))))))))))))

(deftest a-promote-will-not-skip-events-a-fork-has-lost
  (with-small-segments
    (with-dir [pdir]
      (with-dir [cbase]
        (bare! pdir 3)
        (let [child (io/file cbase "child")]
          (tools/fork! pdir child)
          (let [l (log/open! child {:fsync :never :parent {:data-dir (str pdir) :seq 3}})
                s (random-uuid) a (random-uuid)]
            (dotimes [_ 8]
              (log/append!! l (log/pending :field-set s a {:name :t/x :type :string :value (apply str (repeat 3000 "x")) :reader :both})))
            (is (log/prune-oldest! l (log/head l)))
            (log/close! l))
          (is (thrown-with-msg? clojure.lang.ExceptionInfo #"no longer holds all" (tools/own-events child))))))))

;; ---------------------------------------------------------------- the limit, and who sets it

(deftest the-limit-comes-from-a-person-then-the-environment-then-the-default
  (with-dir [dir]
    (let [world (boot dir)]
      (try
        (is (= 16 (retention/limit-gb world)))
        (is (= 40.0 (retention/limit-gb (assoc-in world [:config :retention-gb] 40.0))))
        (is (= "infinite" (retention/limit-gb (assoc-in world [:config :retention-gb] "infinite"))))
        (finally (system/stop world))))))

(deftest ke-retention-gb-is-read
  (let [parse #'knowledge.world.config/retention-gb]
    (is (= 2.5 (parse "2.5")))
    (is (= "infinite" (parse "Infinite")))
    (is (nil? (parse "lots")) "a typo is the default, not no limit")
    (is (nil? (parse "0")))
    (is (nil? (parse "-1")))))

(deftest infinite-turns-pruning-off
  (with-small-segments
    (with-dir [dir]
      (let [world (boot dir {:retention-gb "infinite"})]
        (try
          (let [hex (put-blob! world "b" 9000)]
            (age! world hex 3000)
            (fill! world 12 3000)
            (snapshot! world)
            (fill! world 3 3000)
            (let [r (retention/sweep! world)]
              (is (zero? (:bytes r)))
              (is (nil? (:over r)))
              (is (some? (blob/get (:blobs world) hex)))
              (is (= 1 (log/first-seq (:log world))))
              (is (nil? (snap/anchor-seq dir)))))
          (finally (system/stop world)))))))

(deftest only-a-person-sets-the-limit
  (with-dir [dir]
    (let [world (boot dir)
          root (fx/attach-root world)
          {:keys [client]} (person! world root ["retention.set"])
          wid (:world-id world)]
      (try
        (testing "not root, not an agent, whatever it can reach"
          (is (= :person-only (code (act! root "retention.set" {:gb 1})))))
        (testing "no write of the Field gets past, even a person's: the action keeps it valid"
          (is (= :conflict (code (ws/request!! root :set-field {:object wid :field :retention/limit-gb :type :string :value "infinite" :reader :both}))))
          (is (= :conflict (code (ws/request!! client :set-field {:object wid :field :retention/limit-gb :type :string :value "infinite" :reader :both}))))
          (is (= :conflict (code (ws/request!! root :set-field {:object wid :field :retention/last-sweep :type :map :value {} :reader :both}))))
          (is (= :conflict (:code (:err (authority/write-refusal ":retention/limit-gb" true)))) "however it is spelled")
          (is (= :conflict (:code (:err (authority/write-refusal "retention/last-sweep" true)))))
          (is (= :conflict (:code (:err (authority/clear-refusal :retention/limit-gb true)))))
          (is (nil? (world-field world retention/limit-field))))
        (testing "a person's own session"
          (let [r (act! client "retention.set" {:gb 1})]
            (is (:ok r) (pr-str r))
            (is (= 1 (world-field world retention/limit-field)))
            (is (= 1 (retention/limit-gb world))))
          (is (:ok (act! client "retention.set" {:gb "infinite"})))
          (is (= "infinite" (retention/limit-gb world))))
        (testing "a limit below what the world uses is a preview until confirmed"
          (put-blob! world "big" 20000)
          (let [tiny (/ 1.0 1024 1024) ; a kilobyte, in GiB
                r (act! client "retention.set" {:gb tiny})]
            (is (:ok r) (pr-str r))
            (is (true? (get-in r [:result :result :not-set])))
            (is (pos? (long (get-in r [:result :result :would-go]))))
            (is (= "infinite" (retention/limit-gb world)) "nothing set, nothing pruned")
            (is (:ok (act! client "retention.set" {:gb tiny :confirm true})))
            (is (= tiny (retention/limit-gb world)))
            (is (:ok (act! client "retention.set" {:gb "infinite"})))))
        (testing "nonsense"
          (is (= :invalid-args (code (act! client "retention.set" {:gb 0}))))
          (is (some? (:err (act! client "retention.set" {:gb "lots"}))))
          (is (= "infinite" (retention/limit-gb world))))
        (finally (ws/close! client) (ws/close! root) (system/stop world))))))

(deftest a-fork-does-not-promote-its-disk-settings
  (is (not (tools/promotable? {:event/kind :field-set :event/body {:name :retention/limit-gb}})))
  (is (not (tools/promotable? {:event/kind :field-set :event/body {:name :retention/last-sweep}}))))

(deftest a-person-can-see-what-a-sweep-took
  (with-dir [dir]
    (let [world (boot dir)]
      (try
        (let [hex (put-blob! world "b" 9000)]
          (age! world hex 300)
          (is (nil? (world-field world retention/sweep-field)) "a sweep with nothing to do says nothing")
          (retention/sweep! world)
          (is (nil? (world-field world retention/sweep-field)))
          (let [r (retention/sweep! (limited world -5000))
                f (world-field world retention/sweep-field)]
            (is (= (:bytes r) (:bytes f)))
            (is (= 1 (:count f)))
            (is (pos? (:bytes f)))
            (is (string? (:at f)))
            (is (= 1 (:first-seq f)))))
        (finally (system/stop world))))))

(deftest the-scheduler-runs-a-sweep
  (with-dir [dir]
    (let [world (boot dir {:retention (assoc knobs :every-ms 50) :retention-gb 1.0e-9})]
      (try
        (let [hex (put-blob! world "b" 9000)]
          (age! world hex 300)
          (let [deadline (+ (System/currentTimeMillis) 8000)]
            (loop []
              (when (and (some? (blob/get (:blobs world) hex)) (< (System/currentTimeMillis) deadline))
                (age! world hex 300)
                (Thread/sleep 50)
                (recur))))
          (is (nil? (blob/get (:blobs world) hex))))
        (finally (system/stop world))))))

;; ---------------------------------------------------------------- text logs and releases

(deftest a-text-log-rotates-under-a-writer-that-holds-it-open
  (with-dir [dir]
    (let [logs (doto (io/file dir "logs") (.mkdirs))
          f (io/file logs "world.log")]
      (with-open [out (java.io.FileOutputStream. f true)] ; append mode, as >> opens it
        (.write out (.getBytes (apply str (repeat 2000 "a"))))
        (.flush out)
        (spit (io/file logs "small.log") "small")
        (spit (io/file logs "world.log.1") "older")
        (is (= 1 (retention/rotate-logs! dir {:log-max-bytes 1000 :log-keep 2})))
        (is (= 2000 (.length (io/file logs "world.log.1"))) "what was there is kept")
        (is (= "older" (slurp (io/file logs "world.log.2"))))
        (is (zero? (.length f)))
        (.write out (.getBytes "next line"))
        (.flush out)
        (is (= "next line" (slurp f)) "the open writer carries on at the new end, no hole"))
      (is (= "small" (slurp (io/file logs "small.log"))))
      (testing "generations are bounded"
        (spit f (apply str (repeat 2000 "b")))
        (retention/rotate-logs! dir {:log-max-bytes 1000 :log-keep 2})
        (is (not (.exists (io/file logs "world.log.3"))))
        (is (= 2000 (.length (io/file logs "world.log.2"))))))))

(defn- release! [dir n & {:keys [link]}]
  (let [name (apply str (repeat 40 (str n)))
        d (io/file dir "releases" name)
        sub (io/file d "world")]
    (.mkdirs sub)
    (spit (io/file sub "a.clj") "x")
    (spit (io/file d "release.edn") "{}")
    (when link
      (Files/createSymbolicLink (.toPath (io/file sub "out")) (.toPath (io/file link)) (make-array FileAttribute 0)))
    (doseq [^File f (reverse (file-seq d))] (.setWritable f false false))
    (.setLastModified d (+ 1000000000000 (* n 86400000)))
    name))

(deftest old-releases-go-and-what-a-rollback-needs-stays
  (with-dir [dir]
    (with-dir [outside]
      (let [precious (doto (io/file outside "precious") (spit "keep"))
            [r1 r2 r3 r4 r5 r6] (mapv #(release! dir %) [1 2 3 4 5 6])
            old-linked (release! dir 7 :link outside)
            _ (.setLastModified (io/file dir "releases" old-linked) 900000000000)
            src (doto (io/file dir "src") (.mkdirs))
            work (doto (io/file src "work.txt") (spit "mine"))
            tmp (doto (io/file dir "releases" ".tmp-abc") (.mkdirs))
            world {:data-dir (io/file dir) :config {:release r2}}]
        (spit (io/file dir "release") (str r2 "\n"))
        (spit (io/file dir "release.good") (str r1 "\n"))
        (spit (io/file dir "restart.request") (str "release=" r3 "\nreason=x\n"))
        (let [removed (set (retention/trim-releases! world {:keep-releases 2}))]
          (is (= #{old-linked r4} removed))
          (doseq [r [r1 r2 r3 r5 r6]]
            (is (.isDirectory (io/file dir "releases" r)) r))
          (is (not (.exists (io/file dir "releases" r4))))
          (is (.exists tmp) "a build in progress is not a release")
          (is (= "keep" (slurp precious)) "a link in a release is removed, never followed")
          (is (= "mine" (slurp work)) "src is never touched"))
        (testing "no release file, no supervisor: nothing moves"
          (.delete (io/file dir "release"))
          (is (empty? (retention/trim-releases! world {:keep-releases 1}))))))))
