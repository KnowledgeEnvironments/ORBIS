(ns knowledge.world.pane.holder
  "The world's side of the terminal holder (holder/, docs/panes.md): one socket to the small process
   that owns this world's PTYs and outlives it. A frame either way is [u32 length][u8 op][u8 id
   length][id][payload], big-endian. One reader thread delivers events in order; requests are written
   under a lock and never wait, except `hello` and `text`, which are answered on the reader."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [taoensso.timbre :as timbre])
  (:import [java.net StandardProtocolFamily UnixDomainSocketAddress]
           [java.nio ByteBuffer]
           [java.nio.channels SocketChannel]
           [java.util.concurrent TimeUnit]))

(def ^:private ops {:hello 1 :open 2 :write 3 :resize 4 :kill 5 :ack 6 :attach 7 :forget 8 :text 9})
(def ^:private version 1)
(def ^:private frame-max (* 32 1024 1024))
(def ^:private wait-ms 2000)

(defn socket-path
  "Where this world's holder listens, or nil: KE_HOLDER_SOCK, else holder.sock in the data dir."
  [data-dir]
  (let [p (or (System/getenv "KE_HOLDER_SOCK") (str (io/file (str data-dir) "holder.sock")))]
    (when (.exists (io/file p)) p)))

(defn- utf8 ^bytes [s] (.getBytes (str s) "UTF-8"))

(defn- send-frame!
  [{:keys [^SocketChannel ch lock]} op id ^bytes body]
  (let [idb (utf8 (or id ""))
        n (+ 2 (alength idb) (alength body))
        bb (doto (ByteBuffer/allocate (+ 4 n))
             (.putInt n) (.put (unchecked-byte (ops op))) (.put (unchecked-byte (alength idb)))
             (.put idb) (.put body) (.flip))]
    (locking lock
      (while (.hasRemaining bb) (.write ch bb)))))

(defn- string-list [^ByteBuffer bb items]
  (.putShort bb (unchecked-short (count items)))
  (doseq [^bytes b items]
    (.putShort bb (unchecked-short (alength b)))
    (.put bb b)))

(defn open!
  "Start argv in a pty in the holder, under this pane's id. argv is already boxed: the holder runs what
   it is given, as pty.clj does. The answer comes as an event."
  [conn id {:keys [dir argv env cols rows]}]
  (let [dirb [(utf8 dir)]
        argvb (mapv utf8 argv)
        envb (mapv (fn [[k v]] (utf8 (str k "=" v))) env)
        size (reduce + (+ 10) (map #(+ 2 (alength ^bytes %)) (concat dirb argvb envb)))
        bb (ByteBuffer/allocate size)]
    (.putShort bb (unchecked-short cols))
    (.putShort bb (unchecked-short rows))
    (string-list bb dirb)
    (string-list bb argvb)
    (string-list bb envb)
    (send-frame! conn :open id (java.util.Arrays/copyOf (.array bb) (.position bb)))))

(defn write! [conn id ^bytes bs] (send-frame! conn :write id bs))

(defn resize! [conn id cols rows]
  (send-frame! conn :resize id (.array (doto (ByteBuffer/allocate 4)
                                         (.putShort (unchecked-short cols))
                                         (.putShort (unchecked-short rows))))))

(defn kill! [conn id] (send-frame! conn :kill id (byte-array 0)))

(defn- u64 ^bytes [n] (.array (doto (ByteBuffer/allocate 8) (.putLong (long n)))))

(defn ack!
  "These bytes of output are on the record: the holder may let them go."
  [conn id offset]
  (send-frame! conn :ack id (u64 offset)))

(defn attach!
  "Send this pane's screen, and its output after offset."
  [conn id offset]
  (send-frame! conn :attach id (u64 offset)))

(defn forget! [conn id] (send-frame! conn :forget id (byte-array 0)))

(defn text
  "What the holder still has of a terminal, scrollback then screen, as lines. nil if it did not answer."
  [{:keys [pending-text] :as conn} id]
  (let [p (promise)]
    (swap! pending-text update id (fnil conj []) p)
    (send-frame! conn :text id (byte-array 0))
    (let [s (deref p wait-ms nil)]
      (when s (str/split s #"\n" -1)))))

;; ---------------------------------------------------------------- reading

(defn- get-u8 [^ByteBuffer b] (bit-and 0xff (.get b)))
(defn- get-u16 [^ByteBuffer b] (bit-and 0xffff (.getShort b)))
(defn- get-str ^String [^ByteBuffer b n]
  (let [a (byte-array n)]
    (.get b a)
    (String. a "UTF-8")))

(defn screen-frame
  "The SCREEN event's payload as data, with rows {y [[text attr] ...]}. attr nil for the default style;
   text empty for the right half of a wide character."
  [^ByteBuffer b]
  (let [cols (get-u16 b) rows (get-u16 b) cx (get-u16 b) cy (get-u16 b)
        flags (get-u8 b) shape (get-u8 b) scrolled (bit-and 0xffffffff (long (.getInt b)))
        title (get-str b (get-u16 b))
        modes (set (repeatedly (get-u8 b) #(get-u16 b)))
        n (get-u16 b)
        one (volatile! {})]
    {:cols cols :rows rows :cx cx :cy cy
     :cursor-visible (bit-test flags 0) :alt (bit-test flags 1) :keypad (bit-test flags 2) :full (bit-test flags 3)
     :cursor-shape shape :scrolled scrolled :title (when (seq title) title) :modes modes
     :lines (loop [i 0 acc {}]
              (if (= i n)
                acc
                (let [y (get-u16 b)
                      cells (get-u16 b)
                      line (loop [j 0 row (transient [])]
                             (if (= j cells)
                               (persistent! row)
                               (let [t (get-str b (get-u8 b))
                                     alen (get-u8 b)
                                     a (when (pos? alen)
                                         (let [s (get-str b alen)]
                                           ;; a screen uses few styles: share them
                                           (or (get @one s) (do (vswap! one assoc s s) s))))]
                                 (recur (inc j) (conj! row [t a])))))]
                  (recur (inc i) (assoc acc y line)))))}))

(defn- read-fully [^SocketChannel ch ^ByteBuffer b]
  (while (.hasRemaining b)
    (when (neg? (.read ch b)) (throw (java.io.EOFException. "holder closed"))))
  (.flip b)
  b)

(defn- event [{:keys [listing pending-text]} on-event op id ^ByteBuffer b]
  (case (int op)
    101 (swap! listing assoc :version (get-u16 b))
    102 (swap! listing update :panes assoc id
               {:total (.getLong b) :exited (pos? (get-u8 b)) :code (.getInt b) :cols (get-u16 b) :rows (get-u16 b)})
    103 (deliver (:done @listing) true)
    104 nil
    105 (on-event {:ev :error :id id :text (get-str b (.remaining b))})
    106 (let [s (get-str b (.remaining b))
              [p & more] (get @pending-text id)]
          (swap! pending-text assoc id (vec more))
          (when p (deliver p s)))
    110 (let [off (.getLong b)
              bs (byte-array (.remaining b))]
          (.get b bs)
          (on-event {:ev :output :id id :offset off :bytes bs}))
    111 (on-event {:ev :screen :id id :frame (screen-frame b)})
    112 (on-event {:ev :exit :id id :code (.getInt b)})
    nil))

(defn- read-loop [{:keys [^SocketChannel ch] :as conn} on-event on-lost]
  (try
    (let [head (ByteBuffer/allocate 4)]
      (loop []
        (.clear head)
        (let [n (.getInt ^ByteBuffer (read-fully ch head))]
          (when (or (< n 2) (> n frame-max)) (throw (ex-info "bad frame from the holder" {:n n})))
          (let [b (read-fully ch (ByteBuffer/allocate n))
                op (get-u8 b)
                id (get-str b (get-u8 b))]
            (try (event conn on-event op id b)
                 (catch Throwable t (timbre/warn t "holder event failed" {:op op :id id}))))
          (recur))))
    (catch java.nio.channels.ClosedChannelException _ nil)
    (catch java.io.IOException _ (on-lost))
    (catch Throwable t (timbre/warn t "holder reader ended") (on-lost))))

(defn connect
  "A connection to the holder at path, or nil when nothing answers. on-event gets each event on the
   reader thread; on-lost is called once if the holder goes away. Returns the connection with
   :panes, what the holder is running: {id {:total :exited :code :cols :rows}}."
  [path on-event on-lost]
  (try
    (let [ch (doto (SocketChannel/open StandardProtocolFamily/UNIX)
               (.connect (UnixDomainSocketAddress/of ^String path)))
          conn {:ch ch :lock (Object.) :pending-text (atom {})
                :listing (atom {:panes {} :done (promise)})}
          _ (doto (Thread. ^Runnable (fn [] (read-loop conn on-event on-lost)) "pane-holder-reader")
              (.setDaemon true)
              (.start))
          _ (send-frame! conn :hello nil (byte-array 0))
          answered (deref (:done @(:listing conn)) wait-ms nil)
          {:keys [panes] v :version} @(:listing conn)]
      (cond
        (not answered) (do (.close ch) (timbre/warn "holder did not answer" {:path path}) nil)
        (not= version v) (do (.close ch) (timbre/warn "holder speaks another version" {:have v :want version}) nil)
        :else (assoc conn :panes (or panes {}))))
    (catch java.io.IOException e
      (timbre/warn "no holder" {:path path :error (.getMessage e)})
      nil)))

(defn close!
  "Leave the holder running; a world going down only hangs up."
  [{:keys [^SocketChannel ch]}]
  (when ch (.close ch)))
