(ns knowledge.world.pane.render-test
  "A formula is untrusted input: it runs in the box, with nothing but its job directory to write."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.adapters.confine :as confine]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.pane.render :as render]
            [knowledge.world.ws-client :as ws]))

(def hard
  (str "\\mathfrak{O}_{\\!\\text{RBIS}}=\\oint_{\\partial\\mathcal W}\\!\\left(\\sum_{n=0}^{\\infty}"
       "\\frac{(-1)^n\\nabla^{2n}}{(2n+1)!}\\right)\\rtimes\\prod_{\\alpha<\\omega_1}\\mathbb G_\\alpha^{\\,\\aleph_0}"
       "\\;\\xrightarrow[\\text{audit}]{\\;\\;\\log^\\infty\\;\\;}\\;\\Omega^{\\Omega^{\\Omega}}"))

(defn- ready! [world]
  (loop [n 0]
    (cond (render/prime! world) true
          (> n 900) false
          :else (do (Thread/sleep 200) (recur (inc n))))))

(defn- real? [] (and (render/renderer) (confine/primitive) (.canExecute (io/file "/usr/bin/sips"))))

(deftest the-first-wall-refuses-what-a-formula-has-no-use-for
  (doseq [s ["\\input{/etc/hosts}" "\\immediate\\write18{touch x}" "\\openin1=x" "\\end{document}" "\\def\\a{b}"
             "\\usepackage{x}" "^^5cinput{x}" "\\special{x}" "\\csname input\\endcsname"]]
    (is (re-find render/refused s) s))
  (doseq [s ["\\frac{a}{b}" "\\text{audit}" "\\mathrm{Action}" "\\left(x\\right)" "\\letter" "\\define"]]
    (is (not (re-find render/refused s)) s)))

(deftest a-formula-becomes-an-image-and-is-not-compiled-twice
  (when (real?)
    (fx/with-world [{:keys [world]}]
      (is (ready! world) "packages primed")
      (let [{:keys [ok err]} (render/formula! world hard)]
        (is (nil? err) (pr-str err))
        (is (pos? (:width ok)))
        (is (> (alength (render/formula-png world hard)) 1000))
        (is (= ok (:ok (render/formula! world hard))) "the same answer"))
      (is (str/includes? (:message (:err (render/formula! world "\\nosuchcommand{x}"))) "Undefined") "a TeX error says so"))))

(deftest the-box-holds-with-the-first-wall-out-of-the-way
  (when (real?)
    (fx/with-world [{:keys [world] :as ctx}]
      (is (ready! world))
      (let [data (io/file (:data-dir world))
            token (.getPath (io/file data "root.token"))
            secret (slurp token)
            canary (io/file (:dir ctx) "canary")
            ;; what the wall would have refused goes straight to the compiler
            go (fn [body] (#'render/compile-document! world (str render/preamble body "\n\\end{document}\n")))]
        (testing "reading a token"
          (let [r (go (str "\\newread\\f\\openin\\f=" token " \\read\\f to\\x \\typeout{\\x}\\detokenize\\expandafter{\\x}"))]
            (is (not (str/includes? (str (:error r)) (str/trim secret))))))
        (testing "\\input of a token"
          (let [r (go (str "\\input{" token "}"))]
            (is (:error r) "no such file, from inside the box")
            (is (not (str/includes? (str (:error r)) (str/trim secret))))))
        (testing "running a command"
          (go (str "\\immediate\\write18{touch " (.getPath canary) "}x"))
          (is (not (.exists canary))))
        (testing "writing outside the job"
          (go (str "\\newwrite\\f\\immediate\\openout\\f=" (.getPath canary) " \\immediate\\write\\f{x}\\immediate\\closeout\\f x"))
          (is (not (.exists canary))))))))

(deftest only-a-persons-session-asks
  (fx/with-world [{:keys [root]}]
    (let [r (ws/request!! root :act {:action :tex.render :args {:source "x"}})]
      (is (false? (:ok r)) (pr-str r)))))

(deftest a-source-is-a-formula-a-fragment-or-a-document
  (is (= :math (render/kind-of hard)))
  (is (= :math (render/kind-of "\\text{a word or two here} = 1")))
  (is (= :text (render/kind-of "\\section*{The grid Project}\nResearch on \\textbf{GRiD}")))
  (is (= :text (render/kind-of "Some words that are plainly a sentence.")))
  (is (= :document (render/kind-of "\\documentclass{article}\n\\begin{document}\nHi\n\\end{document}")))
  (is (= :document (render/kind-of "\\begin{document}\nHi\n\\end{document}"))))

(def grid-document
  (str "\\documentclass{article}\n\\usepackage[margin=1in]{geometry}\n\\begin{document}\n"
       "\\section*{The \\texttt{grid} Project}\n\\subsection*{Scope}\n"
       "Research on \\textbf{GRiD Systems Corporation} and the \\textbf{GRiD Compass}.\n"
       "\\begin{itemize}\\item Floppy dumps in \\texttt{gridsources/}\\end{itemize}\n"
       "\\newpage Second page.\n\\end{document}\n"))

(deftest a-whole-document-and-a-fragment-are-typeset
  (when (real?)
    (fx/with-world [{:keys [world]}]
      (is (ready! world) "packages primed")
      (testing "a document compiles as it is, and says how long it is"
        (let [{:keys [ok err]} (render/formula! world grid-document)]
          (is (nil? err) (pr-str err))
          (is (= :document (:kind ok)))
          (is (= 2 (:pages ok)))
          (is (> (:height ok) (:width ok)) "a page")
          (is (:blob/sha256 (render/receipt world grid-document)))))
      (testing "a text fragment"
        (let [{:keys [ok err]} (render/formula! world "\\section*{Scope}\nResearch on \\textbf{GRiD}.\n\\begin{itemize}\\item one\\end{itemize}")]
          (is (nil? err) (pr-str err))
          (is (= :text (:kind ok)))))
      (testing "a small diagram, and a commutative one, and a plot"
        (doseq [source ["\\begin{tikzpicture}[>=Stealth]\n\\node[draw] (a) {world};\n\\node[draw,right=of a] (b) {client};\n\\draw[->] (a) -- (b);\n\\end{tikzpicture}"
                        "\\begin{tikzcd} A \\arrow[r, \"f\"] \\arrow[d] & B \\arrow[d] \\\\ C \\arrow[r] & D \\end{tikzcd}"
                        "\\begin{tikzpicture}\\begin{axis}\\addplot coordinates {(0,0)(1,2)(2,3)};\\end{axis}\\end{tikzpicture}"]]
          (let [{:keys [ok err]} (render/formula! world source)]
            (is (nil? err) (pr-str source err))
            (is (= :text (:kind ok))))))
      (testing "a diagram wider than a chat keeps its whole width, and its image is sharp at it"
        (let [source "\\begin{tikzpicture}\n\\node[draw] (a) at (0,0) {left};\n\\node[draw] (b) at (30,0) {right};\n\\draw (a) -- (b);\n\\end{tikzpicture}"
              {:keys [ok err]} (render/formula! world source)
              png (render/formula-png world source)
              image (javax.imageio.ImageIO/read (java.io.ByteArrayInputStream. png))]
          (is (nil? err) (pr-str err))
          (is (> (:width ok) 840) "30 cm of picture, not 34em of it")
          (is (>= (.getWidth image) (* 3.9 (:width ok))) "drawn at the size it is shown enlarged to")))
      (testing "a document may not read files, and says what stopped it"
        (let [{:keys [err]} (render/formula! world "\\documentclass{article}\\begin{document}\\input{/etc/hosts}\\end{document}")]
          (is (str/includes? (:message err) "\\input"))))
      (testing "a TeX error reads as one line, with no job path"
        (let [{:keys [err]} (render/formula! world "\\documentclass{article}\\begin{document}\\nosuchcommand\\end{document}")]
          (is (str/includes? (:message err) "Undefined control sequence") (pr-str err))
          (is (re-find #", line \d+" (:message err)) (pr-str err))
          (is (<= (count (:message err)) 120))
          (is (not (str/includes? (:message err) "/tex/job")) (pr-str err))
          (is (seq (render/log-of world "\\documentclass{article}\\begin{document}\\nosuchcommand\\end{document}"))
              "the whole log is kept"))))))

(deftest a-reason-is-one-line
  (is (= "Missing $ inserted, line 4"
         (render/tex-error (str "warning: accessing absolute path `/dev/null`; build may not be reproducible\n"
                                "warning: more\nerror: /tmp/tex/job/x/t.tex:4: Missing $ inserted\n"
                                "error: halted on potentially-recoverable error as specified\n"))))
  (is (= "TeX stopped, see its log" (render/tex-error "warning: only warnings\nnote: and notes\n"))))
