(ns knowledge.world.pane.core-test
  "Panes end to end on a real pseudo-terminal: a shell's screen is exact, clients come and go and the
   process stays, deltas follow a first full screen, a dropped client resumes, a model's input is
   gated, and output is hash-chained in blobs."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing use-fixtures]]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.rules :as rules]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.loop.ops :as loop-ops]
            [knowledge.world.log.core :as log]
            [knowledge.world.pane.core :as core]
            [knowledge.world.pane.files :as files]
            [knowledge.world.pane.pty :as pty]
            [knowledge.world.pane.render :as render]
            [knowledge.world.pane.screen :as scr]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]))

(use-fixtures :each (fn [t] (with-redefs [core/shell-command (fn [] ["/bin/zsh" "-f"])] (t))))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- result [r] (let [x (:result r)] (if (contains? x :result) (:result x) x)))
(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))

(defn- until
  "Poll f until truthy, or nil after ms."
  ([f] (until f 8000))
  ([f ms]
   (let [deadline (+ (System/currentTimeMillis) ms)]
     (loop []
       (or (f)
           (when (< (System/currentTimeMillis) deadline)
             (Thread/sleep 25)
             (recur)))))))

(defn- open! [c] (:pane (result (act! c "pane.open" {:kind "terminal" :target "shell.local" :cols 80 :rows 24}))))
(defn- screen [c pane] (:text (result (act! c "pane.read" {:pane (str pane)}))))
(defn- shows? [c pane s] (until #(str/includes? (screen c pane) s)))
(defn- type! [c pane text] (act! c "pane.send" {:pane (str pane) :text text}))
(defn- subscribe! [c pane & [args]] (act! c "pane.subscribe" (merge {:pane pane} args)))
(defn- frame [c pane & [pred]] (ws/await-frame c #(and (= pane (:pane %)) ((or pred identity) %)) 8000))
(defn- close! [c pane] (act! c "pane.close" {:pane (str pane)}))

(defn- client-copy
  "A client's screen: frames applied as a thin client applies them, until pred of it holds."
  [c pane start pred]
  (loop [s start n 0]
    (if (or (pred s) (> n 200))
      s
      (if-let [f (ws/await-frame c #(= pane (:pane %)) 3000)]
        (recur (scr/feed (if (:full f) (scr/create (:cols f) (:rows f)) s) (or (:data f) "")) (inc n))
        s))))

(deftest a-shell-shows-what-it-printed
  (fx/with-world [{:keys [root]}]
    (let [p (open! root)]
      (type! root p "echo hi\n")
      (is (shows? root p "\nhi"))
      (is (re-find #"(?m)^hi$" (screen root p)))
      (close! root p))))

(deftest a-late-client-gets-the-whole-screen-and-then-only-deltas
  (fx/with-world [{:keys [root world]}]
    (let [p (open! root)
          late (fx/attach-root world)]
      (type! root p "echo first\n")
      (shows? root p "\nfirst")
      (testing "the first frame is a full screen, exact"
        (subscribe! late p)
        (let [f (frame late p)
              s (scr/feed (scr/create (:cols f) (:rows f)) (:data f))]
          (is (:full f))
          (is (= [80 24] [(:cols f) (:rows f)]))
          (is (= (screen root p) (scr/text s)))
          (testing "after it, deltas: each names the seq it applies on and is far smaller than a screen"
            (type! root p "echo second\n")
            (let [a (client-copy late p s #(str/includes? (scr/text %) "\nsecond"))]
              (is (= (screen root p) (scr/text a)))
              (type! root p "echo third\n")
              (let [d (frame late p)]
                (is (not (:full d)))
                (is (= (inc (long (:base d))) (:seq d)))
                (is (< (count (:data d)) (/ (count (scr/paint a)) 4))))))))
      (close! root p))))

(deftest two-sessions-see-the-same
  (fx/with-world [{:keys [root world]}]
    (let [p (open! root)
          a (fx/attach-root world)
          b (fx/attach-root world)]
      (subscribe! a p)
      (subscribe! b p)
      (let [fa (frame a p) fb (frame b p)
            sa (scr/feed (scr/create 80 24) (:data fa))
            sb (scr/feed (scr/create 80 24) (:data fb))]
        (type! root p "echo both\n")
        (let [done? #(str/includes? (scr/text %) "\nboth")
              ca (client-copy a p sa done?)
              cb (client-copy b p sb done?)]
          (is (= (scr/text ca) (scr/text cb) ))
          (is (done? ca))))
      (close! root p))))

(deftest a-dropped-socket-resumes-from-its-last-seq
  (fx/with-world [{:keys [root world]}]
    (let [p (open! root)
          phone (fx/attach-root world)]
      (subscribe! phone p)
      (let [f (frame phone p)
            s (scr/feed (scr/create 80 24) (:data f))]
        (type! root p "echo one\n")
        (shows? root p "\none")
        (let [held (client-copy phone p s #(str/includes? (scr/text %) "\none"))
              last-seq (:seq (result (subscribe! phone p)))
              _ (frame phone p)
              held (client-copy phone p held (constantly true))]
          (ws/abort! phone)
          (type! root p "echo missed\n")
          (shows? root p "\nmissed")
          (let [back (fx/attach-root world)]
            (testing "what it held is still in the ring: a delta, not a screen"
              (subscribe! back p {:after last-seq})
              (let [d (frame back p)]
                (is (not (:full d)) (pr-str d))
                (is (= last-seq (:base d)))
                (is (< (count (:data d)) 200))))
            (testing "what fell out of the ring: a full screen"
              (let [c (fx/attach-root world)]
                (subscribe! c p {:after 99999})
                (is (:full (frame c p)))))
            (testing "the socket that left took nothing with it"
              (is (str/includes? (screen back p) "missed")))
            held)))
      (close! root p))))

(deftest an-image-comes-in-chunks
  (fx/with-world [{:keys [world root]}]
    (let [dir (io/file (:data-dir world) "pics")
          png (byte-array (map unchecked-byte (concat [0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A] (range 200))))]
      (.mkdirs dir)
      (io/copy png (io/file dir "a.png"))
      (let [m (act! root "workspace.mount" {:principal "root" :path (.getPath dir) :name "pics" :shell false})
            _ (is (:ok m) (pr-str m))
            open (act! root "pane.open" {:kind "files" :target "pics.fs"})
            files (:pane (result open))
            opened (result (act! root "pane.frob" {:pane (str files) :verb "open" :args {:name "a.png"}}))
            pane (or (:pane opened) (get-in opened [:view :data :pane]))
            part (fn [o] (result (act! root "pane.read" {:pane (str pane) :what "bytes" :offset o :limit 100})))
            a (part 0)
            b (part 100)
            bytes-of #(.decode (java.util.Base64/getDecoder) ^String (:base64 %))]
        (is (= 208 (:size a)))
        (is (true? (:more a)))
        (is (= 100 (alength ^bytes (bytes-of a))))
        (is (= 100 (:offset b)))
        (is (java.util.Arrays/equals ^bytes png
                                     ^bytes (byte-array (concat (bytes-of a) (bytes-of b) (bytes-of (part 200))))))
        (is (false? (:more (part 200))))
        (is (= "image/png" (:mime a)))
        (testing "a pane with nothing binary says so"
          (is (= :invalid-args (get-in (act! root "pane.read" {:pane (str files) :what "bytes"}) [:err :code]))))
        (testing "only a pane's own Fields"
          (is (some? (:err (act! root "pane.read" {:pane (str pane) :what "bytes" :field "principal/token-hash"})))))))))

(deftest a-listing-says-when-a-file-changed
  (let [dir (doto (java.io.File/createTempFile "orbis" "") (.delete) (.mkdirs))]
    (spit (io/file dir "a.txt") "x")
    (is (re-matches #"\d{4}-\d{2}-\d{2}T.*Z" (:modified (first (:entries (files/listing dir))))))))

(deftest who-is-attached-is-a-field
  (fx/with-world [{:keys [world root witness]}]
    (let [pane (open! root)
          me (str (:principal root))
          attached (fn [] (get-in (objects/fields (act/db world) pane) [:pane/attached :field/value]))]
      (fx/grant! root (:id witness) #{pane} #{:read})
      (subscribe! root pane)
      (is (until #(= [me] (attached))))
      (let [w (fx/attach-as world (:token witness))]
        (subscribe! w pane)
        (is (until #(= (sort [me (str (:id witness))]) (attached))) "a second client shows")
        (act! w "pane.unsubscribe" {:pane pane})
        (is (until #(= [me] (attached))))
        (subscribe! w pane)
        (is (until #(= 2 (count (attached)))))
        (ws/close! w)
        (is (until #(= [me] (attached))) "a closed socket goes too"))
      (close! root pane))))

(deftest a-slow-client-names-its-rate
  (fx/with-world [{:keys [root world]}]
    (let [p (open! root)
          phone (fx/attach-root world)]
      (subscribe! phone p {:rate-ms 400})
      (frame phone p)
      (type! root p "for i in 1 2 3 4 5 6 7 8; do echo tick$i; sleep 0.05; done\n")
      (shows? root p "tick8")
      (let [frames (loop [acc []]
                     (if-let [f (ws/await-frame phone #(= p (:pane %)) 900)] (recur (conj acc f)) acc))]
        (is (<= 1 (count frames) 5) (str (count frames) " frames in place of dozens")))
      (close! root p))))

(deftest closing-a-session-leaves-the-process
  (fx/with-world [{:keys [root world]}]
    (let [c (fx/attach-root world)
          p (open! c)]
      (type! c p "marker=alive\n")
      (subscribe! c p)
      (ws/close! c)
      (Thread/sleep 100)
      (is (pty/alive? (:pty (core/terminal-live world p))))
      (type! root p "echo $marker\n")
      (is (shows? root p "\nalive"))
      (is (some #(= p (:pane %)) (:panes (result (act! root "pane.list" {})))))
      (close! root p)
      (is (nil? (core/terminal-live world p))))))

(deftest grants-apply-to-everyone
  (fx/with-world [{:keys [root world witness]}]
    (let [w (fx/attach-as world (:token witness))
          p (open! root)
          act-id (fn [n] (resolve-as root (str "action/" n) :read))
          code (fn [r] (get-in r [:err :code]))]
      (testing "no right to the action"
        (is (= :unresolved (code (act! w "pane.open" {:kind "terminal" :target "shell.local"})))))
      (testing "the action, but no right to the Adapter or the pane"
        (doseq [n ["pane.open" "pane.read" "pane.send" "pane.list" "pane.close"]]
          (fx/grant! root (:id witness) #{(act-id n)} #{:read :evaluate}))
        (is (= :unresolved (code (act! w "pane.open" {:kind "terminal" :target "shell.local"}))))
        (is (= :unresolved (code (act! w "pane.read" {:pane (str p)}))))
        (is (= :unresolved (code (act! w "pane.send" {:pane (str p) :text "x"}))))
        (is (= :unresolved (code (act! w "pane.close" {:pane (str p)}))))
        (is (= :unresolved (code (subscribe! w p))))
        (is (empty? (:panes (result (act! w "pane.list" {}))))))
      (testing "read on the shell lets it watch, not type"
        (fx/grant! root (:id witness) #{(resolve-as root "shell.local" :read)} #{:read})
        (type! root p "echo seen\n")
        (is (shows? root p "\nseen"))
        (is (str/includes? (:text (result (act! w "pane.read" {:pane (str p)}))) "seen"))
        (is (= :unresolved (code (act! w "pane.send" {:pane (str p) :text "x"})))))
      (close! root p))))

(deftest a-terminal-holds-nothing-of-the-world-s-secrets
  (let [env (pty/clean-env)]
    (is (= "xterm-256color" (get env "TERM")))
    (is (empty? (filter #(or (str/starts-with? % "KE_") (str/includes? % "KEY") (str/includes? % "TOKEN"))
                        (keys env))))))

(deftest a-full-screen-program-parses
  (fx/with-world [{:keys [root world]}]
    (let [p (open! root)
          c (fx/attach-root world)
          read-it #(result (act! root "pane.read" {:pane (str p)}))]
      (type! root p "echo before\n")
      (shows? root p "\nbefore")
      (subscribe! c p)
      (frame c p)
      (type! root p "printf '\\033[?1049h\\033[2J\\033[1;1Htop line\\033[12;30H\\033[7mcentre\\033[0m\\033[24;1H-- INSERT --\\033[3;4H'; sleep 1; printf '\\033[?1049l'\n")
      (is (until #(:alternate (read-it))))
      (testing "the screen is the program's, the cursor where it left it"
        (let [r (read-it)
              lines (str/split-lines (:text r))]
          (is (= "top line" (first lines)))
          (is (= (str (apply str (repeat 29 " ")) "centre") (nth lines 11)))
          (is (= "-- INSERT --" (nth lines 23)))
          (is (= {:col 3 :row 2 :visible true} (:cursor r)))))
      (testing "leaving restores the shell's screen; a client got it as a whole repaint"
        (is (until #(and (not (:alternate (read-it))) (str/includes? (:text (read-it)) "before"))))
        (is (not (re-find #"(?m)^-- INSERT" (:text (read-it)))))
        (is (frame c p :full)))
      (close! root p))))

(deftest exit-is-reported
  (fx/with-world [{:keys [root world]}]
    (let [p (open! root)
          c (fx/attach-root world)]
      (subscribe! c p)
      (frame c p)
      (type! root p "exit 3\n")
      (let [f (frame c p :exited)]
        (is (= 3 (:exit f))))
      (let [r (result (act! root "pane.read" {:pane (str p)}))]
        (is (= :exited (:state r)))
        (is (= 3 (:exit r))))
      (is (= :exit (:reason (:event/body (first (filter #(= :pane-closed (:event/kind %))
                                                        (log/read-stream (:log world) p 0 100)))))))
      (testing "input to it is refused"
        (is (= :invalid-args (get-in (type! root p "x") [:err :code])))))))

(deftest output-goes-to-hash-chained-blobs-and-survives-a-restart
  (let [dir (fx/temp-dir)
        world (fx/start-world dir 0)]
    (try
      (with-redefs [core/shell-command (fn [] ["/bin/zsh" "-f"])]
        (let [root (fx/attach-root world)
              p (open! root)]
          (type! root p "seq 1 30000; echo the-end-$((40+2))\n")
          ;; the sum: the typed line on the screen also says the-end
          (is (until #(str/includes? (screen root p) "the-end-42") 20000))
          (close! root p)
          ;; the last chunk lands when the program is gone, a moment after the close answers
          (is (until #(some (fn [e] (= :pane-closed (:event/kind e))) (log/read-stream (:log world) p 0 1000)) 8000))
          (let [events (log/read-stream (:log world) p 0 1000)
                outs (map :event/body (filter #(= :pane-output (:event/kind %)) events))
                bytes-of (fn [{:keys [blob]}] (blob/get (:blobs world) blob))
                text (apply str (map #(String. ^bytes (bytes-of %) "UTF-8") outs))]
            (is (>= (count outs) 2) "more than 64 KiB of output is more than one event")
            (is (= (range 1 (inc (count outs))) (map :n outs)))
            (testing "each link is sha256(previous link || blob hash), and each blob is its hash"
              (reduce (fn [prev {:keys [blob chain size] :as o}]
                        (is (= blob (blob/sha256-hex (bytes-of o))))
                        (is (= size (alength ^bytes (bytes-of o))))
                        (is (= chain (blob/sha256-hex (.getBytes (str prev blob) "UTF-8"))))
                        chain)
                      "" outs))
            (is (str/includes? text "29999"))
            (is (some #(= :pane-input (:event/kind %)) events) "the input is on the record")
            (is (not-any? #(= :pane-output (:event/kind %))
                          (filter #(> (count (pr-str (:event/body %))) 1000) events))
                "output never rides on the log"))
          (ws/close! root)))
      (finally (system/stop world) (fx/delete-tree! dir)))))

(deftest a-world-restart-reads-exited-with-its-scrollback-kept
  (let [dir (fx/temp-dir)
        world (fx/start-world dir 0)
        root (fx/attach-root world)
        p (with-redefs [core/shell-command (fn [] ["/bin/zsh" "-f"])] (open! root))]
    (type! root p "echo kept-through-restart\n")
    (shows? root p "\nkept-through-restart")
    (ws/close! root)
    (system/stop world)
    (let [world2 (fx/start-world dir 0)]
      (try
        (let [root2 (fx/attach-root world2)
              r (result (act! root2 "pane.read" {:pane (str p)}))]
          (is (= :exited (:state r)))
          (is (str/includes? (:text r) "kept-through-restart"))
          (testing "a client attaches to it as to any screen, and hears it has exited"
            (subscribe! root2 p)
            (let [f (frame root2 p)]
              (is (:full f))
              (is (:exited f))
              (is (str/includes? (:data f) "kept-through-restart"))))
          (ws/close! root2))
        (finally (system/stop world2) (fx/delete-tree! dir))))))

;; ---------------------------------------------------------------- a model at a terminal

(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))
(defn- send-line [pane text]
  (str "{\"op\": \"act\", \"args\": {\"action\": \"pane.send\", \"args\": {\"pane\": \"" pane "\", \"text\": \"" text "\"}}}"))

(defn- talk!
  "witness, a person, asks root through the stub Model, which runs the ops in the ask."
  [root world witness]
  (let [route (resolve-as root "root.route" :read)
        w (fx/attach-as world (:token witness))]
    (fx/grant! root (:id witness) #{(:id witness)} #{:read})
    (fx/grant! root (:id witness) #{route} #{:transmit})
    {:w w :ask (fn [text] (ws/request!! w :send {:to #{(:principal root)} :route route :text text}))}))

(defn- pending-tasks [world summary-part]
  (let [db (act/db world)]
    (filter #(str/includes? (str (core/field db % :task/summary)) summary-part)
            (objects/objects-of-class db :task))))

(deftest a-model-s-input-passes-the-gate
  (fx/with-world [{:keys [root world witness]}]
    (let [p (open! root)
          {:keys [ask]} (talk! root world witness)]
      (ask (str "go\n" (ops-block (send-line p "echo from-model\\n"))))
      (let [task (until #(first (pending-tasks world "from-model")) 15000)]
        (is (some? task) "the send became a Task awaiting a person")
        (is (str/includes? (str (core/field (act/db world) task :task/summary)) "type `echo from-model"))
        (is (not (str/includes? (screen root p) "\nfrom-model")) "nothing reached the shell")
        (testing "approved, it runs, and the record keeps the model's text and who sent it"
          (let [w (fx/attach-as world (:token witness))]
            (is (:ok (act! w "task.approve" {:task (str task)})))
            (is (shows? root p "\nfrom-model"))
            (let [input (first (filter #(and (= :pane-input (:event/kind %)) (get-in % [:event/body :text]))
                                       (log/read-stream (:log world) p 0 100)))]
              (is (= "echo from-model\n" (get-in input [:event/body :text])))
              (is (= "loop" (get-in input [:event/body :from])))
              (is (= (:principal root) (get-in input [:event/body :who]))))
            (testing "the model is told what came of it: the screen, once output went quiet"
              (is (until #(str/includes? (str (core/field (act/db world) task :task/result)) "\\nfrom-model")
                         15000)
                  (pr-str (core/field (act/db world) task :task/result)))))))
      (close! root p))))

(deftest a-pane-named-as-the-object-is-the-same-op
  (let [p (random-uuid)
        form (fn [m] (gates/pane-form :act m))]
    (is (= {:action :pane.send :args {:text "x" :pane (str p)}}
           (form {:object p :action :pane.send :args {:text "x"}}))
        "so the gate sees a named action and a model cannot walk around it")
    (is (= {:action :pane.open :args {:kind "terminal" :target (str p)}}
           (form {:object p :action :pane.open :args {:kind "terminal"}})))
    (is (= {:object p :action :run :args {}} (form {:object p :action :run :args {}})))))

(deftest a-terminal-takes-its-shell-as-target-or-adapter
  (fx/with-world [{:keys [root]}]
    (let [p (:pane (result (act! root "pane.open" {:kind "terminal" :adapter "shell.local"})))]
      (is (uuid? p))
      (close! root p))
    (is (= :invalid-args (get-in (act! root "pane.open" {:kind "terminal"}) [:err :code])))))

(deftest a-send-that-cannot-arrive-gets-no-card
  (fx/with-world [{:keys [root world]}]
    (let [shell (resolve-as root "shell.local" :read)
          r (actions/preflight world (act/db world) (:principal root)
                               {:action :pane.send :args {:pane (str shell) :text "ls\n"}})]
      (is (= :invalid-args (get-in r [:err :code])))
      (is (str/includes? (get-in r [:err :message]) "pane.open answered")))))

(deftest read-send-read-again-is-not-a-repeat
  (let [p (str (random-uuid))
        read-op {:action :pane.read :args {:pane p}}
        send-op {:action :pane.send :args {:pane p :text "ls\n"}}
        done [{:op "act" :args read-op :result {:text "old"}}
              {:op "act" :args send-op :result {:sent 3}}]]
    (is (nil? (loop-ops/repeat-of done :act read-op)) "a screen read again is read again")
    (is (nil? (loop-ops/repeat-of done :act send-op)) "the same line twice is the model's to mean")
    (testing "what a person approved does not ask them again"
      (is (some? (loop-ops/repeat-of [{:op "act" :args send-op :result {:approved true}}] :act send-op)))
      (is (nil? (loop-ops/repeat-of [{:op "act" :args send-op :result {:approved true}}] :act
                                    {:action :pane.send :args {:pane p :text "pwd\n"}}))))
    (testing "opening is still making"
      (let [open-op {:action :pane.open :args {:kind "terminal" :target "shell.local"}}]
        (is (some? (loop-ops/repeat-of [{:op "act" :args open-op :result {:pane p}}] :act open-op)))))))

(deftest a-standing-rule-admits-a-whole-line-and-nothing-else
  (fx/with-world [{:keys [root world]}]
    (let [p (open! root)
          db (act/db world)
          id (:principal root)
          args (fn [m] {:action "pane.send" :args (assoc m :pane (str p))})
          run (fn [m] (rules/run-of (act/db world) id :act (args m)))]
      (is (= "shell" (gates/gated? db id :act (args {:text "ls\n"}))))
      (is (= "ls -la" (:cmd (run {:text "ls -la\n"}))))
      (testing "a partial line, a multi-line paste or a raw key has nothing to match"
        (is (nil? (run {:text "ls"})))
        (is (nil? (run {:text "ls\nrm -rf x\n"})))
        (is (nil? (run {:text "ls\u0003\n"})))
        (is (nil? (run {:keys ["enter"]}))))
      (testing "once anyone has typed freely, the world cannot vouch for the prompt"
        (type! root p "cd /tmp\n")
        (is (until #(true? (core/field (act/db world) p :pane/dirty))))
        (is (nil? (run {:text "ls\n"}))))
      (testing "a frob that writes is gated, one that reads is not"
        (let [frob (fn [verb] {:action "pane.frob" :args {:pane (str p) :verb verb}})]
          (is (= "fs:write" (gates/gated? db id :act (frob "save"))))
          (is (= "fs:write" (gates/gated? db id :act (frob "delete"))))
          (is (nil? (gates/gated? db id :act (frob "goto"))))
          (is (nil? (gates/gated? db id :act {:action "pane.read" :args {:pane (str p)}})))
          (is (nil? (gates/gated? db id :act {:action "file.open" :args {:path "x"}})))))
      (close! root p))))

;; ---------------------------------------------------------------- files and editors

(defn- mount!
  "A temp directory mounted for root as name."
  [root world name & [read-only]]
  (let [dir (.toFile (java.nio.file.Files/createTempDirectory "pane-test" (into-array java.nio.file.attribute.FileAttribute [])))
        r (act! root "workspace.mount" (cond-> {:principal "root" :path (.getCanonicalPath dir) :name name}
                                         read-only (assoc :read-only true)))]
    (is (:ok r) (pr-str r))
    (.getCanonicalFile dir)))

(deftest a-file-pane-works-through-the-mount
  (fx/with-world [{:keys [root world]}]
    (let [dir (mount! root world "box")
          _ (spit (io/file dir "a.txt") "alpha")
          _ (.mkdirs (io/file dir "sub"))
          p (:pane (result (act! root "pane.open" {:kind "files" :target "box.fs"})))
          frob (fn [verb args] (act! root "pane.frob" {:pane (str p) :verb verb :args args}))
          listing #(result (act! root "pane.read" {:pane (str p)}))]
      (is (= ["sub" "a.txt"] (map :name (:entries (listing)))))
      (is (:ok (frob "mkdir" {:name "made"})))
      (is (.isDirectory (io/file dir "made")))
      (is (:ok (frob "copy" {:from "a.txt" :to "b.txt"})))
      (is (= "alpha" (slurp (io/file dir "b.txt"))))
      (is (:ok (frob "rename" {:from "b.txt" :to "c.txt"})))
      (is (.exists (io/file dir "c.txt")))
      (is (:ok (frob "move" {:from "c.txt" :to "sub/c.txt"})))
      (is (= :conflict (get-in (frob "copy" {:from "a.txt" :to "sub/c.txt"}) [:err :code])))
      (is (= :outside-mount (get-in (frob "cd" {:path "../.."}) [:err :code])))
      (is (:ok (frob "cd" {:path "sub"})))
      (is (= "sub" (:cwd (listing))))
      (is (= ["c.txt"] (map :name (:entries (listing)))))
      (is (:ok (frob "select" {:names ["c.txt"]})))
      (is (= ["c.txt"] (:selection (listing))))
      (is (:ok (frob "delete" {:name "c.txt"})))
      (is (not (.exists (io/file dir "sub/c.txt"))))
      (testing "open makes a pane of the kind the file is"
        (is (:ok (frob "cd" {:path "/"})))
        (let [r (result (frob "open" {:name "a.txt"}))]
          (is (= :editor (:kind r)))
          (is (= "alpha" (:text (result (act! root "pane.read" {:pane (str (:pane r))}))))))))))

(deftest a-read-only-mount-refuses-what-would-change-it
  (fx/with-world [{:keys [root world]}]
    (let [dir (mount! root world "ro" true)
          _ (spit (io/file dir "a.txt") "alpha")
          p (:pane (result (act! root "pane.open" {:kind "files" :target "ro.fs"})))
          e (:pane (result (act! root "file.open" {:name "ro.fs/a.txt"})))]
      (is (= :not-allowed (get-in (act! root "pane.frob" {:pane (str p) :verb "mkdir" :args {:name "x"}}) [:err :code])))
      (is (= :not-allowed (get-in (act! root "pane.frob" {:pane (str e) :verb "save"}) [:err :code]))))))

(deftest a-buffer-never-loses-anyone-s-edit-silently
  (fx/with-world [{:keys [root world witness]}]
    (let [dir (mount! root world "docs")
          file (io/file dir "note.txt")
          _ (spit file "one\ntwo\n")
          p (:pane (result (act! root "file.open" {:name "docs.fs/note.txt"})))
          frob (fn [c verb args] (act! c "pane.frob" {:pane (str p) :verb verb :args args}))
          buf #(result (act! root "pane.read" {:pane (str p)}))
          w (fx/attach-as world (:token witness))
          sub (fx/attach-root world)]
      (subscribe! sub p)
      (is (= "one\ntwo\n" (:data (frame sub p :full))))
      (fx/grant! root (:id witness) #{(resolve-as root "action/pane.frob" :read)} #{:read :evaluate})
      (fx/grant! root (:id witness) #{p} #{:read :write})
      (testing "each edit carries its author and reaches subscribers as a small change"
        (is (= 1 (:version (result (frob root "insert" {:line 2 :text "1.5\n"})))))
        (is (= "one\n1.5\ntwo\n" (:text (buf))))
        (let [e (:edit (frame sub p :edit))]
          (is (= {:at 4 :del 0 :text "1.5\n" :version 1} (dissoc e :who)))
          (is (= (:principal root) (:who e))))
        (is (= (:principal root) (get-in (last (filter #(= :pane-edit (:event/kind %))
                                                       (log/read-stream (:log world) p 0 100)))
                                         [:event/body :who]))))
      (testing "another's edit without the version, or with a stale one, is a conflict"
        (is (= :conflict (get-in (frob w "insert" {:line 1 :text "x"}) [:err :code])))
        (is (= :conflict (get-in (frob w "insert" {:line 1 :text "x" :version 0}) [:err :code])))
        (is (= "one\n1.5\ntwo\n" (:text (buf)))))
      (testing "with the version it read, it goes in and it holds the buffer"
        (is (= 2 (:version (result (frob w "replace" {:find "two" :with "TWO" :version 1})))))
        (is (= (:id witness) (:holder (buf))))
        (is (= :conflict (get-in (frob root "insert" {:line 1 :text "y"}) [:err :code]))))
      (testing "undo and redo, the same rule"
        (is (:ok (frob w "undo" {})))
        (is (= "one\n1.5\ntwo\n" (:text (buf))))
        (is (:ok (frob w "redo" {})))
        (is (= "one\n1.5\nTWO\n" (:text (buf)))))
      (testing "goto and find"
        (is (= "1.5" (:text (result (frob root "goto" {:line 2})))))
        (is (= [{:at 4 :line 2}] (:hits (result (frob root "find" {:text "1.5"}))))))
      (testing "save writes the file and frees the buffer; a change on disk stops it"
        (is (= "one\ntwo\n" (slurp file)) "nothing reaches the disk before save")
        (is (:ok (frob root "save" {})))
        (is (= "one\n1.5\nTWO\n" (slurp file)))
        (is (nil? (:holder (buf))))
        (is (:ok (frob root "insert" {:line 1 :text "z"})))
        (spit file "someone else\n")
        (is (= :conflict (get-in (frob root "save" {}) [:err :code])))
        (is (= "someone else\n" (slurp file)))
        (is (:ok (frob root "save" {:force true})))
        (is (= "zone\n1.5\nTWO\n" (slurp file))))
      (testing "a client that dropped gets the edits it missed"
        (let [c (fx/attach-root world)]
          (is (:ok (frob root "insert" {:line 1 :text "A"})))
          (let [before (:seq (result (subscribe! c p)))]
            (frame c p)
            (ws/abort! c)
            (is (:ok (frob root "insert" {:line 1 :text "B"})))
            (let [back (fx/attach-root world)]
              (subscribe! back p {:after before})
              (let [f (frame back p)]
                (is (not (:full f)))
                (is (= before (:base f)))
                (is (= "B" (get-in f [:edit :text])))))))))))

(deftest a-buffer-is-a-fold-of-its-log
  (let [dir (fx/temp-dir)
        world (fx/start-world dir 0)
        root (fx/attach-root world)
        d (mount! root world "docs")
        _ (spit (io/file d "n.txt") "base\n")
        p (:pane (result (act! root "file.open" {:name "docs.fs/n.txt"})))
        frob (fn [c verb args] (act! c "pane.frob" {:pane (str p) :verb verb :args args}))]
    (frob root "insert" {:line 1 :text "a"})
    (frob root "insert" {:line 1 :text "b"})
    (frob root "undo" {})
    (ws/close! root)
    (system/stop world)
    (let [world2 (fx/start-world dir 0)]
      (try
        (let [root2 (fx/attach-root world2)
              r (result (act! root2 "pane.read" {:pane (str p)}))]
          (is (= "abase\n" (:text r)))
          (is (= 3 (:version r)))
          (is (:ok (frob root2 "undo" {})) "undo survives too")
          (is (= "base\n" (:text (result (act! root2 "pane.read" {:pane (str p)})))))
          (ws/close! root2))
        (finally (system/stop world2) (fx/delete-tree! dir))))))

(deftest file-open-picks-the-kind-from-the-file
  (fx/with-world [{:keys [root world]}]
    (let [dir (mount! root world "kinds")
          png (byte-array [(unchecked-byte 0x89) 0x50 0x4e 0x47 0 0 0 0])]
      (spit (io/file dir "n.md") "# hi")
      (spit (io/file dir "p.tex") "\\documentclass{article}")
      (spit (io/file dir "c.clj") "(ns x)")
      (.mkdirs (io/file dir "d"))
      (with-open [o (io/output-stream (io/file dir "i.png"))] (.write o png))
      (doseq [[n kind] {"n.md" :markdown "p.tex" :latex "c.clj" :editor "i.png" :image "d" :files}]
        (is (= kind (:kind (result (act! root "file.open" {:name (str "kinds.fs/" n)})))) n))
      (is (= :editor (:kind (result (act! root "file.open" {:name "kinds.fs/n.md" :as "editor"})))))
      (testing "a host path inside a mount works; outside, it says so"
        (is (= :markdown (:kind (result (act! root "file.open" {:path (str dir "/n.md")})))))
        (is (= :outside-mount (get-in (act! root "file.open" {:path "/etc/hosts"}) [:err :code]))))
      (testing "an image pane holds its bytes as a blob a client reads"
        (let [p (:pane (result (act! root "file.open" {:name "kinds.fs/i.png"})))
              r (result (act! root "pane.read" {:pane (str p)}))]
          (is (= "image/png" (:mime r)))
          (is (= 8 (get-in r [:image :blob/size]))))))))

(deftest a-viewer-scrolls-and-renders
  (fx/with-world [{:keys [root world]}]
    (let [dir (mount! root world "tex")
          _ (spit (io/file dir "n.md") "# title\nbody\n")
          _ (spit (io/file dir "p.tex") "hello")
          md (:pane (result (act! root "file.open" {:name "tex.fs/n.md"})))
          tex (:pane (result (act! root "file.open" {:name "tex.fs/p.tex"})))
          frob (fn [p verb args] (act! root "pane.frob" {:pane (str p) :verb verb :args args}))]
      (is (= 2 (:scroll (result (frob md "scroll" {:line 2})))))
      (is (= "# title\nbody\n" (:text (result (frob md "render" {})))))
      (testing "no renderer: a clear err that says what to do"
        (with-redefs [render/renderer (constantly nil)]
          (let [r (frob tex "render" {})]
            (is (= :no-renderer (get-in r [:err :code])))
            (is (str/includes? (get-in r [:err :message]) "KE_TECTONIC")))))
      (testing "a renderer: the result is a view of what it made"
        (let [fake (io/file dir "fake-tectonic.sh")]
          (spit fake "#!/bin/sh\n# --untrusted --only-cached --outdir DIR --chatter minimal FILE\nprintf '%%PDF-fake' > \"$4/p.pdf\"\necho compiled\n")
          (.setExecutable fake true)
          (with-redefs [render/renderer (constantly (.getPath fake))
                        render/prime! (constantly true)]
            (let [r (result (frob tex "render" {}))]
              (is (str/includes? (:log r) "compiled"))
              (is (= "image" (get-in r [:view :kind])))
              (is (some? (get-in r [:view :data :bytes :blob/sha256]))))))))))

(deftest a-view-can-be-tex
  (fx/with-world [{:keys [root world witness]}]
    (let [route (resolve-as root "root.route" :read)
          r (ws/request!! root :present {:to #{(:id witness)} :route route :kind :tex :title "euler"
                                         :data {:source "$e^{i\\pi}+1=0$"}})]
      (is (:ok r) (pr-str r))
      (is (= :tex (:kind (result r))))
      (is (= :invalid-args (get-in (ws/request!! root :present {:to #{(:id witness)} :route route :kind :tex :title "x" :data {}})
                                   [:err :code])))
      world)))

(deftest json-clients-get-the-same-frames
  (fx/with-world [{:keys [root world]}]
    (let [p (open! root)
          c (fx/attach-root world :json)
          _ (ws/request!! c :act {:action "pane.subscribe" :args {:pane (str p)}})
          f (ws/await-frame c #(contains? % :pane) 5000)]
      (is (:full f))
      (is (string? (:data f)))
      (close! root p))))

(deftest a-path-under-a-shell-root-opens
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          shell (some #(when (= :host (get-in (objects/fields db %) [:adapter/binds :field/value])) %)
                      (objects/objects-of-class db :adapter))
          locator (get-in (objects/fields db shell) [:adapter/locator :field/value])
          dir (io/file locator "docs-for-a-click")]
      (.mkdirs dir)
      (try
        (is (= :files (keyword (:kind (result (act! root "file.open" {:path (.getCanonicalPath dir)}))))))
        (finally (.delete dir))))))

(deftest a-persons-terminal-is-theirs
  (fx/with-world [{:keys [world root witness]}]
    (let [w (fx/attach-as world (:token witness))
          shell "shell.local"
          _ (fx/grant! root (:id witness) #{(resolve-as root shell :read)} #{:read :evaluate})
          _ (fx/grant! root (:id witness) #{(resolve-as root "action/pane.open" :read)} #{:read :evaluate})
          r (act! w "pane.open" {:kind "terminal" :target shell})
          id (:pane (result r))]
      (is (:ok r) (pr-str r))
      (is (true? (get-in (objects/fields (act/db world) id) [:pane/person :field/value])) "a person's own, outside the box")
      (is (false? (:ok (act! root "pane.read" {:pane (str id)}))) "an agent cannot read it")
      (is (false? (:ok (act! root "pane.send" {:pane (str id) :text "x"}))) "or type into it"))))
