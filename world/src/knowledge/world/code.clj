(ns knowledge.world.code
  "Working in a Principal's mounted files, without a shell (decisions 2026-10-31 'code, not a shell'):
   code.search finds lines, code.outline shows a file's shape, file.edit changes one exact piece of a
   file. Measured: open questions spent their turns on grep flags that found nothing, and every edit
   fought `sed` and `printf` escaping. Each answer here says where to look next, so a model has no
   need to guess.

   Reach is the mount's: a Grant covering the mount name or its Adapter (spec 6.5), for :read to
   search and outline and :write to edit. One `resolved` event records each search, as search does."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [jsonista.core]
            [knowledge.world.act.core :as act]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.resolve.mount :as mount])
  (:import [java.io File]
           [java.util Date]
           [java.util.regex Pattern PatternSyntaxException]))

(def default-limit 10)
(def max-limit 40)
(def lines-per-file 3)
(def line-chars 140)
(def max-file-bytes (* 2 1024 1024))
(def max-files 40000)
(def search-ms 8000)
(def outline-limit 80)
(def diff-chars 1024)

(def skip-dirs
  "What nobody means when they say the code."
  #{".git" ".hg" ".svn" "node_modules" "build" "target" "dist" "out" ".build" ".cache" ".cpcache"
    "DerivedData" "__pycache__" ".venv" "venv" ".gradle" ".idea" ".vscode"})

(def ^:private source-exts
  #{"c" "h" "cc" "cpp" "cxx" "hpp" "hh" "m" "mm" "swift" "rs" "go" "py" "js" "ts" "tsx" "jsx" "java" "kt"
    "clj" "cljc" "cljs" "rb" "lisp" "lua" "sh" "zsh" "asm" "s"})
(def ^:private doc-exts #{"md" "txt" "rst" "adoc" "tex"})
(def ^:private aside-re #"(?i)(^|/)(vendor|third_party|thirdparty|deps|external|extern|fixtures)(/|$)")

(defn- ext-of [^String rel]
  (let [n (subs rel (inc (.lastIndexOf rel "/")))
        i (.lastIndexOf n ".")]
    (when (pos? i) (str/lower-case (subs n (inc i))))))

;; ---------------------------------------------------------------- mounts

(defn- namespaces [db p] (resolve/namespace-chain db p))

(defn- fs-adapters [db p]
  (filter #(= :filesystem (get-in (objects/fields db %) [:adapter/binds :field/value]))
          (get-in (objects/fields db p) [:principal/adapters :field/value])))

(defn- label-of
  "The mount name that reaches this Adapter for p, so a path here is one `read` accepts."
  [db p adapter]
  (let [nss (namespaces db p)
        bound (map second (objects/names db adapter))
        short (map #(str/replace % #"\.fs$" "") bound)]
    (some (fn [n] (when (= adapter (:adapter (mount/mount-of db nss (str n "/x")))) n))
          (distinct (concat short bound)))))

(defn- reachable? [db p {:keys [adapter root]} op]
  (seq (resolve/grants-for db p adapter (str root "/") op (Date.))))

(defn- own-mounts
  "p's mounts it may read, its own first: {:adapter :root :locator}. Every filesystem Adapter is
   looked at, not only p's: root, asked about two projects, said it had no access path to either."
  [db p]
  (distinct
   (for [a (distinct (concat (fs-adapters db p)
                             (filter #(= :filesystem (get-in (objects/fields db %) [:adapter/binds :field/value]))
                                     (objects/objects-of-class db :adapter))))
         :let [root (label-of db p a)
               m (when root (mount/mount-of db (namespaces db p) (str root "/x")))]
         :when (and m (reachable? db p m :read))]
     {:adapter a :root root :locator (:locator m)})))

(defn- under
  "Where to look for a path: `<mount>/<rel>`, a mount alone, or a path inside p's mounts."
  [db p path]
  (let [mine (own-mounts db p)
        path (str/replace (str path) #"^/+|/+$" "")
        first-seg (first (str/split path #"/"))
        named (when (seq path)
                (or (first (filter #(= (:root %) first-seg) mine))
                    (when-let [m (mount/mount-of db (namespaces db p) (str first-seg "/x"))]
                      (when (reachable? db p m :read) m))))]
    (cond
      (str/blank? path) (map #(assoc % :rel "") mine)
      named [(assoc named :rel (str/replace (subs path (count first-seg)) #"^/+" "") :root (or (:root named) first-seg))]
      :else (map #(assoc % :rel path) mine))))

;; ---------------------------------------------------------------- search

(defn- regexish? [^String s] (re-find #"[\\\[\]()|^$*+?{}]" s))

(defn- regex-query
  "A pattern as a regular expression, or nil when it is not one."
  [^String q]
  (try {:phrase (Pattern/compile q Pattern/CASE_INSENSITIVE) :words []}
       (catch PatternSyntaxException _ nil)))

(defn- literal-words [^String s] (vec (remove str/blank? (str/split s #"[\s_\-]+"))))

(defn- phrase-pattern
  "Words as one thing however it is written: `boot sector` is BootSector, boot_sector, bootsector."
  ^Pattern [words]
  (Pattern/compile (str/join "[\\s_\\-]*" (map #(Pattern/quote %) words)) Pattern/CASE_INSENSITIVE))

(defn- compile-query
  "{:phrase Pattern :words [Pattern]}. Literal by default, `read {object` is those characters; regex?
   asks for a regular expression, and is nil when q is not one."
  [^String q regex?]
  (if regex?
    (regex-query q)
    (let [ws (literal-words q)]
      {:phrase (phrase-pattern ws)
       :words (when (> (count ws) 1) (mapv #(phrase-pattern [%]) (sort-by (comp - count) ws)))})))

(defn- glob-pattern ^Pattern [^String glob]
  (Pattern/compile (str "(?i)" (-> glob Pattern/quote (str/replace "*" "\\E.*\\Q") (str/replace "?" "\\E.\\Q")))))

(defn- text-of
  "The file's text, or nil for a binary or an oversized file."
  [^File f]
  (when (<= (.length f) max-file-bytes)
    (let [bs (java.nio.file.Files/readAllBytes (.toPath f))
          n (min (alength bs) 8192)]
      (when (loop [i 0] (cond (= i n) true (zero? (aget bs i)) false :else (recur (inc i))))
        (String. bs "UTF-8")))))

(defn- walk
  "The regular files under dir, nearest first, without the skip-dirs; stops at the budget."
  [^File dir budget]
  (let [out (transient [])]
    (loop [queue (conj clojure.lang.PersistentQueue/EMPTY dir)]
      (when-let [^File d (peek queue)]
        (when (pos? @budget)
          (let [kids (sort-by #(.getName ^File %) (or (seq (.listFiles d)) []))
                files (filter #(.isFile ^File %) kids)
                dirs (remove #(or (not (.isDirectory ^File %)) (skip-dirs (.getName ^File %))) kids)]
            (doseq [f files] (conj! out f) (swap! budget dec))
            (recur (into (pop queue) dirs))))))
    (persistent! out)))

(defn- rel-of [^File root ^File f]
  (let [r (.getPath (.getCanonicalFile root)) p (.getPath (.getCanonicalFile f))]
    (if (= r p) "" (subs p (inc (count r))))))

(defn- clip [^String s]
  (let [t (str/trim s)] (if (> (count t) line-chars) (str (subs t 0 line-chars) "...") t)))

(defn- scan-file
  "The file's matches, or nil: {:path :hits :lines [{:line :text}]}."
  [^String text ^String shown patterns]
  (let [lines (str/split-lines text)
        seen (volatile! 0)
        picked (volatile! [])]
    (dotimes [i (count lines)]
      (let [^String line (nth lines i)]
        (when (some #(.find (.matcher ^Pattern % line)) patterns)
          (vswap! seen inc)
          (when (< (count @picked) lines-per-file)
            (vswap! picked conj {:line (inc i) :text (clip line)})))))
    (when (pos? @seen) {:path shown :hits @seen :lines @picked})))

(defn- weight
  "Where a person looks first: source, then documents, then anything else, others' code last. A
   match in a file's own name outranks a match inside one."
  [{:keys [path hits]} ^Pattern phrase]
  (+ (if (.find (.matcher phrase (str path))) 8 0)
     (cond (source-exts (ext-of path)) 4 (doc-exts (ext-of path)) 2 :else 0)
     (if (re-find aside-re path) -6 0)
     (min 3 (quot (long hits) 3))))

(defn- scan
  "Matches of patterns under the places, best first. {:found [...] :files n :cut? bool}."
  [places patterns ^Pattern rank glob]
  (let [deadline (+ (System/currentTimeMillis) search-ms)
        budget (atom max-files)
        found (volatile! [])
        files (volatile! 0)
        cut? (volatile! false)
        glob (some-> glob glob-pattern)]
    (doseq [{:keys [root locator rel]} places
            :let [^File base (fs/safe-file locator (str rel))]
            :when (and base (.exists base))
            ^File f (if (.isDirectory base) (walk base budget) [base])
            :let [r (rel-of (fs/safe-file locator "") f)]
            :when (or (nil? glob) (.find (.matcher ^Pattern glob r)))]
      (if (> (System/currentTimeMillis) deadline)
        (vreset! cut? true)
        (when-let [t (text-of f)]
          (vswap! files inc)
          (when-let [m (scan-file t (str root "/" r) patterns)]
            (vswap! found conj m)))))
    {:found (vec (sort-by (fn [m] [(- (weight m rank)) (:path m)]) @found))
     :files @files :cut? (or @cut? (<= @budget 0))}))

(defn- next-read
  "The read that follows from a hit, so the next turn is a read rather than another search."
  [{:keys [path lines]}]
  (let [l (long (or (:line (first lines)) 1))]
    {:read {:name path :range [(max 1 (- l 20)) (+ l 60)]}}))

(defn search!
  "{pattern path? glob? limit?}: lines under p's mounts. A plain phrase matches however it is
   spelled; when no line has it whole, lines with any of its words stand in, and :matched says so."
  [world p {:keys [pattern path glob limit regex]}]
  (let [db (act/db world)
        q (str/trim (str pattern))
        compiled (when (seq q) (compile-query q regex))
        places (under db p path)]
    (cond
      (str/blank? q) (act/err :invalid-args "code.search needs a pattern")
      (nil? compiled) (act/err :invalid-args (str "not a regular expression: " q ". Leave `regex` off to search for it literally."))
      (empty? places) (act/err :unresolved "no mount to search: you have none you can read here")
      :else
      (let [{:keys [phrase words]} compiled
            whole (scan places [phrase] phrase glob)
            partial (when (and (empty? (:found whole)) (seq words)) (scan places words phrase glob))
            ;; literal first; what only makes sense as a pattern, such as boot.*sector, still finds it
            as-regex (when (and (not regex) (empty? (:found whole)) (empty? (:found partial)) (regexish? q))
                       (when-let [{p2 :phrase} (regex-query q)]
                         (let [r (scan places [p2] p2 glob)] (when (seq (:found r)) r))))
            {:keys [found files cut?]} (or as-regex partial whole)
            n (min max-limit (max 1 (long (or limit default-limit))))
            shown (vec (take n found))]
        (act/append! world (log/pending :resolved p p {:name (str "code.search:" (subs q 0 (min 80 (count q))))
                                                       :op :read :count (count found)}))
        {:ok (cond-> {:pattern q
                      :searched (mapv #(str (:root %) "/" (:rel %)) places)
                      :matched (cond as-regex "as a regular expression, nothing matched it literally"
                                     partial "any word, none had the whole phrase"
                                     (seq found) "whole"
                                     :else "nothing")
                      :files-read files :files-matching (count found) :results shown}
               (seq shown) (assoc :next (next-read (first shown)))
               (> (count found) n) (assoc :more (- (count found) n))
               cut? (assoc :truncated true))}))))

;; ---------------------------------------------------------------- outline

(def ^:private def-re
  #"^\s*(?:(?:public|private|protected|internal|static|inline|virtual|export|async|final|override|abstract|extern|pub|mutating|fileprivate|open)\s+)*(?:class|struct|enum|union|namespace|interface|protocol|extension|func|def|fn|function|defn-?|defmacro|defmethod|defmulti|defprotocol|defrecord|deftype|defonce|type|typedef|impl|trait|module|macro_rules!)\b.*")

(def ^:private c-function-re
  #"^[A-Za-z_][\w:<>,\s\*&~\[\]]*\s[\*&]?[A-Za-z_~][\w:~]*\s*\([^;]*$")

(def ^:private heading-re #"^#{1,6}\s+\S.*")
(def ^:private tex-heading-re #"^\\(?:part|chapter|(?:sub){0,2}section)\*?\{.*")

(defn- outline-line? [^String ext ^String line]
  (cond
    (= "md" ext) (re-matches heading-re line)
    (= "tex" ext) (re-matches tex-heading-re line)
    :else (or (re-matches def-re line) (re-matches c-function-re line))))

(defn- outline-file [text ext shown lines-total]
  (let [rows (keep-indexed (fn [i line] (when (outline-line? ext line) {:line (inc i) :text (clip line)}))
                           (str/split-lines text))]
    (cond-> {:name shown :lines lines-total :outline (vec (take outline-limit rows))}
      (> (count rows) outline-limit) (assoc :more (- (count rows) outline-limit))
      (empty? rows) (assoc :note "nothing recognised: read it by range"))))

(defn- outline-dir
  "A directory: its entries and how many files each folder holds, so one call shows the layout."
  [^File dir shown]
  (let [kids (sort-by #(.getName ^File %) (or (seq (.listFiles dir)) []))
        dirs (remove #(or (not (.isDirectory ^File %)) (skip-dirs (.getName ^File %))) kids)
        files (filter #(.isFile ^File %) kids)]
    {:name shown
     :dirs (vec (for [^File d (take 60 dirs)] {:name (str (.getName d) "/") :files (count (walk d (atom 2000)))}))
     :files (vec (take 60 (map #(.getName ^File %) files)))}))

(def ^:private nothing-here "no such path")

(defn outline!
  "{name?}: the shape of a file, or of a directory; no name is every mount."
  [world p {:keys [name]}]
  (let [db (act/db world)
        places (under db p name)]
    (if (empty? places)
      (act/err :unresolved (str "no such mount or path: " name))
      {:ok (let [rows (for [{:keys [root locator rel]} places
                            :let [^File f (fs/safe-file locator (str rel))
                                  shown (str/replace (str root "/" rel) #"/$" "")]
                            :when (and f (.exists f))]
                        (if (.isDirectory f)
                          (outline-dir f shown)
                          (if-let [t (text-of f)]
                            (outline-file t (ext-of rel) shown (count (str/split-lines t)))
                            {:name shown :note "binary or over 2 MiB"})))]
             (cond
               ;; `mounts: []` read as having no mounts at all (seen live): say what is there
               (empty? rows) {:found nothing-here :mounts (mapv :root (own-mounts db p))
                              :next "this is not under any mount. Outline one of these, or a directory someone named is add_project"}
               (= 1 (count rows)) (first rows)
               ;; every mount at once: names only, or the first mount's detail is all a clip leaves
               :else {:mounts (mapv (fn [{:keys [name dirs files]}]
                                      {:name name :dirs (vec (take 24 (map :name dirs))) :files (count files)})
                                    rows)
                      :next "outline or read one by name"}))})))

;; ---------------------------------------------------------------- where a path is

(def ^:private scan-budget "Files looked at for a close match." 4000)

(defn- own
  "The mounts on p itself, so a bare path means p's own project and not any it may read."
  [db p]
  (let [mine (set (fs-adapters db p))] (filter #(mine (:adapter %)) (own-mounts db p))))

(defn- under-mount
  "`rel` in mount m as a File, or nil when it escapes."
  ^File [{:keys [locator]} rel] (fs/safe-file locator rel))

(def ^:private fence-re #"(?s)```.*?(?:```|\z)")
(def ^:private path-re
  "A path, relative or absolute, with a line or a range. Nothing in a URI: what follows `/` or `:` is not a start."
  #"(?<![\w./:@~\-])(/?[\w.\-]+(?:/[\w.\-]+)*)(?::(\d{1,7})(?:-\d{1,7})?)?(?![\w/@])")
(def ^:private max-cited "Candidates looked up per Message." 64)

(defn- where-cited
  "The mount holding the regular file a Principal named, {:adapter :rel}, or nil. Relative to p's own
   mounts, its working directories; only a Principal with none looks in what else it can read."
  [db p ^String n]
  (let [mounts (let [mine (own db p)] (if (seq mine) mine (own-mounts db p)))
        file? (fn [m rel] (when-let [^File f (under-mount m rel)] (when (.isFile f) {:adapter (:adapter m) :rel rel})))]
    (if (str/starts-with? n "/")
      (let [real (.getCanonicalPath (io/file n))]
        (some (fn [{:keys [locator] :as m}]
                (let [home (.getCanonicalPath (fs/safe-file locator "."))]
                  (when (str/starts-with? real (str home "/")) (file? m (subs real (inc (count home)))))))
              mounts))
      (or (some (fn [{:keys [root] :as m}]
                  (when (str/starts-with? n (str root "/")) (file? m (subs n (inc (count (str root)))))))
                mounts)
          (some #(file? % n) mounts)))))

(defn cited-paths
  "Spans of `text` naming files that exist where p works: {:start :end :kind :path :object :path :line?},
   in code points. A span is a fact about the disk when p spoke, never a reading of how the text looks:
   `example.com:443` is no file, so it is no link. Fenced code is left alone."
  [db p ^String text]
  (let [fences (let [m (re-matcher fence-re text)]
                 (loop [out []] (if (.find m) (recur (conj out [(.start m) (.end m)])) out)))
        fenced? (fn [^long i] (some (fn [[a b]] (and (<= a i) (< i b))) fences))
        m (re-matcher path-re text)]
    (loop [out [] looked 0]
      (if (and (< looked max-cited) (.find m))
        (let [line (.group m 2)
              raw (.group m 1)
              ;; a sentence's full stop is not part of a name
              n (if line raw (str/replace raw #"[.\-]+$" ""))
              end (if line (.end m) (+ (.start m) (count n)))]
          (if (or (fenced? (.start m)) (not (re-find #"[A-Za-z]" n)) (not (or line (re-find #"[./]" n))))
            (recur out looked)
            (if-let [{:keys [adapter rel]} (where-cited db p n)]
              (recur (conj out (cond-> {:start (.codePointCount text 0 (.start m)) :end (.codePointCount text 0 (int end))
                                        :kind :path :object adapter :path rel}
                                 line (assoc :line (parse-long line))))
                     (inc looked))
              (recur out (inc looked)))))
        out))))

(defn- close-matches
  "Up to 4 paths in p's mounts nearest the one that was named: the same file name, else one sharing
   its stem. A path that is nearly right is the fastest way to the right one."
  [db p n]
  (let [base (last (str/split (str n) #"/"))
        stem (str/replace (str/lower-case base) #"\.[^.]*$" "")
        rows (for [{:keys [root locator]} (own db p)
                   :let [dir (io/file locator)]
                   ^File f (walk dir (atom scan-budget))]
               {:name (.getName f) :path (str root "/" (rel-of dir f))})]
    (vec (take 4 (concat (filter #(= base (:name %)) rows)
                         (when (>= (count stem) 3)
                           (filter #(and (not= base (:name %)) (str/includes? (str/lower-case (:name %)) stem)) rows)))))))

(defn- unresolved-path
  "The refusal for a path in no mount: the exact form, so the next op can be right."
  [db p n]
  (let [roots (map :root (own db p))
        close (map :path (close-matches db p n))]
    (act/err :unresolved
             (str "does not resolve: `" n "` is under none of your mounts"
                  (when (seq roots) (str " (" (str/join ", " (map #(str "`" % "`") roots)) ")"))
                  ". A path starts with its mount's name"
                  (when-let [r (when-not (str/starts-with? (str n) "/") (first roots))] (str ", such as `" r "/" n "`")) "."
                  (when (seq close) (str " Close matches: " (str/join ", " (map #(str "`" % "`") close)) "."))))))

(defn path-hint
  "What read-hint adds about a path that resolves to nothing, or nil."
  [db p n]
  (get-in (unresolved-path db p n) [:err :message]))

(defn- locate
  "The mount-qualified form of a path p named: {:name}, or {:err} saying what to write instead. A
   bare path under one of p's own mounts, or an absolute one, is that file: 22 approved edits in one
   run named `world/test/...` and every one came back `does not resolve` (benchmark, Part B). A new
   file's directory must be there: `memory/notes`, meant as a Field, landed in a project as a file."
  [db p n fresh?]
  (let [n (str n)
        ;; the Adapter's name for the mount's, and the two of them in a row: both seen in every run
        n (reduce (fn [s {:keys [root]}]
                    (str/replace s (re-pattern (str "^" (java.util.regex.Pattern/quote (str root ".fs/"))
                                                    "(?:" (java.util.regex.Pattern/quote (str root "/")) ")?"))
                                 (java.util.regex.Matcher/quoteReplacement (str root "/"))))
                  n (when (str/includes? n ".fs/") (own db p)))]
    (if (mount/mount-of db (namespaces db p) n)
      {:name n}
      (let [mine (own db p)
            abs (when (str/starts-with? n "/")
                  (let [real (.getCanonicalPath (io/file n))]
                    (some (fn [{:keys [root locator]}]
                            (let [home (.getCanonicalPath (io/file locator))]
                              (when (str/starts-with? real (str home "/")) (str root "/" (subs real (inc (count home)))))))
                          mine)))
            there? (fn [m] (when-let [^File f (under-mount m n)]
                             (or (.exists f) (and fresh? (some-> (.getParentFile f) .isDirectory)))))
            hits (when-not (str/starts-with? n "/") (filter there? mine))]
        (cond
          abs {:name abs}
          (str/starts-with? n "/") (unresolved-path db p n)
          (= 1 (count hits)) {:name (str (:root (first hits)) "/" n)}
          (seq hits) (act/err :unresolved (str "ambiguous: `" n "` is in " (str/join " and " (map #(str "`" (:root %) "/" n "`") hits))
                                               ". Name the mount."))
          :else (unresolved-path db p n))))))

(defn qualified
  "An op's args with a file path in its mount-qualified form, so the card and the log say which file.
   What does not resolve is left as written, for the action to refuse."
  [db p op args]
  (let [a (if (map? (:args args)) (:args args) {})
        action (some-> (:action args) name)]
    (cond
      (and (= "act" (name op)) (= "file.edit" action) (string? (:name a)))
      (if-let [n (:name (locate db p (:name a) (and (some? (:new a)) (not (seq (str (:old a)))))))]
        (assoc-in args [:args :name] n)
        args)

      (and (= "read" (name op)) (string? (:name args)) (not (resolve/lookup db p (:name args))))
      (if-let [n (:name (locate db p (:name args) false))] (assoc args :name n) args)

      :else args)))

;; ---------------------------------------------------------------- edit

(defn- occurrences [^String text ^String old]
  (loop [at 0 found []]
    (let [i (.indexOf text old (int at))]
      (if (neg? i) found (recur (+ i (max 1 (count old))) (conj found i))))))

(defn- line-at [^String text ^long i]
  (inc (count (filter #(= \newline %) (subs text 0 i)))))

(defn- nearest
  "Where the text nearly is: the first line sharing the old text's first line, trimmed, for the
   refusal to point at."
  [^String text ^String old]
  (let [wanted (str/trim (first (str/split-lines old)))
        rows (str/split-lines text)]
    (when (seq wanted)
      (some (fn [[i ^String l]] (when (str/includes? (str/lower-case l) (str/lower-case wanted)) {:line (inc i) :text (clip l)}))
            (map-indexed vector rows)))))

(defn- diff-of
  "A short unified view: the lines that changed, and where."
  [^String before ^String after]
  (let [a (str/split-lines before) b (str/split-lines after)
        head (count (take-while true? (map = a b)))
        tail (count (take-while true? (map = (reverse (drop head a)) (reverse (drop head b)))))
        gone (subvec (vec a) head (- (count a) tail))
        came (subvec (vec b) head (- (count b) tail))
        body (str "@@ line " (inc head) " @@\n"
                  (str/join "\n" (concat (map #(str "-" %) gone) (map #(str "+" %) came))))]
    (if (> (count body) diff-chars) (str (subs body 0 diff-chars) "\n[... diff cut]") body)))

(defn- changed
  "The new text, or {:err}. old is exact; `at` top or end inserts new as whole lines."
  [^String text {:keys [old new at all]}]
  (let [new (str new)]
    (cond
      ;; `at` wins over an `old` sent beside it: seen live, the heading it named was replaced, not kept
      ;; `new` written with the line it goes above: the line is there already, so it is not doubled
      (= "top" (some-> at name))
      (let [line (str/replace new #"\n+$" "")
            kept (or (not-empty (str/trim (str old))) (first (str/split-lines text)))
            line (if (and (seq kept) (str/ends-with? line kept))
                   (str/replace (subs line 0 (- (count line) (count kept))) #"\n+$" "")
                   line)]
        {:text (if (str/blank? line) text (str line "\n" text))})
      (= "end" (some-> at name))
      {:text (str text (when-not (or (empty? text) (str/ends-with? text "\n")) "\n") (str/replace new #"\n+$" "") "\n")}
      (str/blank? (str old)) (act/err :invalid-args "this file exists, and `new` alone makes a new file. To add to it, give `at` end; to change it, give `old`, the exact text to replace; or `at` top or end")
      :else
      (let [hits (occurrences text old)]
        (cond
          (empty? hits)
          (act/err :no-match (str "that exact text is not in the file; nothing changed."
                                  (when-let [{:keys [line] t :text} (nearest text old)]
                                    (str " Nearest, line " line ": " t))))
          (and (> (count hits) 1) (not all))
          (act/err :ambiguous (str (count hits) " places match, at lines "
                                   (str/join ", " (take 8 (map #(line-at text %) hits)))
                                   "; nothing changed. Give more of the surrounding text, or all true."))
          :else {:text (str/replace text old new)})))))

;; ---------------------------------------------------------------- how a project is tested

(defn- json-names [^String text k]
  (try (mapv #(get % "name") (get (jsonista.core/read-value text) k)) (catch Exception _ [])))

(defn test-command
  "The command that takes this project from nothing to test results, from its own build files, or
   nil. Given to the Principal with its Adapters: asked to run the tests, a model read the build
   files for eight turns, or configured and stopped; never once did it reach a result."
  [^String dir]
  (let [has? (fn [n] (.exists (io/file dir n)))
        presets (when (has? "CMakePresets.json")
                  (let [t (slurp (io/file dir "CMakePresets.json"))]
                    (first (filter (set (json-names t "testPresets")) ["debug" "default" "release"]))))]
    (cond
      presets (str "cmake --preset " presets " && cmake --build --preset " presets " && ctest --preset " presets)
      (has? "CMakeLists.txt") "cmake -S . -B build && cmake --build build && ctest --test-dir build"
      (has? "Cargo.toml") "cargo test"
      (has? "Makefile") "make test"
      (has? "package.json") "npm test"
      (has? "pyproject.toml") "pytest"
      (has? "go.mod") "go test ./..."
      (has? "deps.edn") "clojure -M:test"
      (has? "gradlew") "./gradlew test"
      (has? "pom.xml") "mvn test"
      (has? "tests/run.sh") "sh tests/run.sh"
      (has? "test.sh") "sh test.sh"
      :else nil)))

(defn- write-file!
  "The whole file or none of it."
  [^File f ^String text]
  (.mkdirs (.getParentFile f))
  (let [tmp (File. (.getParentFile f) (str "." (.getName f) ".orbis-" (System/nanoTime)))]
    (spit tmp text)
    (java.nio.file.Files/move (.toPath tmp) (.toPath f)
                              (into-array java.nio.file.CopyOption [java.nio.file.StandardCopyOption/ATOMIC_MOVE
                                                                    java.nio.file.StandardCopyOption/REPLACE_EXISTING]))))

(defn current-text
  "The file named as an edit named it, as it stands: {:name :text :lines}, or nil when it cannot be
   read as text. The work log takes this after an edit, so the next `old` is copied, not guessed."
  [db p name]
  (let [{located :name} (locate db p name false)
        m (when located (mount/mount-of db (namespaces db p) located))
        ^File f (when m (fs/safe-file (:locator m) (:rel m)))]
    (when (and f (.isFile f))
      (when-let [text (text-of f)]
        {:name located :text text :lines (count (str/split-lines text))}))))

(defn- planned
  "What this edit would do, written or not: {:file :name :before :text :created?}, or {:err} with why
   it cannot land. The gate asks this first, so a person never approves an edit that cannot happen."
  [db p {:keys [name old new at] :as args}]
  (let [fresh? (and (some? new) (not (seq (str old))))
        {located :name :keys [err]} (locate db p name fresh?)
        m (when located (mount/mount-of db (namespaces db p) located))
        ^File f (when m (fs/safe-file (:locator m) (:rel m)))]
    (cond
      err {:err err}
      (or (nil? m) (nil? f) (not (reachable? db p m :write))) (unresolved-path db p name)
      (get-in (objects/fields db (:adapter m)) [:adapter/read-only :field/value])
      (act/err :not-allowed (if (str/starts-with? (str located) "harness/")
                              (str "harness is the running release, read-only. Your working copy is the mount `src`: edit `src/"
                                   (subs (str located) (count "harness/")) "`, build and test there, commit, then release.make and world.restart.")
                              "this mount is read-only"))
      (.isDirectory f) (act/err :invalid-args "that is a directory")
      (and (.exists f) (nil? (text-of f))) (act/err :invalid-args "binary or over 2 MiB: not editable here")
      (and (not (.exists f)) (not fresh?))
      (let [close (map :path (close-matches db p located))]
        (act/err :no-match (str "no such file: " located "; to create it give only `new`"
                                (when (seq close) (str ". Close matches: " (str/join ", " (map #(str "`" % "`") close)))))))
      :else
      (let [before (if (.exists f) (slurp f) "")
            created? (not (.exists f))
            {:keys [text err]} (if created? {:text (str new)} (changed before args))]
        (cond
          err {:err err}
          (= text before) (act/err :no-change "the new text equals the old; nothing changed")
          :else {:file f :name located :before before :text text :created? created?})))))

;; ---------------------------------------------------------------- names an edit introduces

(def ^:private cpp-exts #{"c" "cc" "cpp" "cxx" "h" "hh" "hpp" "hxx" "mm"})
(def ^:private cpp-name-re #"\b([A-Za-z_]\w*)((?:::[A-Za-z_]\w*)+)(\s*\()?")
(def ^:private names-checked "Names looked up per edit." 4)
(def ^:private names-ms 3000)

(defn- introduced
  "Qualified names `new` brings to a C or C++ file that neither `old` nor the file had:
   [{:name :qualifier :segment :call?}]. Every benchmark round had one: `log::debug` written
   against a `Log.hpp` that was never read."
  [ext ^String new ^String old ^String before]
  (when (cpp-exts ext)
    (let [had (str old "\n" before)]
      (->> (re-seq cpp-name-re (str new))
           (map (fn [[_ head tail call]]
                  (let [segs (rest (str/split tail #"::"))]
                    {:name (str head tail) :qualifier (str/join "::" (cons head (butlast segs)))
                     :segment (last segs) :call? (some? call)})))
           (remove #(str/includes? had (:name %)))
           distinct
           (take names-checked)))))

(defn- source-texts
  "The source files under the places with their text, the edited file left out, until the deadline."
  [places ^File edited deadline]
  (let [skip (.getCanonicalPath edited)
        budget (atom max-files)]
    (->> (for [{:keys [locator rel]} places
               :let [^File base (fs/safe-file locator (str rel))]
               :when (and base (.exists base))
               ^File f (if (.isDirectory base) (walk base budget) [base])
               :when (and (source-exts (ext-of (.getName f))) (not= skip (.getCanonicalPath f)))
               :let [t (text-of f)]
               :when t]
           t)
         (take-while (fn [_] (< (System/currentTimeMillis) deadline))))))

(defn- guessed-names
  "Of the names an edit introduced, those the tree cannot have meant: some file declares the qualifier
   (`namespace ke::log`, `enum class Level`), and no file that declares it has the member. A qualifier
   no file declares is a library's. Only the declaring files count for the member: a call-shaped
   `debug(` elsewhere (`DebugHooks debug(started)` in App.cpp) excused GLM's `log::debug` once."
  [places edited names]
  (when (seq names)
    (let [q (fn [{:keys [qualifier]}]
              (let [own (last (str/split qualifier #"::"))]
                (re-pattern (str "\\b(?:namespace\\s+(?:\\w+::)*|class\\s+|struct\\s+|enum\\s+(?:class\\s+)?)"
                                 (Pattern/quote own) "\\b"))))
          s (fn [{:keys [segment call?]}] (re-pattern (str "\\b" (Pattern/quote segment) (if call? "\\s*\\(" "\\b"))))
          res (map (fn [n] [n (q n) (s n)]) names)
          seen (reduce (fn [acc ^String t]
                         (let [acc (reduce (fn [acc [n qr sr]]
                                             (if (and (not (get-in acc [n :s])) (re-find qr t))
                                               (cond-> (assoc-in acc [n :q] true)
                                                 (re-find sr t) (assoc-in [n :s] true))
                                               acc))
                                           acc res)]
                           (if (every? #(get-in acc [% :s]) names) (reduced acc) acc)))
                       {} (source-texts places edited (+ (System/currentTimeMillis) names-ms)))]
      (filter #(and (get-in seen [% :q]) (not (get-in seen [% :s]))) names))))

(defn- unknown-names
  "What an edit's result says about names it introduced that the tree does not have, or nil."
  [db p located ^File file ^String before {:keys [old new]}]
  (try
    (let [names (introduced (ext-of located) (str new) (str old) (str before))
          places (when (seq names) (under db p (first (str/split (str located) #"/"))))]
      (seq (for [{:keys [name qualifier segment call?]} (guessed-names places file names)]
             (str name ": the tree knows `" qualifier "` and has no `" segment (when call? "(") "` anywhere. "
                  "It will not build. Read where `" qualifier "` is declared before using it"))))
    (catch Exception _ nil)))

(defn edit-refusal
  "Why this edit cannot land, as {:err}, or nil. Reads the file; writes nothing."
  [db p args]
  (select-keys (planned db p args) [:err]))

(defn edit!
  "{name old new all? at?}: one change to a mounted file, atomically. A new file is `new` with no
   `old` on a path that does not exist. The change lands on the log as the file Object's own
   refreshed Fields, with the diff in file/last-edit. The gate is fs:write's (act.gates)."
  [world p {:keys [via] :as args}]
  (let [{:keys [err file before text created?] located :name} (planned (act/db world) p args)]
    (if err
      {:err err}
      (try
        (write-file! file text)
        (let [{:keys [object]} (resolve/resolve! world (act/db world) p located :read)
              diff (diff-of before text)]
          (when object
            (act/set-field! world p {:object object :field :file/last-edit :type :string :value diff :reader :both :via via}))
          {:ok (cond-> {:name located :created created? :bytes (count (.getBytes ^String text "UTF-8")) :diff diff}
                 (not created?) (as-> r (if-let [u (unknown-names (act/db world) p located file before args)]
                                          (assoc r :unknown-names (vec u))
                                          r)))})
        (catch java.io.IOException e
          (act/err :not-allowed (str "the file could not be written: " (ex-message e))))))))
