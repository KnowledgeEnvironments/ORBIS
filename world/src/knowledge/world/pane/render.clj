(ns knowledge.world.pane.render
  "Rendering in the world, never in a client: LaTeX through tectonic on the host, its path a setting
   (KE_TECTONIC, else tectonic on PATH), the first page rasterized by sips so a phone can show it.
   With no renderer, a clear err: the source still goes to a client as a view of kind tex.

   TeX source can come from a model, so it is untrusted. Every compile and rasterize runs in the box
   with no network, tectonic `--untrusted --only-cached`, reading its packages from a cache primed
   once, outside the box, from a fixed document no model wrote. Writing goes only to a job directory."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.confine :as confine]
            [knowledge.world.log.blob :as blob]
            [knowledge.world.pane.pty :as pty])
  (:import [java.io File]
           [java.nio.file Files]
           [java.security MessageDigest]
           [java.util UUID]
           [java.util.concurrent Semaphore TimeUnit]))

(def timeout-s "A first compile fetches packages." 180)

(defn- on-path [^String program]
  (some (fn [dir] (let [f (io/file dir program)] (when (.canExecute f) (.getPath f))))
        (concat (str/split (or (System/getenv "PATH") "") #":") ["/opt/homebrew/bin" "/usr/local/bin"])))

(defn renderer
  "The tectonic binary, or nil."
  []
  (let [configured (System/getenv "KE_TECTONIC")]
    (if (str/blank? configured)
      (on-path "tectonic")
      (when (.canExecute (io/file configured)) configured))))

(defn- run
  ([command ^File dir] (run command dir nil timeout-s))
  ([command ^File dir environment seconds]
   (let [pb (doto (ProcessBuilder. ^java.util.List command) (.directory dir) (.redirectErrorStream true))
         env (.environment pb)
         _ (.clear env)
         _ (.putAll env (or environment (pty/clean-env)))
         p (.start pb)
         out (future (slurp (.getInputStream p)))
         finished (.waitFor p (long seconds) TimeUnit/SECONDS)]
     (when-not finished (.destroyForcibly p))
     {:exit (if finished (.exitValue p) -1) :log (deref out 2000 "") :timed-out (not finished)})))

(defn- tail [^String s n] (if (> (count s) n) (subs s (- (count s) n)) s))

(defn- base-name [^File f] (str/replace (.getName f) #"\.[^.]*$" ""))

(defn- delete-tree! [^File d]
  (doseq [^File f (reverse (file-seq d))] (.delete f)))

;; ---------------------------------------------------------------- the box

(defn- tex-dir ^File [world] (io/file (:data-dir world) "tex"))
(defn- cache-dir
  "Tectonic's packages: the machine's, not one world's, since a fetch takes minutes and a world can
   be thrown away. KE_TEX_CACHE names another place. Read-only from inside the box."
  ^File [_world]
  (io/file (or (System/getenv "KE_TEX_CACHE") (io/file (System/getProperty "user.home") ".cache" "orbis" "tectonic"))))

(defn- with-cache
  "A command's environment with tectonic's cache named."
  ^java.util.HashMap [world ^java.util.Map env]
  (doto (java.util.HashMap. env) (.put "TECTONIC_CACHE_DIR" (.getPath (cache-dir world)))))

(defn- boxed
  "{:argv :env} for a command in the box: writes only under job, no network, packages read-only."
  [world ^File job extra-read argv]
  (let [box (confine/spec {:root (.getPath job) :data-dir (str (:data-dir world)) :adapter "tex-render"
                           :read [(.getPath (cache-dir world))] :data-read extra-read :network "none"})]
    {:argv (confine/command box argv) :env (with-cache world (confine/environment box))}))

(defn- job-dir ^File [world]
  (doto (io/file (tex-dir world) "job" (str (UUID/randomUUID))) .mkdirs))

(def preamble
  "The fixed head of a formula's document. Its packages are what priming fetches."
  (str "\\documentclass[10pt,border=1pt]{standalone}\n"
       "\\usepackage{amsmath,amssymb,amsfonts,mathtools,mathrsfs,stmaryrd}\n"
       "\\begin{document}\n"))

(defn- document [source] (str preamble "$\\displaystyle " source "$\n\\end{document}\n"))

(def text-preamble
  "The head of a text fragment: a page as tall as what is on it, about a chat's measure wide."
  (str "\\documentclass[11pt,border=6pt,varwidth=34em]{standalone}\n"
       "\\usepackage{amsmath,amssymb}\n"
       "\\begin{document}\n"))

(def ^:private tikz-libraries
  "What a diagram may ask for by \\usetikzlibrary, and has without asking. Nothing else is cached."
  "arrows.meta,positioning,shapes.geometric,shapes.misc,calc,fit,matrix,automata,decorations.pathmorphing")

(def diagram-preamble
  "The head of a diagram: a small picture, in a chat, with TikZ and commutative diagrams to hand."
  ;; varwidth is a limit, not a width: at a chat's measure a wide diagram was cut off at the page edge
  (str "\\documentclass[11pt,border=6pt,varwidth=120em]{standalone}\n"
       "\\usepackage{amsmath,amssymb}\n"
       "\\usepackage{tikz}\n\\usepackage{tikz-cd}\n"
       "\\usetikzlibrary{" tikz-libraries "}\n"))

(def ^:private plot-head "A plot costs seconds to load, so only a plot loads it." "\\usepackage{pgfplots}\n\\pgfplotsset{compat=1.18}\n")

(def ^:private diagram #"\\begin\s*\{\s*(tikzpicture|tikzcd)\s*\}")
(def ^:private plot #"\\begin\s*\{\s*(axis|semilogxaxis|semilogyaxis|loglogaxis)\s*\}")

(def ^:private text-commands
  #"\\(section|subsection|subsubsection|paragraph|part|chapter|title|maketitle|textbf|textit|texttt|textsc|emph|underline|item|begin\s*\{\s*(itemize|enumerate|description|center|quote|quotation|tabular|verbatim|abstract|tikzpicture|tikzcd)\s*\})(?![a-zA-Z])")

(defn- prose?
  "Three words in a row, once what commands hold is set aside: `\\text{a b c}` is still math."
  [^String source]
  (some? (re-find #"[A-Za-z]{2,}[ \t\n]+[A-Za-z]{2,}[ \t\n]+[A-Za-z]{2,}"
                  (str/replace source #"\\[a-zA-Z]+\*?(\{[^{}]*\})*" " "))))

(defn kind-of
  "What a source is: a :document, :text with typesetting commands or sentences in it, or :math."
  [^String source]
  (cond (re-find #"\\documentclass|\\begin\s*\{\s*document\s*\}" source) :document
        (or (re-find text-commands source) (prose? source)) :text
        :else :math))

(defn- as-document
  "The file a source compiles as, by what it is. A document keeps its own head."
  [kind ^String source]
  (case kind
    :document (if (re-find #"\\documentclass" source)
                source
                (str "\\documentclass[11pt]{article}\n\\usepackage{amsmath,amssymb}\n" source "\n"))
    :text (str (if (re-find diagram source)
                 (str diagram-preamble (when (re-find plot source) plot-head) "\\begin{document}\n")
                 text-preamble)
               source "\n\\end{document}\n")
    (document source)))

(def ^:private math-fonts
  "Every Type 1 face math can land on at a size, so nothing is fetched from inside the box."
  (for [[family sizes] [["cmr" [5 6 7 8 9 10 12]] ["cmmi" [5 6 7 8 9 10 12]] ["cmsy" [5 6 7 8 9 10]] ["cmex" [7 8 9 10]]
                        ["cmbx" [5 6 7 8 9 10 12]] ["cmti" [7 8 9 10 12]] ["cmss" [8 9 10 12]] ["cmtt" [8 9 10 12]]
                        ["cmmib" [5 6 7 8 9 10]] ["cmbsy" [5 6 7 8 9 10]]
                        ["msam" [5 6 7 8 9 10]] ["msbm" [5 6 7 8 9 10]] ["eufm" [5 6 7 8 9 10]] ["eufb" [5 6 7 8 9 10]]
                        ["rsfs" [5 7 10]] ["stmary" [5 6 7 8 9 10]]]
        size sizes]
    (str family size)))

(defn- priming-document
  "A fixed document no model wrote: the math packages, each face at each size, and the text faces."
  []
  (str preamble
       (apply str (map-indexed (fn [i f] (str "\\font\\p" (apply str (map #(char (+ 97 (- (int %) 48))) (str i))) "=" f " "
                                              "\\p" (apply str (map #(char (+ 97 (- (int %) 48))) (str i))) " A\n"))
                               math-fonts))
       "\\normalfont $\\text{a}_{\\text{a}_{\\text{a}}}\\mathrm{a}\\mathbf{a}\\mathit{a}\\mathsf{a}\\mathtt{a}\\boldsymbol{a}"
       "\\mathbb{R}\\mathcal{A}\\mathscr{A}\\mathfrak{A}\\textbf{a}\\textit{a}\\texttt{a}\\oint\\sum\\Diamond"
       "\\rtimes\\llbracket x\\rrbracket\\xrightarrow[a]{b}\\left(\\frac{a}{b}\\right)\\sqrt{a}\\binom{a}{b}"
       "\\begin{pmatrix}a\\\\b\\end{pmatrix}\\begin{cases}a\\\\b\\end{cases}$\n\\end{document}\n"))

(def ^:private document-packages
  "What a model's document may ask for: the rest fails with TeX's own reason, as nothing is fetched then."
  ["fontenc" "inputenc" "lmodern" "geometry" "hyperref" "graphicx" "xcolor" "booktabs" "enumitem" "fancyhdr"
   "titlesec" "listings" "tabularx" "longtable" "array" "multirow" "caption" "float" "url" "parskip" "microtype"
   "amsmath" "amssymb" "amsthm" "mathtools" "tikz" "tikz-cd" "pgfplots"])

(def ^:private every-face
  "Each family, weight and shape a document reaches for: a font file is fetched only when it is used."
  (apply str (for [family ["\\rmfamily" "\\sffamily" "\\ttfamily"]
                   weight ["\\mdseries" "\\bfseries"]
                   shape ["\\upshape" "\\itshape" "\\slshape" "\\scshape"]]
               (str "{" family weight shape " a}"))))

(defn- priming-documents
  "Formulas, text fragments, and the usual run of article, so a document compiles with no network."
  []
  [(priming-document)
   (str text-preamble "\\section*{a \\texttt{a}}\\subsection*{a \\texttt{a}}" every-face
        "\\begin{itemize}\\item a\\end{itemize}\\begin{enumerate}\\item a\\end{enumerate}\n\\end{document}\n")
   (str "\\documentclass[11pt]{article}\n\\usepackage[T1]{fontenc}\n\\usepackage[utf8]{inputenc}\n"
        (apply str (for [p (remove #{"fontenc" "inputenc"} document-packages)] (str "\\usepackage{" p "}\n")))
        "\\begin{document}\n\\section{a \\texttt{a}}\\subsection{a \\texttt{a}}" every-face
        "\\begin{tabular}{ll}\\toprule a&b\\\\\\bottomrule\\end{tabular}\n"
        "\\end{document}\n")
   (str "\\documentclass[11pt]{article}\n\\usepackage{amsmath,amssymb}\n\\begin{document}\n"
        "\\title{a}\\author{a}\\date{a}\\maketitle\\tableofcontents\\section{a \\texttt{a}}\\subsection{a \\texttt{a}}"
        every-face "\\begin{itemize}\\item a\\end{itemize}\n\\end{document}\n")
   ;; a diagram and a plot, touching what loads only when it is drawn
   (str diagram-preamble plot-head "\\begin{document}\n"
        "\\begin{tikzpicture}[>=Stealth]\\node[draw,rectangle,rounded corners] (a) {a $x$};\\node[draw,circle,right=of a] (b) {b};"
        "\\node[draw,diamond,below=of a] (c) {\\textbf{c}};\\draw[->] (a) -- (b);\\draw[-{Latex}] (a) -- (c);"
        "\\draw[decorate,decoration=snake] (b) -- (c);\\end{tikzpicture}\n"
        "\\begin{tikzcd}A \\arrow[r,\"f\"] & B\\end{tikzcd}\n"
        "\\begin{tikzpicture}\\begin{axis}[xlabel=$x$,ylabel=$y$]\\addplot coordinates {(0,0)(1,1)};\\end{axis}\\end{tikzpicture}\n"
        "\\end{document}\n")])

(defn- primed-mark
  "Stands for one priming set: change what is primed and the next render primes again."
  ^File [world]
  (io/file (cache-dir world) (str ".primed-" (Integer/toHexString (hash (priming-documents))))))

(def ^:private priming (atom nil))

(defn prime!
  "Fill tectonic's cache once, from fixed documents: the one run with network and without the box,
   because nothing a model wrote is in them. Starts in the background; true when ready."
  [world]
  (or (.exists (primed-mark world))
      (do (when (compare-and-set! priming nil :running)
            (future
              (try
                (.mkdirs (cache-dir world))
                (let [job (job-dir world)
                      f (io/file job "prime.tex")
                      tectonic (renderer)
                      ;; each in turn: a later one needing what an earlier one fetched costs nothing
                      fine (doall (for [doc (priming-documents)]
                                    (do (spit f doc)
                                        (:exit (run [tectonic "--untrusted" "--chatter" "minimal" "--outdir" (.getPath job) (.getPath f)]
                                                 job (with-cache world (pty/clean-env)) timeout-s)))))]
                  (when (every? #(zero? (long %)) fine) (spit (primed-mark world) "ok"))
                  (doseq [^File x (reverse (file-seq job))] (.delete x)))
                (finally (reset! priming nil)))))
          false)))

;; ---------------------------------------------------------------- a formula

(def max-source-chars 4096)
(def formula-seconds "A formula that takes longer is stopped." 20)
(def raster-width-factor 4)
(def max-raster-width 4000)
(def max-rendered-bytes (* 2 1024 1024))
(def ^:private two-at-once (Semaphore. 2))

(def refused
  "What a formula has no use for. A first wall, ahead of the box: anything that opens a file, runs
   something, or redefines the language."
  #"\\(input|include|openin|openout|read|readline|write|immediate|newwrite|newread|catcode|csname|endcsname|expandafter|usepackage|RequirePackage|documentclass|def|gdef|edef|xdef|let|futurelet|newcommand|renewcommand|providecommand|DeclareMathOperator|special|directlua|pdf[a-zA-Z]*|font|everyjob|everypar|loop|repeat|newpage|clearpage|shipout|output|jobname|end\{document\}|begin\{document\})(?![a-zA-Z])|\^\^|\\end\s*\{\s*document")

(def refused-in-a-document
  "A document may have a head of its own, but opens no file, runs nothing and writes nothing. The box
   holds regardless; this is the reason a person sees."
  #"\\(input|include|openin|openout|read|readline|write|immediate|newwrite|newread|catcode|csname|endcsname|expandafter|def|gdef|edef|xdef|let|futurelet|special|directlua|pdf[a-zA-Z]*|everyjob|everypar|shipout|output|jobname|verbatiminput|lstinputlisting|includegraphics)(?![a-zA-Z])|\^\^")

(defn tex-error
  "TeX's first complaint as one short line, such as `Missing $ inserted, line 4`. Warnings are never
   the reason, and the log is kept beside it."
  [^String log]
  (let [line (some #(when (and (str/starts-with? % "error:") (not (str/includes? % "halted on potentially-recoverable"))) %)
                   (str/split-lines (str log)))
        said (some-> line (str/replace #"^error:\s*" "") str/trim)
        [_ at why] (when said (re-find #"^\S*t\.tex:(\d+):\s*(.*)$" said))
        reason (cond why (str why ", line " at)
                     said (str/replace said #"\S*/" "")
                     :else "TeX stopped, see its log")]
    (if (> (count reason) 120) (str (subs reason 0 117) "...") reason)))

(defn- pages-of
  "How many pages TeX said it wrote: only the first is shown."
  [^File log]
  (or (some-> (when (.exists log) (re-find #"Output written on .*\((\d+) page" (slurp log))) second parse-long) 1))

(defn- sha256 [^String s]
  (apply str (map #(format "%02x" %) (.digest (MessageDigest/getInstance "SHA-256") (.getBytes s "UTF-8")))))

(defn- compile-document!
  "tectonic then sips in the box, for one document. {:png bytes :width :height} in points, or {:error}."
  [world ^String doc]
  (let [job (job-dir world)
        tex (io/file job "t.tex")
        pdf (io/file job "t.pdf")
        png (io/file job "t.png")
        tectonic (renderer)]
    (try
      (spit tex doc)
      (let [{:keys [argv env]} (boxed world job [] [tectonic "--untrusted" "--only-cached" "--chatter" "minimal" "--keep-logs"
                                                    "--outdir" (.getPath job) (.getPath tex)])
            {:keys [exit log timed-out]} (run argv job env formula-seconds)]
        (cond
          timed-out {:error "the formula took too long"}
          (or (not (zero? (long exit))) (not (.exists pdf))) {:error (tex-error log) :log (tail (str log) 20000)}
          :else
          (let [size (boxed world job [] ["/usr/bin/sips" "-g" "pixelWidth" "-g" "pixelHeight" (.getPath pdf)])
                {:keys [log]} (run (:argv size) job (:env size) formula-seconds)
                number (fn [k] (some-> (re-find (re-pattern (str k ": ([0-9.]+)")) (str log)) second Double/parseDouble))
                w (number "pixelWidth") h (number "pixelHeight")]
            (if-not (and w h)
              {:error "could not read the formula's size"}
              ;; sips draws a PDF at its own size, so `--resampleWidth` enlarges a 72 dpi picture and
              ;; every edge goes soft. A second page holding the first, scaled, is drawn sharp.
              (let [scale (min (double raster-width-factor) (/ (double max-raster-width) w))
                    up (io/file job "u.tex")
                    big (io/file job "u.pdf")
                    _ (spit up (str "\\documentclass[border=0pt]{standalone}\n\\usepackage{graphicx}\n\\begin{document}\n"
                                    "\\includegraphics[scale=" (format "%.3f" scale) ",page=1]{t.pdf}\n\\end{document}\n"))
                    scaled (boxed world job [] [tectonic "--untrusted" "--only-cached" "--chatter" "minimal"
                                                "--outdir" (.getPath job) (.getPath up)])
                    _ (run (:argv scaled) job (:env scaled) formula-seconds)
                    r (boxed world job [] (if (.exists big)
                                            ["/usr/bin/sips" "-s" "format" "png" "--out" (.getPath png) (.getPath big)]
                                            ["/usr/bin/sips" "-s" "format" "png" "--resampleWidth" (str (long (* scale w)))
                                             "--out" (.getPath png) (.getPath pdf)]))]
                (run (:argv r) job (:env r) formula-seconds)
                (cond
                  (not (.exists png)) {:error "could not rasterize the formula"}
                  (> (.length png) max-rendered-bytes) {:error "the formula is too large"}
                  :else {:png (Files/readAllBytes (.toPath png)) :width w :height h
                         :pages (pages-of (io/file job "t.log"))}))))))
      (finally (doseq [^File x (reverse (file-seq job))] (.delete x))))))

(def ^:private memo-format "The form of a cached result: older answers are not this one's." "4")

(defn- memo-of ^File [world source]
  (io/file (tex-dir world) "rendered" (str (sha256 (str memo-format preamble (str/trim (str source)))) ".edn")))

(defn formula-png
  "The bytes of a rendered source, or nil."
  ^bytes [world ^String source]
  (let [memo (memo-of world source)]
    (when (.exists memo)
      (when-let [sha (:sha256 (read-string (slurp memo)))]
        (blob/get (:blobs world) sha)))))

(defn receipt
  "A rendered source's blob, for a pane on its first page, or nil."
  [world ^String source]
  (let [memo (memo-of world source)]
    (when (.exists memo)
      (when-let [sha (:sha256 (read-string (slurp memo)))]
        (when-let [^bytes bs (blob/get (:blobs world) sha)]
          {:blob/sha256 sha :blob/size (alength bs)})))))

(defn log-of
  "What TeX said about a source that failed, or nil."
  [world ^String source]
  (let [memo (memo-of world source)]
    (when (.exists memo) (:log (read-string (slurp memo))))))

(defn- compile-as
  "A source as what it is. A fragment read as math that TeX will not take is tried as text."
  [world kind ^String source]
  (let [first-try (compile-document! world (as-document kind source))]
    (if (and (:error first-try) (= :math kind))
      (let [second-try (compile-document! world (as-document :text source))]
        (if (:error second-try) first-try (assoc second-try :kind :text)))
      (assoc first-try :kind kind))))

(defn formula!
  "A source as an image, by hash of it: a formula, a text fragment, or a whole document, its first page.
   {:ok {:sha256 :width :height :pages :kind}}, with width and height in points, or {:err}. Cached:
   a result, or a TeX error, never compiles twice."
  [world ^String source]
  (let [source (str/trim (str source))
        kind (kind-of source)
        memo (memo-of world source)
        wall (case kind :math refused refused-in-a-document)
        stopped (when (<= (count source) max-source-chars) (re-find wall source))
        stopped (if (vector? stopped) (first stopped) stopped)]
    (cond
      (.exists memo)
      (let [{:keys [error] :as m} (read-string (slurp memo))]
        (if error (act/err :invalid error) {:ok (dissoc m :error :log)}))
      (> (count source) max-source-chars) (act/err :invalid (str "too long to typeset here, over " max-source-chars " characters"))
      stopped (act/err :invalid (str stopped " is not allowed here: TeX runs to typeset, not to read or run things"))
      (nil? (renderer)) (act/err :no-renderer "no TeX renderer on this host")
      (nil? (confine/primitive)) (act/err :unconfined "no sandbox on this host, so TeX does not run here")
      (not (.canExecute (io/file "/usr/bin/sips"))) (act/err :no-renderer "no rasterizer on this host")
      (not (prime! world)) (act/err :renderer-preparing "the TeX packages are being fetched, once")
      :else
      (if-not (.tryAcquire two-at-once 15 TimeUnit/SECONDS)
        (act/err :conflict "TeX is busy")
        (try
          (let [{:keys [png width height pages kind error log]} (compile-as world kind source)]
            (io/make-parents memo)
            (if error
              (do (spit memo (pr-str {:error error :log log})) (act/err :invalid error))
              (let [{:blob/keys [sha256]} (blob/put! (:blobs world) png)
                    m {:sha256 sha256 :width width :height height :pages pages :kind kind}]
                (spit memo (pr-str m))
                {:ok m})))
          (finally (.release two-at-once)))))))

;; ---------------------------------------------------------------- a document

(defn- png-of
  "The first page as a PNG, or nil where the host has no sips."
  [world ^File pdf ^File outdir]
  (let [png (io/file outdir "page.png")]
    (when (.canExecute (io/file "/usr/bin/sips"))
      (let [{:keys [argv env]} (boxed world outdir [] ["/usr/bin/sips" "-s" "format" "png" "--out" (.getPath png) (.getPath pdf)])]
        (run argv outdir env timeout-s))
      (when (.exists png) png))))

(defn latex!
  "Compile a .tex file on the host, in the box. {:ok {:pdf receipt :image receipt? :log}} or {:err}."
  [world ^File source]
  (if-let [tectonic (renderer)]
    (if-not (prime! world)
      (act/err :renderer-preparing "the TeX packages are being fetched, once: try again shortly")
    (let [outdir (job-dir world)
          dir (.getParentFile source)]
      (try
        (let [{:keys [argv env]} (boxed world outdir [(.getPath dir)]
                                        [tectonic "--untrusted" "--only-cached" "--outdir" (.getPath outdir) "--chatter" "minimal" (.getPath source)])
              {:keys [exit log]} (run argv dir env timeout-s)
              pdf (io/file outdir (str (base-name source) ".pdf"))]
          (if (and (zero? (long exit)) (.exists pdf))
            (let [png (png-of world pdf outdir)
                  put (fn [^File f] (blob/put! (:blobs world) (Files/readAllBytes (.toPath f))))]
              {:ok (cond-> {:pdf (put pdf) :log (tail log 2000)}
                     png (assoc :image (put png)))})
            (act/err :invalid (str "tectonic failed: " (tail log 1500)))))
        (finally (delete-tree! outdir)))))
    (act/err :no-renderer (str "no TeX renderer on this host: set KE_TECTONIC to the tectonic binary. "
                             "Show the source as a view of kind tex and the client renders the math."))))
