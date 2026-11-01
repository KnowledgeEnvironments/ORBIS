(ns knowledge.world.scratch-test
  "Off-the-record sessions: an in-memory fork under a Principal. Reads resolve as that Principal,
   writes vanish, anything past the world process is refused, nothing reaches the disk except the
   audit stub, and a person alone can open one."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.protocol.ops :as pops]
            [knowledge.world.scratch.core :as scratch]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]
            [taoensso.timbre :as timbre])
  (:import [java.nio.file Files]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args} 20000))
(defn- res [r] (get-in r [:result :result]))
(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))

(defn- until [f ms]
  (let [end (+ (System/currentTimeMillis) ms)]
    (loop []
      (or (f) (when (< (System/currentTimeMillis) end) (Thread/sleep 25) (recur))))))

(defn- person! [root witness & [ops]]
  (fx/grant! root (:id witness) :everything (or ops #{:read :reference :transmit :evaluate})))

(defn- own-events [world id]
  (let [child (:child @(get @(:scratches world) id))]
    (log/read-own-from (:log child) 0 100000)))

(defn- idle? [world id] (not (:busy @(get @(:scratches world) id))))

(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))

(defn- files-under [dir]
  (into {} (for [^java.io.File f (file-seq (io/file dir)) :when (.isFile f)]
             [(str (.relativize (.toPath (io/file dir)) (.toPath f))) (Files/readAllBytes (.toPath f))])))

(defn- holds? [^bytes bs ^String marker]
  (str/includes? (String. bs "ISO-8859-1") marker))

(deftest only-a-person-opens-one
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)]
      (person! root witness)
      (testing "a process Principal's session is refused"
        (is (= :session-only (get-in (act! root "scratch.begin" {:principal (str root-id)}) [:err :code]))))
      (testing "a loop is refused, whatever its Grants"
        (let [ctx {:world world :loop? true :conn (atom {:session (atom {:id "loop" :principal root-id})})}]
          (is (= :session-only (get-in (pops/handle ctx :act {:action :scratch.begin :args {:principal root-id}}) [:err :code])))))
      (testing "a model is never told: no action Object and nothing in the Actions block"
        (is (nil? (resolve-as root "action/scratch.begin")))
        (is (not (str/includes? (str (actions/acting-text (act/db world) root-id)) "scratch")))
        (is (not (str/includes? ser/acting "scratch"))))
      (testing "a person needs evaluate on the Principal"
        (let [other (fx/attach-as world (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :person :name "bystander"}}) [:result :result :token]))]
          (is (= :unresolved (get-in (act! other "scratch.begin" {:principal (str root-id)}) [:err :code]))))))))

(deftest nothing-reaches-the-disk
  (let [dir (fx/temp-dir)
        world (fx/start-world dir 0)
        marker "OFFTHERECORD-7f3a91"
        seen (atom [])
        capture {:enabled? true :min-level nil :fn (fn [d] (swap! seen conj (force (:output_ d))))}]
    (timbre/merge-config! {:appenders {:scratch-test capture}})
    (try
      (let [root (fx/attach-root world)
            witness (fx/create-witness! root)
            _ (person! root witness)
            root-id (:principal root)
            c (fx/attach-as world (:token witness) :json)
            before-head (log/head (:log world))
            before (files-under dir)
            r (act! c "scratch.begin" {:principal (str root-id)})
            id (parse-uuid (str (:scratch (res r))))
            pad (apply str (repeat 900 "lorem ipsum "))
            s (act! c "scratch.send" {:scratch (str id) :text (str marker " " pad)})]
        (is (:ok r) (pr-str r))
        (is (:ok s) (pr-str s))
        (is (= before-head (:forked-at (res r))) "nothing logged before the audit stub")
        (is (until #(idle? world id) 20000))
        (testing "the Model really ran, and the push reached the session tagged"
          (let [events (own-events world id)]
            (is (some #(and (= :evaluated (:event/kind %)) (= :done (get-in % [:event/body :state]))) events))
            (is (some #(= :field-read (:event/kind %)) events) "reads happened, in memory"))
          (let [p (ws/await-frame c ws/push? 5000)]
            (is (= (str id) (:scratch p)) (pr-str (dissoc p :events)))))
        (testing "only the stub is added to the real log"
          (is (= (inc before-head) (log/head (:log world))) "scratch-began alone so far"))
        (is (:ok (act! c "scratch.end" {:scratch (str id)})))
        (is (= (+ 2 before-head) (log/head (:log world))))
        (ws/close! c)
        (ws/close! root)
        (system/stop world)
        (testing "byte-for-byte: no new blob or file with content, the marker nowhere"
          (let [after (files-under dir)
                blobs (fn [m] (set (filter #(str/starts-with? % "blobs") (keys m))))]
            (is (= (blobs before) (blobs after)) "no blob written")
            (is (not-any? (fn [[f bs]] (holds? bs marker)) after) "marker on disk")
            (doseq [[f bs] before :when (not (re-find #"^log/|^snapshots/|world.pid" f))]
              (is (java.util.Arrays/equals ^bytes bs ^bytes (get after f)) (str f " changed")))))
        (testing "no log line carried the text"
          (is (not-any? #(str/includes? (str %) marker) @seen))))
      (finally
        (timbre/merge-config! {:appenders {:scratch-test nil}})
        (fx/delete-tree! dir)))))

(deftest writes-vanish-and-the-outside-is-refused
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          _ (person! root witness #{:read :reference :transmit :evaluate :write})
          c (fx/attach-as world (:token witness))
          id (:scratch (res (act! c "scratch.begin" {:principal root-id})))
          shell (resolve-as root "shell.local")
          other (:id witness)
          block (ops-block
                  (format "{\"op\":\"set-field\",\"args\":{\"object\":\"%s\",\"field\":\"memory/notes\",\"type\":\"string\",\"value\":\"secret note\"}}" root-id)
                  (format "{\"op\":\"read\",\"args\":{\"object\":\"%s\"}}" root-id)
                  (format "{\"op\":\"act\",\"args\":{\"object\":\"%s\",\"action\":\"run\",\"args\":{\"cmd\":\"ls\"}}}" shell)
                  (format "{\"op\":\"act\",\"args\":{\"action\":\"task.run\",\"args\":{\"adapter\":\"%s\",\"cmd\":\"ls\"}}}" shell)
                  (format "{\"op\":\"send\",\"args\":{\"to\":[\"%s\"],\"route\":\"%s\",\"text\":\"out\"}}" root-id (resolve-as root "root.route"))
                  (format "{\"op\":\"grant\",\"args\":{\"grantee\":\"%s\",\"scope\":\"everything\",\"ops\":[\"read\"]}}" other)
                  (format "{\"op\":\"bind\",\"args\":{\"namespace\":\"%s\",\"name\":\"x\",\"object\":\"%s\"}}" root-id root-id)
                  "{\"op\":\"act\",\"args\":{\"action\":\"token.issue\",\"args\":{\"principal\":\"x\",\"label\":\"y\"}}}"
                  "{\"op\":\"act\",\"args\":{\"action\":\"world.send\",\"args\":{\"world\":\"x\",\"text\":\"y\"}}}")
          real-before (get-in (objects/fields (act/db world) root-id) [:memory/notes :field/value])]
      (is (:ok (act! c "scratch.send" {:scratch id :text block})))
      (is (until #(idle? world id) 20000))
      (let [entry @(get @(:scratches world) id)
            child (:child entry)
            cdb (act/db child)
            results (->> (own-events world id)
                         (filter #(= :evaluation/results (get-in % [:event/body :name])))
                         first :event/body :value)
            by-op (mapv (juxt :op :ok #(get-in % [:err :code])) results)]
        (testing "the note landed in the child alone"
          (is (= "secret note" (get-in (objects/fields cdb root-id) [:memory/notes :field/value])))
          (is (= real-before (get-in (objects/fields (act/db world) root-id) [:memory/notes :field/value]))))
        (testing "reads ok, the rest scratch-readonly"
          (is (= [true true] (mapv second (take 2 by-op))) (pr-str by-op))
          (is (= 7 (count (drop 2 by-op))) (pr-str by-op))
          (is (every? #(contains? #{:scratch-readonly "scratch-readonly"} (nth % 2)) (drop 2 by-op)) (pr-str by-op)))
        (testing "the prompt says so"
          (let [ser-id (some #(when (= :serialization (get-in % [:event/body :class])) (:event/stream %)) (own-events world id))
                bs (ser/bytes-of child (objects/state cdb ser-id))]
            (is (str/includes? (str (if (bytes? bs) (String. ^bytes bs "UTF-8") bs)) "off-the-record session"))))
        (testing "no Task, no Message to anyone but the person"
          (is (empty? (filter #(= :task-moved (:event/kind %)) (own-events world id))))))
      (act! c "scratch.end" {:scratch id})
      (testing "forgotten"
        (is (nil? (get @(:scratches world) (parse-uuid (str id)))))))))

(deftest a-conversation-continues-and-only-its-opener-may-use-it
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          _ (person! root witness)
          c (fx/attach-as world (:token witness))
          id (:scratch (res (act! c "scratch.begin" {:principal root-id})))]
      (is (:ok (act! c "scratch.send" {:scratch id :text "one"})))
      (is (until #(idle? world id) 20000))
      (is (:ok (act! c "scratch.send" {:scratch id :text "two"})))
      (is (until #(idle? world id) 20000))
      (let [e (filter #(= :message (get-in % [:event/body :class])) (own-events world id))
            threads (set (map #(get-in % [:event/body :fields :message/thread]) e))]
        (is (= 4 (count e)) "two asked, two answered")
        (is (= 1 (count threads)) "one conversation"))
      (let [c2 (fx/attach-as world (:token witness))]
        (is (= :unresolved (get-in (act! c2 "scratch.send" {:scratch id :text "not mine"}) [:err :code])) "another session is not its opener"))
      (is (= :invalid (get-in (act! c "scratch.send" {:scratch id :text " "}) [:err :code])))
      (is (idle? world id) "a refused send leaves it free"))))

(deftest stop-reaches-its-own-and-the-audit-is-honest
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          _ (person! root witness #{:read :reference :transmit :evaluate :write})
          model (resolve-as root "root.model")
          _ (ws/request!! root :set-field {:object model :field :model/stub-delta-ms :type :number :value 200 :reader :both})
          c (fx/attach-as world (:token witness))
          id (:scratch (res (act! c "scratch.begin" {:principal root-id})))]
      (is (:ok (act! c "scratch.send" {:scratch id :text "slow"})))
      (testing "a second line while it answers is refused"
        (is (= :conflict (get-in (act! c "scratch.send" {:scratch id :text "again"}) [:err :code]))))
      (testing "the real Principal's STOP does not see it"
        (is (until #(some (fn [e] (= :running (get-in e [:event/body :state]))) (own-events world id)) 10000))
        (is (nil? (evaluate/running-evaluation root-id))))
      (let [spent (res (act! c "scratch.end" {:scratch id}))]
        (is (= "ended" (:reason spent)) (pr-str spent))
        (is (not-any? #(str/includes? (pr-str %) "slow") [spent])))
      (let [stub (filter #(#{:scratch-began :scratch-ended} (:event/kind %))
                         (log/read-stream (:log world) (:id witness) 0 100000))]
        (is (= [:scratch-began :scratch-ended] (mapv :event/kind stub)))
        (is (not-any? #(str/includes? (pr-str %) "slow") stub) "no text in the stub")
        (is (= #{:scratch :principal :messages :tokens-in :tokens-out :duration-ms :reason} (set (keys (:event/body (last stub)))))))
      (testing "root cannot turn the audit off"
        (is (= :person-only (get-in (ws/request!! root :set-field {:object (:world-id world) :field :policy/scratch-audit :type :boolean :value false :reader :both}) [:err :code])))
        (let [h (log/head (:log world))
              s2 (:scratch (res (act! c "scratch.begin" {:principal root-id})))]
          (is (= (inc h) (log/head (:log world))))
          (act! c "scratch.end" {:scratch s2})))
      (testing "a person setting it off stops the stub"
        (is (:ok (ws/request!! c :set-field {:object (:world-id world) :field :policy/scratch-audit :type :boolean :value false :reader :both})))
        (let [h (log/head (:log world))
              s3 (:scratch (res (act! c "scratch.begin" {:principal root-id})))]
          (act! c "scratch.end" {:scratch s3})
          (is (= h (log/head (:log world)))))))))

(deftest it-ends-with-its-session-and-its-lifetime
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          _ (person! root witness)
          c (fx/attach-as world (:token witness))
          a (parse-uuid (str (:scratch (res (act! c "scratch.begin" {:principal root-id})))))
          b (parse-uuid (str (:scratch (res (act! c "scratch.begin" {:principal root-id})))))]
      (scratch/sweep! world)
      (is (= #{a b} (set (keys @(:scratches world)))) "both live")
      (scratch/sweep! (assoc-in world [:config :scratch-max-s] -1))
      (is (empty? @(:scratches world)) "past the lifetime")
      (let [d (parse-uuid (str (:scratch (res (act! c "scratch.begin" {:principal root-id})))))
            s (:session @(get @(:scratches world) d))]
        (swap! s assoc :detached-at 1)
        (scratch/sweep! world)
        (is (empty? @(:scratches world)) "the session is past its TTL")
        (is (some? d))))))

(deftest stop-ends-the-call-and-the-session-goes-on
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          _ (person! root witness #{:read :reference :transmit :evaluate :write})
          model (resolve-as root "root.model")
          _ (ws/request!! root :set-field {:object model :field :model/stub-delta-ms :type :number :value 200 :reader :both})
          c (fx/attach-as world (:token witness))
          id (:scratch (res (act! c "scratch.begin" {:principal root-id})))
          sid (parse-uuid (str id))]
      (is (:ok (act! c "scratch.send" {:scratch id :text "slow"})))
      (is (until #(some (fn [e] (= :running (get-in e [:event/body :state]))) (own-events world id)) 10000))
      (is (true? (:stopped (res (act! c "scratch.stop" {:scratch id})))))
      (is (until #(idle? world sid) 10000) "the session is free again")
      (is (some (fn [e] (= :cancelled (get-in e [:event/body :state]))) (own-events world id)))
      (is (false? (:stopped (res (act! c "scratch.stop" {:scratch id})))) "stopping nothing is not an error")
      (is (:ok (act! c "scratch.send" {:scratch id :text "next"})))
      (act! c "scratch.end" {:scratch id}))))

(deftest reasoning-is-the-sessions-own
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          _ (person! root witness #{:read :reference :transmit :evaluate :write})
          c (fx/attach-as world (:token witness))
          id (:scratch (res (act! c "scratch.begin" {:principal root-id})))
          sid (parse-uuid (str id))
          mode-of-last (fn []
                         (some->> (own-events world id)
                                  (filter #(= :evaluation/reasoning (get-in % [:event/body :name])))
                                  last :event/body :value))
          created-mode (fn []
                         (some->> (own-events world id)
                                  (filter #(and (= :created (:event/kind %)) (= :evaluation (get-in % [:event/body :class]))))
                                  last (#(get-in % [:event/body :fields :evaluation/reasoning]))))]
      (is (:ok (act! c "scratch.send" {:scratch id :text "one" :reasoning "on"})))
      (is (until #(idle? world sid) 10000))
      (is (= :on (or (created-mode) (mode-of-last))) "this session reasons")
      (is (:ok (act! c "scratch.send" {:scratch id :text "two"})))
      (is (until #(idle? world sid) 10000))
      (is (= :on (or (created-mode) (mode-of-last))) "it stays on until changed")
      (is (:ok (act! c "scratch.send" {:scratch id :text "three" :reasoning "off"})))
      (is (until #(idle? world sid) 10000))
      (is (= :off (or (created-mode) (mode-of-last))))
      (is (nil? (get-in (objects/fields (act/db world) root-id) [:loop/reasoning :field/value]))
          "the real Principal's Field is untouched")
      (act! c "scratch.end" {:scratch id}))))
