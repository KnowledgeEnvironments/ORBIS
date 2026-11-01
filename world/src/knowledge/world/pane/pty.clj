(ns knowledge.world.pane.pty
  "A real pseudo-terminal from the JVM, through pty4j. forkpty over FFM is not safe here: a child
   forked from a many-threaded JVM may run no Java before exec, and pty4j's tested native helper
   already does that step. One reader thread drains the master for as long as the child lives,
   because an undrained pty blocks the program writing to it."
  (:import [com.pty4j PtyProcess PtyProcessBuilder WinSize]
           [java.io InputStream OutputStream]
           [java.nio ByteBuffer CharBuffer]
           [java.nio.charset CharsetDecoder CodingErrorAction StandardCharsets]
           [java.util HashMap]
           [java.util.concurrent TimeUnit]))

(def env-kept
  "What a terminal inherits from the world. An allow-list: the world's environment holds the model
   key, and a model that can read the screen can run `env`."
  ["PATH" "HOME" "USER" "LOGNAME" "SHELL" "LANG" "LC_ALL" "LC_CTYPE" "TMPDIR"])

(defn clean-env
  "The environment a terminal starts with."
  ^HashMap []
  (let [env (HashMap.)]
    (doseq [k env-kept]
      (when-let [v (System/getenv k)] (.put env k v)))
    (.put env "TERM" "xterm-256color")
    env))

(defn- utf8-decoder
  "A fn from a chunk of n bytes to the text it completes; a character cut at the chunk's edge waits
   for the next chunk."
  []
  (let [^CharsetDecoder dec (-> StandardCharsets/UTF_8 .newDecoder
                                (.onMalformedInput CodingErrorAction/REPLACE)
                                (.onUnmappableCharacter CodingErrorAction/REPLACE))
        in (ByteBuffer/allocate 16384)
        out (CharBuffer/allocate 16384)]
    (fn [^bytes bs ^long n]
      (.put in bs 0 n)
      (.flip in)
      (.clear out)
      (.decode dec in out false)
      (.compact in)
      (.flip out)
      (.toString out))))

(defn spawn!
  "Start command in a pty. on-output gets [bytes text] for each chunk, on the reader thread;
   on-exit gets the exit code once, after the output is drained."
  [{:keys [command dir cols rows on-output on-exit env]}]
  (let [^PtyProcess proc (-> (PtyProcessBuilder. ^"[Ljava.lang.String;" (into-array String command))
                             (.setEnvironment (or env (clean-env)))
                             (.setDirectory (str dir))
                             (.setInitialColumns (int cols))
                             (.setInitialRows (int rows))
                             (.setConsole false)
                             (.setRedirectErrorStream true)
                             (.start))
        ^InputStream in (.getInputStream proc)
        decode (utf8-decoder)
        reader (doto (Thread.
                      (fn []
                        (let [buf (byte-array 8192)]
                          (try
                            (loop []
                              (let [n (.read in buf)]
                                (when (pos? n)
                                  (on-output (java.util.Arrays/copyOf buf n) (decode buf n))
                                  (recur))))
                            (catch java.io.IOException _ nil))))
                      "pane-pty-reader")
                 (.setDaemon true)
                 (.start))
        _waiter (doto (Thread.
                       (fn []
                         (let [code (.waitFor proc)]
                           ;; a child's own children can hold the pty open past its exit
                           (.join ^Thread reader 500)
                           (on-exit code)))
                       "pane-pty-waiter")
                  (.setDaemon true)
                  (.start))]
    {:proc proc :out (.getOutputStream proc)}))

(defn write!
  "Bytes to the program."
  [{:keys [^OutputStream out]} ^bytes bs]
  (locking out
    (.write out bs)
    (.flush out)))

(defn resize!
  "Tell the program its new size."
  [{:keys [^PtyProcess proc]} cols rows]
  (.setWinSize proc (WinSize. (int cols) (int rows))))

(defn alive? [{:keys [^PtyProcess proc]}] (.isAlive proc))

(defn kill!
  "End the program: a hangup first, then for certain (an interactive shell ignores a polite TERM)."
  [{:keys [^PtyProcess proc]}]
  (.destroy proc)
  (when-not (.waitFor proc 300 TimeUnit/MILLISECONDS)
    (.destroyForcibly proc)))
