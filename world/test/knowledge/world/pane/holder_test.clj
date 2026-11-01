(ns knowledge.world.pane.holder-test
  "Terminals in a real holder process (holder/, docs/panes.md). They need its binary: `make holder`
   builds it, and without it these say so and pass nothing."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing use-fixtures]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.index.core :as index]
            [knowledge.world.index.snapshot :as snap]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.log.segment :as seg]
            [knowledge.world.pane.core :as core]
            [knowledge.world.retention :as retention]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]))

(def binary (io/file "../holder/build/holder"))

(defn- until [f ms]
  (let [end (+ (System/currentTimeMillis) ms)]
    (loop []
      (or (f)
          (when (< (System/currentTimeMillis) end)
            (Thread/sleep 25)
            (recur))))))

(defn- start-holder [dir]
  (let [sock (str (io/file dir "holder.sock"))
        p (.start (ProcessBuilder. ^java.util.List [(.getPath binary) sock]))]
    (assert (until #(.exists (io/file sock)) 3000) "holder did not start")
    p))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- result [r] (let [x (:result r)] (if (contains? x :result) (:result x) x)))

(defn- open! [root]
  (with-redefs [core/shell-command (fn [] ["/bin/zsh" "-f"])]
    (let [r (act! root "pane.open" {:kind "terminal" :target "shell.local" :cols 80 :rows 24})]
      (or (:pane (result r)) (throw (ex-info "no pane" {:r r}))))))

(defn- type! [root p text]
  (let [r (act! root "pane.send" {:pane (str p) :text text})]
    (assert (:ok r) (pr-str r))
    r))

(defn- screen-text [root p] (:text (result (act! root "pane.read" {:pane (str p)}))))

(defn- output-of
  "Everything on the record of this terminal's output."
  [world p]
  (let [events (filter #(= :pane-output (:event/kind %)) (log/read-stream (:log world) p 0 Long/MAX_VALUE))]
    (is (= (range 1 (inc (count events))) (map (comp :n :event/body) events)) "the chain has no hole")
    (let [out (java.io.ByteArrayOutputStream.)]
      (doseq [e events] (.write out ^bytes (blob/get (:blobs world) (:blob (:event/body e)))))
      (String. (.toByteArray out) "UTF-8"))))

(defmacro ^:private with-holder [[dir h] & body]
  `(if-not (.exists binary)
     (println "no holder binary: run `make holder`")
     (let [~dir (fx/temp-dir)
           ~h (start-holder ~dir)]
       (try ~@body
            (finally (.destroyForcibly ^Process ~h) (fx/delete-tree! ~dir))))))

(deftest a-program-runs-through-a-world-restart-and-no-output-is-lost
  (with-holder [dir h]
    (let [world (fx/start-world dir 0)
          root (fx/attach-root world)
          p (open! root)]
      ;; about two seconds of numbered lines, and the world goes down in the middle of them
      (type! root p "i=0; while [ $i -lt 4000 ]; do echo row-$i; i=$((i+1)); sleep 0.0005; done; echo all-done\n")
      (is (until #(str/includes? (str (screen-text root p)) "row-") 3000))
      (ws/close! root)
      (system/stop world)
      (Thread/sleep 300)
      (let [world2 (fx/start-world dir 0)]
        (try
          (let [root2 (fx/attach-root world2)]
            (testing "it is still running"
              (is (= :open (:state (result (act! root2 "pane.read" {:pane (str p)}))))))
            (is (until #(str/includes? (str (screen-text root2 p)) "\nall-done") 20000)
                (str (screen-text root2 p)))
            (testing "input reaches it, and its screen comes back"
              (type! root2 p "echo typed-after\n")
              (is (until #(str/includes? (str (screen-text root2 p)) "\ntyped-after") 5000)))
            (testing "scrollback reaches back past the restart"
              (let [lines (:lines (result (act! root2 "pane.read" {:pane (str p) :what "scrollback" :range [1 5000]})))]
                (is (some #(= "row-0" %) lines) (pr-str (take 5 lines)))))
            (core/sweep! world2)
            (Thread/sleep 50)
            (core/stop-all! world2)
            (let [rows (filter #(re-matches #"row-\d+" %) (str/split (output-of world2 p) #"\r?\n"))]
              (is (= 4000 (count rows)) (pr-str (take 3 (remove (set rows) (map #(str "row-" %) (range 4000))))))
              (is (= (map #(str "row-" %) (range 4000)) rows) "every line once, in order"))
            (ws/close! root2))
          (finally (system/stop world2)))))))

(deftest a-terminal-carries-on-through-a-prune-that-took-its-record
  ;; a quiet terminal's last pane-output can fall below the log's first seq: the holder must not be
  ;; asked for everything again, and the chain must go on from where it was
  (with-holder [dir h]
    (with-redefs [seg/roll-bytes 4096]
      (let [config (assoc (fx/config dir 0) :retention {:grace-ms 0 :low-water 1.0})
            world (system/start config)
            wid (:world-id world)
            root (fx/attach-root world)
            p (open! root)
            outputs (fn [w] (filterv #(= :pane-output (:event/kind %)) (log/read-stream (:log w) p 0 Long/MAX_VALUE)))
            fill! (fn [] (dotimes [i 8]
                           (log/append!! (:log world) (log/pending :field-set wid wid {:name (keyword "test" (str "f" i)) :type :string
                                                                                       :value (apply str (repeat 3000 "x")) :reader :both}))))]
        (type! root p "echo before-prune\n")
        (is (until #(str/includes? (str (screen-text root p)) "\nbefore-prune") 5000))
        (is (until #(do (with-redefs [core/idle-flush-ms 0] (core/sweep! world)) (seq (outputs world))) 5000))
        (let [{:keys [n chain offset]} (:event/body (peek (outputs world)))]
          (ws/close! root)
          (fill!)
          (snap/write! (:dir (:index world)) (index/applied-seq (:index world)) (index/db (:index world)))
          (fill!)
          (retention/sweep! (assoc-in world [:config :retention-gb] 1.0e-9))
          (is (empty? (outputs world)) "its output events have gone")
          (is (= offset (:offset (core/field (act/db world) p core/record-field))))
          (system/stop world)
          (let [world2 (system/start config)]
            (try
              (let [root2 (fx/attach-root world2)]
                (type! root2 p "echo after-prune\n")
                (is (until #(str/includes? (str (screen-text root2 p)) "\nafter-prune") 5000))
                (core/stop-all! world2)
                (let [first-new (:event/body (first (outputs world2)))
                      text (String. ^bytes (blob/get (:blobs world2) (:blob first-new)) "UTF-8")]
                  (is (= (inc n) (:n first-new)) "the count goes on")
                  (is (= (:chain first-new) (blob/sha256-hex (.getBytes (str chain (:blob first-new)) "UTF-8"))) "so does the chain")
                  (is (>= (:offset first-new) offset))
                  (is (not (str/includes? text "before-prune\r\n")) "nothing it already recorded comes again"))
                (ws/close! root2))
              (finally (system/stop world2)))))))))

(deftest a-terminal-that-exits-while-the-world-is-down-is-told-to-the-next
  (with-holder [dir h]
    (let [world (fx/start-world dir 0)
          root (fx/attach-root world)
          p (open! root)]
      (type! root p "echo last-words; sleep 0.4; exit 3\n")
      (is (until #(str/includes? (str (screen-text root p)) "\nlast-words") 3000))
      (ws/close! root)
      (system/stop world)
      (Thread/sleep 900)
      (let [world2 (fx/start-world dir 0)]
        (try
          (let [root2 (fx/attach-root world2)]
            (is (until #(= :exited (:state (result (act! root2 "pane.read" {:pane (str p)})))) 5000))
            (let [r (result (act! root2 "pane.read" {:pane (str p)}))]
              (is (= 3 (:exit r)))
              (is (str/includes? (str (:text r)) "last-words")))
            (ws/close! root2))
          (finally (system/stop world2)))))))

(deftest losing-the-holder-ends-terminals-with-what-they-showed
  (with-holder [dir h]
    (let [world (fx/start-world dir 0)]
      (try
        (let [root (fx/attach-root world)
              p (open! root)]
          (type! root p "echo before-the-crash\n")
          (is (until #(str/includes? (str (screen-text root p)) "\nbefore-the-crash") 3000))
          (.destroyForcibly ^Process h)
          (is (until #(= :exited (:state (result (act! root "pane.read" {:pane (str p)})))) 5000))
          (let [r (result (act! root "pane.read" {:pane (str p)}))]
            (is (str/includes? (str (:text r)) "before-the-crash"))
            (is (nil? (:exit r))))
          (testing "the reason is on the record"
            (is (some #(= :holder-lost (:reason (:event/body %)))
                      (filter #(= :pane-closed (:event/kind %)) (log/read-stream (:log world) p 0 Long/MAX_VALUE)))))
          (ws/close! root))
        (finally (system/stop world))))))

(deftest a-world-without-a-holder-still-has-terminals
  (let [dir (fx/temp-dir)
        world (fx/start-world dir 0)]
    (try
      (let [root (fx/attach-root world)
            p (open! root)]
        (type! root p "echo no-holder-here\n")
        (is (until #(str/includes? (str (screen-text root p)) "\nno-holder-here") 3000))
        (ws/close! root))
      (finally (system/stop world) (fx/delete-tree! dir)))))
