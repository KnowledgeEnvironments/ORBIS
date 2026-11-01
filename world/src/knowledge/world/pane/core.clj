(ns knowledge.world.pane.core
  "Panes (docs/panes.md): live surfaces that belong to the world. This namespace holds the registry,
   the Object a pane is, who may touch it, terminals and the stream to subscribed sessions. Every
   pane, file operation and render happens here, so a client needs no PTY, shell or local files.

   The stream is built for a phone: a first full screen, then deltas from a screen diff, coalesced at
   16 ms, each naming the seq it applies on (`base`), so a client sees a gap at once and a dropped
   socket resumes from its last seq. Output is not on the log: it goes to blobs with a `pane-output`
   event per 64 KiB, hash-chained, and a terminal's scrollback survives a restart through them.

   A terminal runs in one of two places. With a holder (pane.holder), a small process that outlives
   the world owns the PTY and its libghostty-vt terminal, and a restart loses nothing. Without one, a
   world started by hand runs it here through pty4j and pane.screen's own parser, and the terminal ends
   with the world. Everything above the PTY is the same for both."
  (:require [knowledge.world.adapters.confine :as confine]
            [knowledge.world.authority :as authority]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.index.core :as index]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.log.core :as log]
            [knowledge.world.pane.holder :as holder]
            [knowledge.world.pane.pty :as pty]
            [knowledge.world.pane.screen :as scr]
            [knowledge.world.protocol.codec :as codec]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.resolve.core :as resolve]
            [org.httpkit.server :as hk]
            [taoensso.timbre :as timbre])
  (:import [java.io ByteArrayOutputStream]
           [java.util Date]
           [java.util.concurrent ScheduledExecutorService TimeUnit]))

(def output-chunk "Bytes of output per pane-output event." (* 64 1024))
(def ring-cap "Screens or frames a resuming client can start from." 64)
(def idle-flush-ms 30000)
(def tick-ms 16)

(defn registry [] (atom {}))

(defn- now-ms ^long [] (System/currentTimeMillis))
(defn- ->uuid [v] (cond (uuid? v) v (string? v) (parse-uuid v) :else nil))

(defn field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn applied-seq [world] (index/applied-seq (:index world)))

(defn- shell-env [] (or (System/getenv "SHELL") "/bin/zsh"))

(defn shell-command
  "What a terminal runs: the host's login shell."
  []
  [(shell-env) "-l"])

;; ---------------------------------------------------------------- the Object

(defn- blank-live [id]
  {:id id :lock (Object.) :seq (atom 0) :ring (atom []) :subs (atom {}) :attached (atom [])})

(defn live
  "The in-memory half of a pane, made on first touch: after a restart a pane has only its log."
  [world id]
  (or (get @(:panes world) id)
      (get (swap! (:panes world) (fn [m] (if (contains? m id) m (assoc m id (blank-live id))))) id)))

(defn pane-id
  "A pane the world knows, from an id, or nil."
  [world v]
  (when-let [id (->uuid v)]
    (when (field (act/db world) id :pane/kind) id)))

(defn create!
  "A pane Object, the kernel's record of the open under the Grant that admitted it, as a Task is.
   fields are its pane/* Fields beyond the common ones. Returns {:pane id :seq n}."
  [world p kind target {:keys [title fields grant via blob size]}]
  (let [id (random-uuid)
        fields (cond-> (assoc fields :pane/kind kind :pane/state :open :pane/owner p)
                 target (assoc :pane/target target)
                 title (assoc :pane/title title)
                 size (assoc :pane/size size))]
    (act/append! world (log/pending :created id p {:class :object :fields fields} :delegation grant :via via))
    {:pane id
     :seq (act/append! world (log/pending :pane-opened id p
                                          (cond-> {:pane id :kind kind}
                                            target (assoc :target target)
                                            blob (assoc :blob blob))
                                          :streams #{p} :delegation grant :via via))}))

(defn set-field!
  "A pane Field as the kernel's bookkeeping: the pane's owner, under the Grant the pane was opened with."
  [world id k type v]
  (let [db (act/db world)
        owner (field db id :pane/owner)
        grant (:object/created-under (objects/entity db id))]
    (act/append! world (log/pending :field-set id owner {:name k :type type :value v :reader :both}
                                    :delegation grant))))

(defn- may-name? [db p id op] (some? (resolve/resolves? db p (act/id-name id) op)))

(defn party?
  "Is p who opened this pane, or the person it was delivered to?"
  [db p id]
  (or (= p (field db id :pane/owner)) (= p (field db id :pane/asker))))

(defn may-read?
  "The pane's Grants or its target's: :read on either, or being a party to it."
  [db p id]
  (and (not (authority/closed-pane? db id p))
       (or (party? db p id)
           (some? (authority/pane-share db id p (Date.)))
           (may-name? db p id :read)
           (when-let [t (field db id :pane/target)] (may-name? db p t :read)))))

(defn- typing-share?
  "Has p, an agent, been given a person's terminal to type into?"
  [db p id]
  (= "type" (some-> (authority/pane-share db id p (Date.)) :mode name)))

(defn may-drive?
  "A frob that stays in the world: a party, or :write on the pane. On a person's terminal an agent
   needs a typing share, whatever it holds."
  [db p id]
  (if (and (authority/person-terminal? db id) (not (authority/person? db p)))
    (typing-share? db p id)
    (or (party? db p id) (may-name? db p id :write))))

(defn may-evaluate?
  "Input to a terminal: :evaluate on its shell Adapter, whoever opened it. A person's terminal goes by
   its share alone: it is not the Adapter's shell an agent would be typing into."
  [db p id]
  (if (and (authority/person-terminal? db id) (not (authority/person? db p)))
    (typing-share? db p id)
    (when-let [t (field db id :pane/target)] (may-name? db p t :evaluate))))

;; ---------------------------------------------------------------- stream

(defn- send-frame!
  "Send one frame to a session; false when it has no open socket."
  [session frame]
  (let [{:keys [channel format]} @session]
    (if (and channel (hk/open? channel))
      (do (hk/send! channel (codec/encode format frame)) true)
      false)))

(defn- conj-ring [ring entry]
  (let [r (conj ring entry)]
    (if (> (count r) ring-cap) (subvec r (- (count r) ring-cap)) r)))

(declare publish! holder-conn! closed!)

(defn- schedule! [world ms f]
  (.schedule ^ScheduledExecutorService (:scheduler world)
             ^Runnable (fn [] (try (f) (catch Throwable t (timbre/warn t "pane push failed"))))
             (long ms) TimeUnit/MILLISECONDS))

(defn- kick!
  "A publish in at most ms, however much output arrives first: this is the coalescing."
  [world {:keys [scheduled] :as l} ms]
  (when (compare-and-set! scheduled false true)
    (schedule! world ms (fn [] (reset! scheduled false) (publish! world l)))))

(defn- full-frame [id seq screen]
  {:pane id :seq seq :full true :cols (:cols screen) :rows (:rows screen) :data (scr/paint screen)})

(defn- catch-up!
  "Bring one subscriber to the newest screen, at its own rate."
  [world {:keys [id subs ring] :as l} sid {:keys [session sent screen rate-ms at] :as _sub}]
  (let [{newest :seq now :screen} (peek @ring)
        wait (- (+ (long (or at 0)) (long (or rate-ms 0))) (now-ms))]
    (when (> (long newest) (long sent))
      (if (pos? wait)
        (kick! world l wait)
        (let [d (scr/diff screen now)
              frame (cond (nil? d) (full-frame id newest now)
                          (= "" d) nil
                          :else {:pane id :seq newest :base sent :data d})]
          (when frame
            (if (send-frame! session frame)
              (swap! subs update sid assoc :sent newest :screen now :at (now-ms))
              (swap! subs dissoc sid))))))))

(defn- settle!
  "The newest screen into the ring, under the pane's lock."
  [{:keys [dirty screen seq ring]}]
  (when (compare-and-set! dirty true false)
    (let [n (swap! seq inc)]
      (swap! ring conj-ring {:seq n :screen @screen}))))

(defn publish!
  "Push the newest screen to every subscriber."
  [world {:keys [lock subs] :as l}]
  (locking lock
    (settle! l)
    (doseq [[sid sub] @subs] (catch-up! world l sid sub))))

(defn push!
  "A frame of a pane that is no terminal, kept for a client that resumes. frame without its :seq."
  [world id frame]
  (let [{:keys [lock seq ring subs]} (live world id)]
    (locking lock
      (let [base @seq
            n (swap! seq inc)
            f (assoc frame :pane id :seq n :base base)]
        (swap! ring conj-ring {:seq n :frame f})
        (doseq [[sid {:keys [session]}] @subs]
          (when-not (send-frame! session f) (swap! subs dissoc sid)))))))

(defmulti snapshot
  "What a client needs to draw a pane that is no terminal: {:state map :data string?}."
  (fn [_world kind _id] kind))

(defmethod snapshot :default [world _ id]
  (let [db (act/db world)]
    {:state (into {} (for [[k {:keys [field/value]}] (objects/fields db id)
                           :when (and (= "pane" (namespace k)) (not (bytes? value)))]
                       [(name k) value]))}))

(defn- replay-screen
  "A terminal rebuilt from its pane-output blobs, for a world that has restarted."
  [world id]
  (let [db (act/db world)
        {:keys [cols rows]} (field db id :pane/size)
        events (log/read-stream (:log world) id 0 Long/MAX_VALUE)]
    (reduce (fn [s {:keys [event/kind event/body]}]
              (if (= :pane-output kind)
                (if-let [bs (blob/get (:blobs world) (:blob body))]
                  (scr/feed s (String. ^bytes bs "UTF-8"))
                  s)
                s))
            (scr/create (or cols 80) (or rows 24))
            events)))

(defn terminal-screen
  "The screen of a terminal, live or replayed once from its blobs."
  [world id]
  (let [{:keys [screen] :as l} (live world id)]
    (if screen
      @screen
      (let [s (replay-screen world id)]
        (swap! (:panes world) assoc-in [id :screen] (atom s))
        s))))

(defn- publish-attached!
  "pane/attached: the Principals subscribed now, written when that set changes. Who is watching a
   pane matters to whoever types in it."
  [world {:keys [id subs attached]}]
  (let [now (vec (sort (distinct (keep (fn [[_ {:keys [session]}]] (some-> (:principal @session) str)) @subs))))
        was (reset-vals! attached now)]
    (when (not= (first was) now)
      (set-field! world id :pane/attached [:list :string] now))))

(defn drop-session!
  "A socket closed: its subscriptions end, and every pane it watched says so."
  [world session]
  (let [sid (:id @session)]
    (doseq [[_ {:keys [subs] :as l}] @(:panes world)
            :when (contains? @subs sid)]
      (swap! subs dissoc sid)
      (publish-attached! world l))))

(defn subscribe!
  "Attach a session to a pane. A first frame goes at once: a delta from `after` when the ring still
   holds it, else a full screen, so a late client is exact and a dropped one pays only for what it
   missed. rate-ms lowers the frame rate for this subscriber. Returns {:ok ...}."
  [world session p {:keys [pane after rate-ms]}]
  (let [db (act/db world)
        id (pane-id world pane)]
    (cond
      (nil? id) act/unresolved
      (not (may-read? db p id)) act/unresolved
      :else
      (let [kind (field db id :pane/kind)
            sid (:id @session)
            terminal? (= :terminal kind)
            {:keys [lock ring subs] :as l} (live world id)
            screen (when terminal? (terminal-screen world id))]
        (locking lock
          (let [{:keys [dirty]} l]
            (when dirty (settle! l)))
          (let [n @(:seq l)
                held (when (some? after) (first (filter #(= (long after) (:seq %)) @ring)))
                exited? (= :exited (field db id :pane/state))
                exit-of (field db id :pane/exit)]
            (if terminal?
              (let [now (if-let [s (:screen l)] @s screen)
                    d (when (and held (:screen held)) (scr/diff (:screen held) now))
                    frame (cond-> (cond (nil? held) (full-frame id n now)
                                        (nil? d) (full-frame id n now)
                                        :else {:pane id :seq n :base after :data d})
                            exited? (assoc :exited true)
                            (and exited? (number? exit-of)) (assoc :exit (long exit-of)))]
                (send-frame! session frame)
                (swap! subs assoc sid {:session session :sent n :screen now :rate-ms rate-ms :at (now-ms)}))
              (let [frames (when (and (some? after) (seq @ring) (>= (long after) (dec (long (:seq (first @ring))))))
                             (filterv #(> (long (:seq %)) (long after)) @ring))
                    current? (and (some? after) (= (long after) n))]
                (cond
                  current? (send-frame! session {:pane id :seq n :base after})
                  (some? frames) (doseq [{:keys [frame]} frames] (send-frame! session frame))
                  :else (let [{:keys [state data]} (snapshot world kind id)]
                          (send-frame! session (cond-> {:pane id :seq n :full true :state state}
                                                 data (assoc :data data)))))
                (swap! subs assoc sid {:session session})))
            (publish-attached! world l)
            {:ok {:result {:pane id :kind kind :seq n :subscribed true} :seq (applied-seq world)}}))))))

(defn unsubscribe!
  "Detach a session; the pane goes on."
  [world session {:keys [pane]}]
  (if-let [id (pane-id world pane)]
    (let [l (live world id)]
      (swap! (:subs l) dissoc (:id @session))
      (publish-attached! world l)
      {:ok {:result {:pane id :subscribed false} :seq (applied-seq world)}})
    act/unresolved))

;; ---------------------------------------------------------------- terminals

(defn- flush-output!
  "What has accumulated becomes a blob and a pane-output event, chained to the one before: the
   chain is sha256(previous chain || blob hash)."
  [world {:keys [id owner grant ^ByteArrayOutputStream pending chain n last-flush lock-out offset]}]
  (locking lock-out
    (when (pos? (.size pending))
      (let [bs (.toByteArray pending)
            _ (.reset pending)
            {:keys [blob/sha256 blob/size]} (blob/put! (:blobs world) bs)
            link (blob/sha256-hex (.getBytes (str @chain sha256) "UTF-8"))
            i (swap! n inc)
            done (when offset @offset)]
        (reset! chain link)
        (reset! last-flush (now-ms))
        (act/append! world (log/pending :pane-output id owner
                                        (cond-> {:pane id :n i :blob sha256 :size size :chain link}
                                          done (assoc :offset done))
                                        :delegation grant))
        ;; only what is on the record may leave the holder: a crash here repeats it, never loses it
        (when-let [conn (and done @(:holder world))] (try (holder/ack! conn (str id) done) (catch java.io.IOException _ nil)))))))

(defn- keep-output!
  "Output on its way to the record. end is the holder's offset after these bytes: it moves with them,
   under the same lock, so a flush can never claim bytes it has not got."
  ([world l bs] (keep-output! world l bs nil))
  ([world {:keys [^ByteArrayOutputStream pending lock-out last-output offset] :as l} ^bytes bs end]
   (reset! last-output (now-ms))
   (when (locking lock-out
           (.write pending bs 0 (alength bs))
           (when end (reset! offset end))
           (>= (.size pending) output-chunk))
     (flush-output! world l))))

(defn- on-output! [world {:keys [screen dirty] :as l} ^bytes bs ^String text]
  (swap! screen scr/feed text)
  (reset! dirty true)
  (keep-output! world l bs)
  (kick! world l tick-ms))

(defn- flush-typed!
  "A person's keystrokes, counted and never kept, as one pane-input per burst."
  [world {:keys [id typed]}]
  (let [[old _] (reset-vals! typed {})]
    (doseq [[who bytes] old]
      (act/append! world (log/pending :pane-input id who {:pane id :who who :bytes bytes :from "session"})))))

(defn- closed!
  "The program is gone: the rest of its output on the record, then its exit."
  [world {:keys [id owner grant subs done stopping] :as l} code]
  (try
    (flush-output! world l)
    (flush-typed! world l)
    (publish! world l)
    (let [db (act/db world)
          reason (let [s @stopping] (cond (keyword? s) s s :world-stop :else :exit))]
      (act/append! world (log/pending :pane-closed id owner (cond-> {:pane id :reason reason} code (assoc :exit code))
                                      :delegation grant))
      (set-field! world id :pane/state :keyword :exited)
      (when code (set-field! world id :pane/exit :number code))
      (when (field db id :pane/kind)
        (doseq [[sid {:keys [session]}] @subs]
          (when-not (send-frame! session (cond-> {:pane id :seq @(:seq l) :exited true} code (assoc :exit code)))
            (swap! subs dissoc sid)))))
    (catch Throwable t (timbre/warn t "pane close failed"))
    (finally (deliver done code))))

(defn- terminal-state
  "The live half of a terminal."
  [id owner grant screen]
  (assoc (blank-live id)
         :owner owner :grant grant
         :screen (atom screen) :dirty (atom false) :scheduled (atom false)
         :pending (ByteArrayOutputStream.) :lock-out (Object.) :chain (atom "") :n (atom 0)
         :last-flush (atom (now-ms)) :last-output (atom 0) :typed (atom {}) :done (promise) :stopping (atom false)
         :ring (atom [{:seq 0 :screen screen}])))

(defn- on-holder-event!
  "What the holder says about a terminal, in order, on its reader thread."
  [world {:keys [ev id] :as e}]
  (when-let [{:keys [screen dirty offset] :as l} (get @(:panes world) (->uuid id))]
    (case ev
      ;; offsets decide what is new: after a reattach the holder repeats nothing we hold
      :output (let [^bytes bs (:bytes e)
                    end (+ (long (:offset e)) (alength bs))]
                (when (> end (long @offset))
                  (let [skip (max 0 (- (long @offset) (long (:offset e))))
                        fresh (if (pos? skip) (java.util.Arrays/copyOfRange bs (int skip) (alength bs)) bs)]
                    (keep-output! world l fresh end))))
      :screen (do (swap! screen scr/apply-frame (:frame e))
                  (reset! dirty true)
                  (kick! world l 0))
      :exit (do (closed! world l (:code e))
                (when-let [conn @(:holder world)] (holder/forget! conn id)))
      :error (do (timbre/warn "holder refused a terminal" {:pane id :why (:text e)})
                 (closed! world l 127))
      nil)))

(defn- holder-lost!
  "The holder went away with its PTYs. Each terminal ends where it was: what it showed stays."
  [world conn]
  (compare-and-set! (:holder world) conn nil)
  (timbre/warn "terminal holder lost")
  (doseq [[_ {:keys [pty done] :as l}] @(:panes world)
          :when (and (:holder pty) (not (realized? done)))]
    (reset! (:stopping l) :holder-lost)
    (closed! world l nil)))

(defn holder-conn!
  "The connection to this world's holder, made if there is one to reach, or nil."
  [world]
  (or @(:holder world)
      (when-let [path (holder/socket-path (:data-dir world))]
        (let [self (atom nil)
              conn (holder/connect path
                                   (fn [e] (on-holder-event! world e))
                                   (fn [] (holder-lost! world @self)))]
          (when conn
            (reset! self conn)
            (reset! (:holder world) conn)
            (timbre/info "terminal holder connected" {:path path :panes (count (:panes conn))}))
          conn))))

(defn open-terminal!
  "A shell in a pty, on an Adapter. Returns {:pane id :seq n}."
  [world p {:keys [adapter dir title cols rows grant via fields box]}]
  (let [cols (long (or cols 80))
        rows (long (or rows 24))
        {:keys [pane seq]} (create! world p :terminal adapter
                                    {:title title :grant grant :via via :size {:cols cols :rows rows}
                                     :fields (or fields {})})
        s0 (scr/create cols rows)
        conn (holder-conn! world)
        lv (cond-> (terminal-state pane p grant s0)
             conn (assoc :offset (atom 0)))
        _ (swap! (:panes world) assoc pane lv)
        command (if box (confine/command box (shell-command)) (shell-command))
        env (confine/environment box)
        pty (if conn
              (do (holder/open! conn (str pane) {:dir (str dir) :argv command :env env :cols cols :rows rows})
                  (holder/attach! conn (str pane) 0)
                  {:holder true :id (str pane)})
              (pty/spawn! {:command command :env env :dir dir :cols cols :rows rows
                           :on-output (fn [bs text] (on-output! world lv bs text))
                           :on-exit (fn [code] (closed! world lv code))}))]
    (swap! (:panes world) assoc-in [pane :pty] pty)
    {:pane pane :seq seq}))

(defn terminal-live
  "The running side of a terminal, or nil."
  [world id]
  (let [l (get @(:panes world) id)] (when (and (:pty l) (not (realized? (:done l)))) l)))

(defn send!
  "Bytes to a terminal, on the record. A model's text is kept; a person's keystrokes are only
   counted, so a password typed into a shell stays off the log."
  [world p id ^bytes bs text from]
  (let [{:keys [pty typed] :as l} (terminal-live world id)]
    (if (:holder pty)
      (holder/write! @(:holder world) (:id pty) bs)
      (pty/write! pty bs))
    (if text
      (do (flush-typed! world l)
          (act/append! world (log/pending :pane-input id p {:pane id :who p :bytes (alength bs) :from from :text text})))
      (do (swap! typed update p (fnil + 0) (alength bs))
          (applied-seq world)))))

(def quiet-ms "Output this long gone means a program has probably finished for now." 1200)

(defn await-quiet!
  "Block until a terminal has gone quiet since `since`, has exited, or max-ms has passed. A model
   gets the screen with its send, so what it reports is what ran, not what it expects."
  [world id since max-ms]
  (when-let [{:keys [last-output done]} (terminal-live world id)]
    (let [deadline (+ (now-ms) (long max-ms))]
      (loop []
        (let [t (now-ms)
              heard (long @last-output)
              quiet-since (max (long since) heard)]
          (when-not (or (realized? done) (>= t deadline) (>= (- t quiet-since) (long quiet-ms)))
            (Thread/sleep 25)
            (recur)))))))

(defn resize!
  "A new size for a terminal: the program told, the screen refit, every client repainted."
  [world id cols rows]
  (when-let [{:keys [pty screen dirty] :as l} (terminal-live world id)]
    (if (:holder pty)
      (holder/resize! @(:holder world) (:id pty) cols rows) ; its screen comes back as an event
      (do (pty/resize! pty cols rows)
          (swap! screen scr/resize cols rows)
          (reset! dirty true)
          (kick! world l 0))))
  (set-field! world id :pane/size :map {:cols cols :rows rows}))

(defn close!
  "End a pane. The process of a terminal is killed; its record stays."
  [world id]
  (swap! (:panes world) (fn [m] (if (contains? m id) (update m id dissoc :watch) m)))
  (if-let [{:keys [pty done]} (terminal-live world id)]
    (do (if (:holder pty)
          (holder/kill! @(:holder world) (:id pty))
          (pty/kill! pty))
        (deref done 2000 nil))
    (when (= :open (field (act/db world) id :pane/state))
      (act/append! world (log/pending :pane-closed id (field (act/db world) id :pane/owner) {:pane id :reason :closed}))
      (set-field! world id :pane/state :keyword :exited)
      (push! world id {:exited true}))))

;; ---------------------------------------------------------------- the world's lifetime

(defn sweep!
  "Idle output and typing onto the record, so a quiet terminal's last screen survives a crash."
  [world]
  (doseq [[_ {:keys [pty last-flush] :as l}] @(:panes world)
          :when pty]
    (when (> (- (now-ms) (long @last-flush)) idle-flush-ms)
      (flush-output! world l))
    (flush-typed! world l)))

(defn stop-all!
  "Before the log closes. A terminal in the holder is left running with its output on the record, and
   the next world picks it up; one here stops with this world."
  [world]
  (let [running (filter :pty (vals @(:panes world)))
        held (filter #(:holder (:pty %)) running)
        here (remove #(:holder (:pty %)) running)]
    (doseq [{:keys [done] :as l} held
            :when (not (realized? done))]
      (try (flush-output! world l) (flush-typed! world l)
           (catch Throwable t (timbre/warn t "pane flush failed"))))
    (when-let [conn @(:holder world)]
      (reset! (:holder world) nil)
      (holder/close! conn))
    (doseq [{:keys [stopping pty]} here]
      (reset! stopping true)
      (try (pty/kill! pty) (catch Throwable t (timbre/warn t "pane kill failed"))))
    (doseq [{:keys [done]} here]
      (deref done 2000 nil))))

(defn- revive!
  "A terminal the holder kept through a restart: its record's chain and offset, and the holder asked
   for what came after."
  [world conn id {:keys [cols rows]}]
  (let [db (act/db world)
        owner (field db id :pane/owner)
        grant (:object/created-under (objects/entity db id))
        last-output (last (filter #(= :pane-output (:event/kind %))
                                  (log/read-stream (:log world) id 0 Long/MAX_VALUE)))
        {:keys [n chain offset]} (:event/body last-output)
        start (long (or offset 0))
        lv (-> (terminal-state id owner grant (scr/create cols rows))
               (assoc :offset (atom start) :pty {:holder true :id (str id)}))]
    (reset! (:n lv) (long (or n 0)))
    (reset! (:chain lv) (or chain ""))
    (swap! (:panes world) assoc id lv)
    (holder/attach! conn (str id) start)))

(defn reconcile!
  "At boot: a terminal the holder still has goes on; any other still open was lost with its world."
  [world]
  (let [db (act/db world)
        conn (holder-conn! world)
        held (:panes conn)]
    (doseq [id (objects/objects-with-field db :pane/kind)
            :when (and (= :terminal (field db id :pane/kind)) (= :open (field db id :pane/state)))]
      (if-let [h (get held (str id))]
        (revive! world conn id h)
        (do (act/append! world (log/pending :pane-closed id (field db id :pane/owner) {:pane id :reason :lost}))
            (set-field! world id :pane/state :keyword :exited))))
    ;; what the record does not know is nobody's: a pane from a world whose log was replaced
    (doseq [id (keys held)
            :when (not= :open (field db (->uuid id) :pane/state))]
      (holder/forget! conn id))))

(defn terminal-lines
  "Every line still held of a terminal, oldest first."
  [world id]
  (let [{:keys [pty] :as l} (terminal-live world id)
        conn @(:holder world)
        held (when (and (:holder pty) conn) (holder/text conn (:id pty)))]
    (cond
      held held
      (or (:holder pty) (nil? l)) (let [s (or (when (:offset (get @(:panes world) id)) (replay-screen world id))
                                              (terminal-screen world id))]
                                    (into (scr/scrollback-lines s) (scr/screen-lines s)))
      :else (let [s (terminal-screen world id)]
              (into (scr/scrollback-lines s) (scr/screen-lines s))))))
