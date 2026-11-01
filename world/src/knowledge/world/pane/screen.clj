(ns knowledge.world.pane.screen
  "A terminal's screen as a value (docs/panes.md): feed it what the program wrote, read back exact
   text, a cursor, scrollback, or the escape sequence that paints it, so a client attaching late is
   exact. An xterm subset: enough for a shell, `top`, `less`, `vim`, `htop`. Unknown sequences are consumed
   and do nothing, because a program's output is never an error.

   A cell is [text attr]: text a string (empty for the right half of a wide character), attr the
   SGR parameters in force as a string, or nil."
  (:require [clojure.string :as str]))

(def blank [" " nil])
(def scrollback-cap 5000)

(defn- blank-line [cols] (vec (repeat cols blank)))
(defn- blank-lines [cols rows] (vec (repeat rows (blank-line cols))))
(defn- tabstops [cols] (set (range 8 cols 8)))

(defn create
  "An empty screen."
  [cols rows]
  {:cols cols :rows rows :cx 0 :cy 0 :lines (blank-lines cols rows) :scrollback [] :alt nil
   :attr nil :top 0 :bottom (dec rows) :saved nil :parser :ground :buffer "" :pending-wrap false
   :title nil :cursor-visible true :autowrap true :origin false :insert false :tabs (tabstops cols)
   :last-char nil :scrolled 0 :modes #{} :keypad false :cursor-shape 0})

(def passed-modes
  "Private modes a client must know, because they change what its keys, mouse and paste send: the
   screen does not act on them, it carries them across. Application cursor keys, focus events,
   mouse reporting and its encodings, bracketed paste."
  #{1 1000 1002 1003 1004 1005 1006 1015 2004})

(defn- clamp [lo hi v] (max lo (min hi v)))

(defn- wide?
  "Does this code point take two cells? East Asian wide and emoji blocks."
  [cp]
  (or (<= 0x1100 cp 0x115F) (<= 0x2E80 cp 0x303E) (<= 0x3041 cp 0x33FF) (<= 0x3400 cp 0x4DBF)
      (<= 0x4E00 cp 0x9FFF) (<= 0xA000 cp 0xA4CF) (<= 0xAC00 cp 0xD7A3) (<= 0xF900 cp 0xFAFF)
      (<= 0xFE30 cp 0xFE6F) (<= 0xFF00 cp 0xFF60) (<= 0xFFE0 cp 0xFFE6) (<= 0x1F300 cp 0x1F64F)
      (<= 0x1F900 cp 0x1F9FF) (<= 0x20000 cp 0x3FFFD)))

(defn- combining? [cp]
  (or (<= 0x0300 cp 0x036F) (<= 0x1AB0 cp 0x1AFF) (<= 0x1DC0 cp 0x1DFF) (<= 0x20D0 cp 0x20FF)
      (<= 0xFE00 cp 0xFE0F) (<= 0xFE20 cp 0xFE2F) (= cp 0x200D)))

;; ---------------------------------------------------------------- scrolling

(defn- scroll-up
  "Scroll the region up n lines; lines leaving the top of a full primary screen go to scrollback."
  [{:keys [top bottom cols alt] :as s} n]
  (let [n (clamp 0 (inc (- bottom top)) n)
        lines (:lines s)
        leaving (subvec lines top (+ top n))
        kept (subvec lines (+ top n) (inc bottom))
        fresh (blank-lines cols n)
        region (into (vec kept) fresh)]
    (cond-> (assoc s :lines (vec (concat (subvec lines 0 top) region (subvec lines (inc bottom)))))
      (and (zero? top) (nil? alt))
      (-> (update :scrolled + n)
          (update :scrollback (fn [sb] (let [sb (into sb leaving) over (- (count sb) scrollback-cap)]
                                         (if (pos? over) (subvec sb over) sb))))))))

(defn- scroll-down [{:keys [top bottom cols] :as s} n]
  (let [n (clamp 0 (inc (- bottom top)) n)
        lines (:lines s)
        kept (subvec lines top (- (inc bottom) n))
        region (into (blank-lines cols n) kept)]
    (assoc s :lines (vec (concat (subvec lines 0 top) region (subvec lines (inc bottom)))))))

(defn- index [{:keys [cy bottom rows] :as s}]
  (cond (= cy bottom) (scroll-up s 1)
        (< cy (dec rows)) (update s :cy inc)
        :else s))

(defn- reverse-index [{:keys [cy top] :as s}]
  (if (= cy top) (scroll-down s 1) (update s :cy #(max 0 (dec %)))))

(defn- newline* [s] (-> s index (assoc :pending-wrap false)))

;; ---------------------------------------------------------------- printing

(defn- set-cell [s y x cell] (assoc-in s [:lines y x] cell))

(defn- wide-lead? [[^String t]] (and (seq t) (wide? (.codePointAt t 0))))

(defn- heal
  "Row y with no half a wide character left behind: what a shift or an erase cut in two is blank."
  [s y]
  (update-in s [:lines y]
             (fn [line]
               (let [n (count line)]
                 (reduce (fn [l i]
                           (let [c (nth l i)]
                             (cond
                               (and (= "" (first c)) (or (zero? i) (not (wide-lead? (nth l (dec i)))))) (assoc l i blank)
                               (and (wide-lead? c) (or (= i (dec n)) (not= "" (first (nth l (inc i)))))) (assoc l i blank)
                               :else l)))
                         line (range n))))))

(defn- combine [{:keys [cx cy] :as s} ^String ch]
  (let [x (max 0 (dec cx))]
    (update-in s [:lines cy x 0] str ch)))

(defn- wrap-if-due [{:keys [pending-wrap autowrap] :as s}]
  (if (and pending-wrap autowrap)
    (-> s (assoc :cx 0 :pending-wrap false) index)
    s))

(defn- insert-blanks [{:keys [cols cx cy] :as s} n]
  (heal (update-in s [:lines cy]
                   (fn [line] (vec (take cols (concat (subvec line 0 cx) (repeat n blank) (subvec line cx))))))
        cy))

(defn- put
  "One printable code point at the cursor."
  [s ^long cp]
  (let [ch (String. (Character/toChars cp))]
    (if (combining? cp)
      (combine s ch)
      (let [width (if (wide? cp) 2 1)
            s (wrap-if-due s)
            s (if (and (= width 2) (= (:cx s) (dec (:cols s))))
                (-> s (assoc :cx 0) index)
                s)
            s (if (:insert s) (insert-blanks s width) s)
            {:keys [cx cy cols attr]} s
            cut? (or (= "" (first (get-in s [:lines cy cx])))
                     (= "" (first (get-in s [:lines cy (+ cx width)]))))
            s (set-cell s cy cx [ch attr])
            s (if (= width 2) (set-cell s cy (inc cx) ["" attr]) s)
            s (if cut? (heal s cy) s)
            nx (+ cx width)]
        (cond-> (assoc s :last-char cp)
          (< nx cols) (assoc :cx nx)
          (>= nx cols) (assoc :cx (dec cols) :pending-wrap true))))))

;; ---------------------------------------------------------------- editing

(defn- erase-cells [s y from to]
  (heal (update-in s [:lines y]
                   (fn [line] (reduce (fn [l x] (assoc l x blank)) line (range from to))))
        y))

(defn- erase-display [{:keys [cx cy cols rows] :as s} mode]
  (case (long mode)
    0 (reduce #(erase-cells %1 %2 0 cols) (erase-cells s cy cx cols) (range (inc cy) rows))
    1 (reduce #(erase-cells %1 %2 0 cols) (erase-cells s cy 0 (inc cx)) (range 0 cy))
    2 (assoc s :lines (blank-lines cols rows))
    3 (assoc s :scrollback [])
    s))

(defn- erase-line [{:keys [cx cy cols] :as s} mode]
  (case (long mode)
    0 (erase-cells s cy cx cols)
    1 (erase-cells s cy 0 (inc cx))
    2 (erase-cells s cy 0 cols)
    s))

(defn- delete-chars [{:keys [cols cx cy] :as s} n]
  (heal (update-in s [:lines cy]
                   (fn [line] (let [n (min n (- cols cx))
                                    right (subvec line (+ cx n))]
                                (vec (concat (subvec line 0 cx) right (repeat n blank))))))
        cy))

(defn- insert-lines [{:keys [cy top bottom] :as s} n]
  (if (<= top cy bottom)
    (let [held (assoc s :top cy)]
      (assoc (scroll-down held n) :top top))
    s))

(defn- delete-lines [{:keys [cy top bottom alt] :as s} n]
  (if (<= top cy bottom)
    ;; lines deleted inside the screen are gone: they never go to scrollback
    (let [held (assoc s :top cy :alt (or alt :held))]
      (assoc (scroll-up held n) :top top :alt alt))
    s))

;; ---------------------------------------------------------------- cursor

(defn- move-to [{:keys [rows cols origin top bottom] :as s} y x]
  (let [y (if origin (clamp top bottom (+ top y)) (clamp 0 (dec rows) y))]
    (assoc s :cy y :cx (clamp 0 (dec cols) x) :pending-wrap false)))

(defn- move-rel [{:keys [cx cy rows cols top bottom] :as s} dy dx]
  (let [lo (if (>= cy top) top 0)
        hi (if (<= cy bottom) bottom (dec rows))]
    (assoc s :cy (clamp lo hi (+ cy dy)) :cx (clamp 0 (dec cols) (+ cx dx)) :pending-wrap false)))

(defn- next-tab [{:keys [cx cols tabs]}]
  (or (first (filter tabs (range (inc cx) cols))) (dec cols)))

(defn- prev-tab [{:keys [cx tabs]}]
  (or (first (filter tabs (range (dec cx) -1 -1))) 0))

(defn- save-cursor [{:keys [cx cy attr autowrap origin] :as s}]
  (assoc s :saved {:cx cx :cy cy :attr attr :autowrap autowrap :origin origin}))

(defn- restore-cursor [{:keys [saved] :as s}]
  (if saved (-> (merge s saved) (assoc :pending-wrap false)) (assoc s :cx 0 :cy 0 :attr nil :pending-wrap false)))

;; ---------------------------------------------------------------- alternate screen

(defn- enter-alt [{:keys [alt cols rows] :as s} save?]
  (if alt
    s
    (let [s (if save? (save-cursor s) s)]
      (assoc s :alt {:lines (:lines s)} :lines (blank-lines cols rows)))))

(defn- leave-alt [{:keys [alt] :as s} restore?]
  (if alt
    (let [s (assoc s :lines (:lines alt) :alt nil)]
      (if restore? (restore-cursor s) s))
    s))

;; ---------------------------------------------------------------- SGR

(defn- sgr
  "Keep the attributes as the program stated them; `0` or an empty list resets."
  [s ^String raw]
  (let [params (str/split raw #";" -1)
        reset? (or (str/blank? raw) (some #(or (= % "0") (= % "")) (take 1 params)))
        kept (when reset? (remove #(or (= % "0") (= % "")) (rest params)))]
    (cond
      (and reset? (empty? kept)) (assoc s :attr nil)
      reset? (assoc s :attr (str/join ";" kept))
      :else (update s :attr (fn [a] (if a (str a ";" raw) raw))))))

;; ---------------------------------------------------------------- CSI

(defn- param [ps i default]
  (let [v (get ps i)] (if (or (nil? v) (zero? v)) default v)))

(defn- raw-param [ps i default]
  (let [v (get ps i)] (if (nil? v) default v)))

(defn- modes [s private ps on?]
  (reduce
    (fn [s p]
      (if private
        (case (long p)
          6 (-> s (assoc :origin on?) (move-to 0 0))
          7 (assoc s :autowrap on?)
          25 (assoc s :cursor-visible on?)
          (47 1047) (if on? (enter-alt s false) (leave-alt s false))
          1049 (if on? (enter-alt s true) (leave-alt s true))
          (if (passed-modes p) (update s :modes (if on? conj disj) p) s))
        (case (long p)
          4 (assoc s :insert on?)
          s)))
    s ps))

(defn- csi
  "Act on a complete control sequence."
  [{:keys [cx cy cols rows] :as s} ^String body final]
  (let [private (when (and (pos? (count body)) (#{\? \> \= \<} (.charAt body 0))) (.charAt body 0))
        raw (if private (subs body 1) body)
        ps (mapv (fn [^String p] (let [d (re-find #"^\d+" p)] (when d (parse-long d))))
                 (if (str/blank? raw) [] (str/split raw #";" -1)))
        n (param ps 0 1)]
    (case final
      \A (move-rel s (- n) 0)
      \B (move-rel s n 0)
      \C (move-rel s 0 n)
      \D (move-rel s 0 (- n))
      \E (move-rel (assoc s :cx 0) n 0)
      \F (move-rel (assoc s :cx 0) (- n) 0)
      (\G \`) (assoc s :cx (clamp 0 (dec cols) (dec n)) :pending-wrap false)
      (\H \f) (move-to s (dec (param ps 0 1)) (dec (param ps 1 1)))
      \J (if private s (erase-display s (raw-param ps 0 0)))
      \K (if private s (erase-line s (raw-param ps 0 0)))
      \L (insert-lines s n)
      \M (delete-lines s n)
      \P (delete-chars s n)
      \S (scroll-up s n)
      \T (if (> (count ps) 1) s (scroll-down s n))
      \X (erase-cells s cy cx (min cols (+ cx n)))
      \@ (insert-blanks s n)
      \I (reduce (fn [s _] (assoc s :cx (next-tab s))) s (range n))
      \Z (reduce (fn [s _] (assoc s :cx (prev-tab s))) s (range n))
      \a (move-rel s 0 n)
      \e (move-rel s n 0)
      \d (let [y (dec (param ps 0 1))] (assoc s :cy (clamp 0 (dec rows) y) :pending-wrap false))
      \b (if-let [c (:last-char s)] (reduce (fn [s _] (put s c)) s (range n)) s)
      \g (case (long (raw-param ps 0 0))
           0 (update s :tabs disj cx)
           3 (assoc s :tabs #{})
           s)
      \h (modes s private ps true)
      \l (modes s private ps false)
      \m (if private s (sgr s raw))
      \q (if (and (not private) (str/ends-with? raw " ")) (assoc s :cursor-shape (raw-param ps 0 0)) s)
      \r (if private
           s
           (let [t (dec (param ps 0 1)) b (dec (param ps 1 rows))]
             (if (< t b)
               (move-to (assoc s :top (clamp 0 (dec rows) t) :bottom (clamp 0 (dec rows) b)) 0 0)
               s)))
      \s (if private s (save-cursor s))
      \u (if private s (restore-cursor s))
      s)))

;; ---------------------------------------------------------------- the parser

(defn- osc-done [{:keys [buffer] :as s}]
  (let [[_ code text] (re-matches #"(?s)(\d+);(.*)" buffer)]
    (cond-> (assoc s :parser :ground :buffer "")
      (#{"0" "2"} code) (assoc :title text))))

(defn- step
  "The screen after one code point."
  [{:keys [parser] :as s} ^long cp]
  (let [cancel? (or (= cp 0x18) (= cp 0x1A))]
    (case parser
      :ground
      (cond
        (= cp 0x1B) (assoc s :parser :esc)
        (= cp 0x9B) (assoc s :parser :csi :buffer "")
        (= cp 0x0D) (assoc s :cx 0 :pending-wrap false)
        (<= 0x0A cp 0x0C) (newline* s)
        (= cp 0x08) (assoc s :cx (max 0 (dec (:cx s))) :pending-wrap false)
        (= cp 0x09) (assoc s :cx (next-tab s) :pending-wrap false)
        (< cp 0x20) s
        (= cp 0x7F) s
        (<= 0x80 cp 0x9F) s
        :else (put s cp))

      :esc
      (let [s (assoc s :parser :ground)]
        (case (char (min cp 0xFFFF))
          \[ (assoc s :parser :csi :buffer "")
          \] (assoc s :parser :osc :buffer "")
          (\P \_ \^ \X) (assoc s :parser :string :buffer "")
          (\( \) \* \+ \- \. \/ \# \% \space) (assoc s :parser :skip-one)
          \7 (save-cursor s)
          \8 (restore-cursor s)
          \D (index s)
          \M (reverse-index s)
          \E (-> s index (assoc :cx 0 :pending-wrap false))
          \H (update s :tabs conj (:cx s))
          \c (create (:cols s) (:rows s))
          \= (assoc s :keypad true)
          \> (assoc s :keypad false)
          s))

      :skip-one (assoc s :parser :ground)

      :csi
      (cond
        cancel? (assoc s :parser :ground :buffer "")
        (= cp 0x1B) (assoc s :parser :esc :buffer "")
        (<= 0x40 cp 0x7E) (csi (assoc s :parser :ground :buffer "") (:buffer s) (char cp))
        (<= 0x20 cp 0x3F) (update s :buffer str (char cp))
        :else s)

      :osc
      (cond
        (= cp 0x07) (osc-done s)
        (= cp 0x9C) (osc-done s)
        (= cp 0x1B) (assoc s :parser :osc-esc)
        cancel? (assoc s :parser :ground :buffer "")
        (< (count (:buffer s)) 4096) (update s :buffer str (String. (Character/toChars cp)))
        :else s)

      :osc-esc (osc-done s)

      :string
      (cond
        (= cp 0x1B) (assoc s :parser :string-esc)
        (or (= cp 0x9C) (= cp 0x07) cancel?) (assoc s :parser :ground :buffer "")
        :else s)

      :string-esc (assoc s :parser :ground :buffer ""))))

(defn feed
  "The screen after text, a string as the program wrote it."
  [s ^String text]
  (let [n (.length text)]
    (loop [i 0 s s]
      (if (< i n)
        (let [cp (.codePointAt text i)]
          (recur (+ i (Character/charCount cp)) (step s cp)))
        s))))

;; ---------------------------------------------------------------- a screen from the holder

(defn apply-frame
  "A screen the holder's terminal has already worked out (pane/holder, screen-frame): the cells that
   changed and the state around them. paint and diff read the result as they read a fed screen, with no
   scrolling region of its own, because the client is sent whole rows."
  [s {:keys [cols rows lines alt] :as frame}]
  (let [fresh? (or (:full frame) (not= cols (:cols s)) (not= rows (:rows s)))
        base (if fresh? (blank-lines cols rows) (:lines s))
        fit (fn [line] (if (= (count line) cols) line (subvec (into line (repeat cols blank)) 0 cols)))]
    (assoc s :cols cols :rows rows :top 0 :bottom (dec rows)
           :cx (:cx frame) :cy (:cy frame) :cursor-visible (:cursor-visible frame)
           :title (:title frame) :modes (:modes frame) :keypad (:keypad frame) :cursor-shape (:cursor-shape frame)
           :scrolled (:scrolled frame)
           :alt (when alt true)
           :lines (reduce-kv (fn [v y line] (if (< y rows) (assoc v y (fit line)) v)) base lines))))

;; ---------------------------------------------------------------- reading

(defn- line-text [line]
  (str/trimr (apply str (map first line))))

(defn screen-lines
  "The visible lines as text, right-trimmed."
  [{:keys [lines]}]
  (mapv line-text lines))

(defn text
  "The visible screen as text, trailing blank lines dropped."
  [s]
  (let [ls (screen-lines s)
        last-used (last (keep-indexed (fn [i l] (when-not (str/blank? l) i)) ls))]
    (str/join "\n" (subvec ls 0 (if last-used (inc last-used) 0)))))

(defn scrollback-lines
  "Lines that have left the top of the primary screen, oldest first."
  [{:keys [scrollback]}]
  (mapv line-text scrollback))

(defn cursor [{:keys [cx cy cursor-visible]}] {:col cx :row cy :visible cursor-visible})

(defn- paint-line [line]
  (let [sb (StringBuilder.)]
    (reduce (fn [attr [ch a]]
              (when (not= a attr)
                (.append sb (str "\u001b[0" (when a (str ";" a)) "m")))
              (.append sb ^String ch)
              a)
            nil line)
    (.append sb "\u001b[0m")
    (str sb)))

(defn- paint-modes
  "What differs between two mode states, as escape sequences. old nil states every mode, for a client
   whose own state is unknown."
  [^StringBuilder sb old {:keys [modes keypad cursor-shape]}]
  (doseq [m (sort passed-modes)
          :let [on? (contains? modes m)]
          :when (or (nil? old) (not= on? (contains? (:modes old) m)))]
    (.append sb (str "\u001b[?" m (if on? "h" "l"))))
  (when (or (nil? old) (not= keypad (:keypad old)))
    (.append sb (if keypad "\u001b=" "\u001b>")))
  (when (or (nil? old) (not= cursor-shape (:cursor-shape old)))
    (.append sb (str "\u001b[" cursor-shape " q"))))

(defn paint
  "Escape sequences that bring an empty terminal of the same size to exactly this screen."
  [{:keys [lines cx cy cursor-visible alt title top bottom rows] :as screen}]
  (let [sb (StringBuilder.)]
    (.append sb "\u001b[?1049l\u001b[r\u001b[0m\u001b[H\u001b[2J")
    (when alt (.append sb "\u001b[?1049h\u001b[H\u001b[2J"))
    (when title (.append sb (str "\u001b]2;" title "\u0007")))
    (doseq [[i line] (map-indexed vector lines)]
      (.append sb (str "\u001b[" (inc i) ";1H"))
      (.append sb ^String (paint-line line)))
    (when-not (and (zero? top) (= bottom (dec rows)))
      (.append sb (str "\u001b[" (inc top) ";" (inc bottom) "r")))
    (.append sb (str "\u001b[" (inc cy) ";" (inc cx) "H"))
    (.append sb (if cursor-visible "\u001b[?25h" "\u001b[?25l"))
    (paint-modes sb nil screen)
    (str sb)))

;; ---------------------------------------------------------------- deltas

(defn- changed-runs
  "The [from to) column runs where two lines differ, runs closer than a cursor move joined."
  [old new]
  (let [diffs (keep-indexed (fn [i c] (when (not= c (nth old i nil)) i)) new)]
    (reduce (fn [runs i]
              (let [[from to] (peek runs)]
                (if (and to (<= (- i to) 4))
                  (conj (pop runs) [from (inc i)])
                  (conj runs [i (inc i)]))))
            [] diffs)))

(defn- whole-cells
  "A run widened so it never starts or ends inside a wide character."
  [line [from to]]
  [(if (and (pos? from) (= "" (ffirst (subvec line from (inc from))))) (dec from) from)
   (if (and (< to (count line)) (= "" (first (nth line to)))) (inc to) to)])

(defn- paint-run
  "One changed run of row y; a blank tail is an erase, which costs less than the spaces."
  [^StringBuilder sb y line [from to]]
  (let [cols (count line)
        used (if (= to cols)
               (inc (or (last (keep-indexed (fn [i c] (when (and (>= i from) (not= c blank)) i)) line))
                        (dec from)))
               to)
        move (if (zero? from) (str "\u001b[" (inc y) "H") (str "\u001b[" (inc y) ";" (inc from) "H"))]
    (.append sb ^String move)
    (when (> used from) (.append sb (str/replace ^String (paint-line (subvec line from used)) #"\u001b\[0m$" "")))
    (when (< used to) (.append sb (if (> (- used from) 0) "\u001b[0m\u001b[K" "\u001b[K")))
    (when (and (> used from) (>= used to) (some second (subvec line from used))) (.append sb "\u001b[0m"))))

(defn diff
  "Escape sequences that bring a terminal showing old to exactly new, in as few bytes as a cell diff
   gives: changed runs of changed rows, and whole lines reused when the screen scrolled. nil when
   the two cannot be bridged (another size, the alternate screen entered or left): paint instead."
  [old new]
  (when (and (= (:cols old) (:cols new)) (= (:rows old) (:rows new))
             (= (some? (:alt old)) (some? (:alt new))))
    (let [sb (StringBuilder.)
          {:keys [rows cols top bottom title cursor-visible]} new
          full? (and (zero? top) (= bottom (dec rows)) (zero? (:top old)) (= (:bottom old) (dec rows)))
          k (- (:scrolled new 0) (:scrolled old 0))
          scroll? (and full? (nil? (:alt new)) (< 0 k rows))
          base (if scroll?
                 (into (subvec (:lines old) k) (repeat k (blank-line cols)))
                 (:lines old))]
      (when-not (= title (:title old)) (.append sb (str "\u001b]2;" title "\u0007")))
      (when-not (and (= top (:top old)) (= bottom (:bottom old)))
        (.append sb (str "\u001b[" (inc top) ";" (inc bottom) "r")))
      (when scroll? (.append sb (str "\u001b[" k "S")))
      (dotimes [y rows]
        (let [o (nth base y) n (nth (:lines new) y)]
          (when-not (identical? o n)
            (when (not= o n)
              (doseq [run (changed-runs o n)]
                (paint-run sb y n (whole-cells n run)))))))
      (when (or (pos? (.length sb)) (not= [(:cx old) (:cy old)] [(:cx new) (:cy new)]))
        (.append sb (str "\u001b[" (inc (:cy new)) ";" (inc (:cx new)) "H")))
      (when-not (= cursor-visible (:cursor-visible old))
        (.append sb (if cursor-visible "\u001b[?25h" "\u001b[?25l")))
      (paint-modes sb old new)
      (str sb))))

(defn resize
  "The screen at a new size: rows leave the top into scrollback, the cursor stays on its line."
  [{:keys [cols rows cy] :as s} ncols nrows]
  (let [fit (fn [line] (if (>= (count line) ncols)
                         (subvec line 0 ncols)
                         (into line (repeat (- ncols (count line)) blank))))
        lines (mapv fit (:lines s))
        used (inc (or (last (keep-indexed (fn [i l] (when-not (str/blank? (line-text l)) i)) lines)) 0))
        used (max used (inc cy))
        over (max 0 (- used nrows))
        gone (subvec lines 0 over)
        kept (subvec lines over (min (count lines) (+ over nrows)))
        lines' (into kept (repeat (- nrows (count kept)) (blank-line ncols)))]
    (-> s
        (assoc :cols ncols :rows nrows :lines lines' :top 0 :bottom (dec nrows)
               :cx (clamp 0 (dec ncols) (:cx s)) :cy (clamp 0 (dec nrows) (- cy over))
               :tabs (tabstops ncols) :pending-wrap false)
        (update :scrollback (fn [sb] (if (:alt s) sb (into sb gone))))
        (cond-> (:alt s) (update-in [:alt :lines]
                                    (fn [ls] (let [ls (mapv fit ls)
                                                   k (subvec ls 0 (min nrows (count ls)))]
                                               (into k (repeat (- nrows (count k)) (blank-line ncols))))))))))
