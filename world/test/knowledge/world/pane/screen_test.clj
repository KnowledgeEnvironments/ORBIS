(ns knowledge.world.pane.screen-test
  "The terminal screen model: exact text from what a program wrote."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.pane.screen :as scr]))

(defn- run [cols rows & chunks] (reduce scr/feed (scr/create cols rows) chunks))
(def esc "\u001b")

(deftest plain-output-lands-where-it-should
  (is (= "hello" (scr/text (run 20 4 "hello"))))
  (is (= "one\ntwo" (scr/text (run 20 4 "one\r\ntwo"))))
  (is (= "ab\n  cd" (scr/text (run 20 4 "ab\n\r  cd"))))
  (is (= "xbc" (scr/text (run 20 4 "abc\rx"))) "a carriage return overwrites")
  (is (= "abd" (scr/text (run 20 4 "abc\bd"))) "backspace moves left")
  (is (= "a       b" (scr/text (run 20 4 "a\tb"))) "tab stops every 8"))

(deftest long-lines-wrap-and-the-screen-scrolls
  (is (= "abcd\nef" (scr/text (run 4 3 "abcdef"))))
  (testing "the last column holds the cursor until the next character"
    (let [s (run 4 3 "abcd")] (is (= {:col 3 :row 0 :visible true} (scr/cursor s)))))
  (let [s (run 10 2 "one\r\ntwo\r\nthree")]
    (is (= "two\nthree" (scr/text s)))
    (is (= ["one"] (scr/scrollback-lines s)))))

(deftest cursor-moves-and-erases
  (is (= "aXc" (scr/text (run 20 4 "abc" (str esc "[1;2H") "X"))))
  (is (= "ab" (scr/text (run 20 4 "abcd" (str esc "[1;3H" esc "[K")))))
  (is (= "" (scr/text (run 20 4 "abc\r\ndef" (str esc "[2J")))))
  (is (= "abd" (scr/text (run 20 4 "abcd" (str esc "[1;3H" esc "[P")))))
  (is (= "ab cd" (scr/text (run 20 4 "abcd" (str esc "[1;3H" esc "[@"))))))

(deftest colour-is-kept-and-does-not-reach-the-text
  (let [s (run 20 4 (str esc "[1;32mok" esc "[0m!"))]
    (is (= "ok!" (scr/text s)))
    (is (= "1;32" (get-in s [:lines 0 0 1])))
    (is (nil? (get-in s [:lines 0 2 1])))))

(deftest full-screen-programs-use-the-alternate-screen
  (let [shell (run 20 4 "$ vim\r\n")
        vim (scr/feed shell (str esc "[?1049h" esc "[2J" esc "[H~\r\n~" esc "[4;1H:wq"))]
    (is (= "~\n~\n\n:wq" (scr/text vim)) "the program's screen")
    (is (= "$ vim" (scr/text (scr/feed vim (str esc "[?1049l")))) "and the shell's comes back whole")
    (is (= {:col 0 :row 1 :visible true} (select-keys (scr/cursor (scr/feed vim (str esc "[?1049l"))) [:col :row :visible]))
        "with its cursor")))

(deftest a-scroll-region-scrolls-only-itself
  (let [s (run 10 4 "head\r\none\r\ntwo\r\nfoot" (str esc "[2;3r" esc "[3;1H\n"))]
    (is (= "head\ntwo\n\nfoot" (scr/text s)))
    (is (empty? (scr/scrollback-lines s)) "a region's lines never reach scrollback")))

(deftest wide-and-combining-characters
  (let [s (run 6 2 "日本x")]
    (is (= "日本x" (scr/text s)))
    (is (= {:col 5 :row 0 :visible true} (scr/cursor s))))
  (is (= "éx" (scr/text (run 6 2 "éx")))))

(deftest the-title-and-unknown-sequences
  (is (= "t" (:title (run 10 2 (str esc "]0;t\u0007")))))
  (is (= "ok" (scr/text (run 10 2 (str esc "[?2004h" esc "]133;A\u0007" esc "Pjunk" esc "\\" "ok")))))
  (testing "a sequence split across chunks"
    (is (= "aXc" (scr/text (run 20 4 "abc" (str esc "[") "1;2" "HX"))))))

(deftest painting-reproduces-the-screen-exactly
  (doseq [chunks [["plain\r\ntext"]
                  [(str esc "[1;32mgreen" esc "[0m and " esc "[7minverse" esc "[0m") "\r\nnext"]
                  ["$ vim\r\n" (str esc "[?1049h" esc "[2J" esc "[H~\r\n" esc "[4;2H:q")]
                  [(str "abc" esc "[?25l")]]]
    (let [s (apply run 20 4 chunks)
          again (scr/feed (scr/create 20 4) (scr/paint s))]
      (is (= (scr/text s) (scr/text again)))
      (is (= (:lines s) (:lines again)) "attributes too")
      (is (= (scr/cursor s) (scr/cursor again))))))

(deftest resizing-keeps-what-matters
  (let [s (scr/resize (run 10 4 "one\r\ntwo\r\nthree\r\nfour") 10 2)]
    (is (= "three\nfour" (scr/text s)))
    (is (= ["one" "two"] (scr/scrollback-lines s))))
  (is (= "abc" (scr/text (scr/resize (run 10 4 "abcdef") 3 4))) "narrower clips")
  (is (= 4 (count (:lines (scr/resize (run 10 2 "a") 10 4))))))

;; ---------------------------------------------------------------- deltas

(defn- same-picture? [a b]
  (and (= (:lines a) (:lines b)) (= (scr/cursor a) (scr/cursor b)) (= (:title a) (:title b))
       (= (:top a) (:top b)) (= (:bottom a) (:bottom b))))

(defn- through-delta
  "A client that has old's paint, then receives only the delta to new."
  [old new]
  (let [client (scr/feed (scr/create (:cols old) (:rows old)) (scr/paint old))]
    (scr/feed client (scr/diff old new))))

(deftest a-delta-is-small-and-exact
  (let [old (run 80 24 "$ ls\r\none\r\ntwo\r\n$ ")
        new (scr/feed old "x")
        d (scr/diff old new)]
    (is (same-picture? new (through-delta old new)))
    (is (< (count d) 24) (str "a keystroke is a few bytes, not a screen: " (pr-str d)))
    (is (= "" (scr/diff new new)) "nothing changed, nothing sent"))
  (testing "colour, erasure and wide characters survive"
    (let [old (run 30 6 esc "[31mred" esc "[0m plain\r\n" "\u4e2d\u6587 wide\r\n" "long line to erase")
          new (scr/feed old (str esc "[3;6H" esc "[K" esc "[1;1H" esc "[42mgreen" esc "[0m" esc "[2;1Hab"))]
      (is (same-picture? new (through-delta old new)))
      (is (= (:lines new) (:lines (through-delta old new))))))
  (testing "a title, a region and a hidden cursor"
    (let [old (run 30 6 "x")
          new (scr/feed old (str esc "]2;work" "\u0007" esc "[2;5r" esc "[?25l"))]
      (is (same-picture? new (through-delta old new))))))

(deftest a-scroll-reuses-the-lines-the-client-has
  (let [old (reduce (fn [s i] (scr/feed s (str "line " i " with some length to it\r\n")))
                    (scr/create 80 24) (range 40))
        new (scr/feed old "next line\r\n")
        d (scr/diff old new)]
    (is (same-picture? new (through-delta old new)))
    (is (str/includes? d (str esc "[1S")))
    (is (< (count d) 60) (str "a scroll is not a repaint: " (pr-str d)))
    (is (< (count d) (/ (count (scr/paint new)) 10)))))

(deftest a-delta-is-exact-through-anything-a-program-writes
  (let [chunks ["hello\r\n" (str esc "[31mred" esc "[0m\r\n") (str esc "[5;10Hmoved") (str esc "[2J" esc "[H")
                "abcdefghijklmnopqrstuvwxyz0123456789 wraps past the edge of it" (str esc "[3;8r" esc "[8;1H\n\n\n")
                (str esc "[r") (str esc "[2;3H" esc "[2L") (str esc "[2M") (str esc "[1;4H" esc "[3P")
                (str esc "[1;4H" esc "[3@") "\u4e2d\u6587\r\n" (str esc "[?25l") (str esc "[?25h") "\r\n\r\n\r\n"
                (str esc "[K") (str esc "[1J") (str esc "[7mrev" esc "[27m") "\t\ttab\r\n"]
        rng (java.util.Random. 42)]
    (dotimes [_ 200]
      (let [old (reduce scr/feed (scr/create 24 8)
                        (repeatedly (.nextInt rng 8) #(nth chunks (.nextInt rng (count chunks)))))
            new (reduce scr/feed old
                        (repeatedly (inc (.nextInt rng 4)) #(nth chunks (.nextInt rng (count chunks)))))]
        (is (same-picture? new (through-delta old new)))))))

(deftest what-cannot-be-bridged-says-so
  (let [s (run 20 4 "x")]
    (is (nil? (scr/diff s (scr/resize s 30 4))))
    (is (nil? (scr/diff s (scr/feed s (str esc "[?1049h")))))))

(deftest modes-a-client-must-know-cross-the-wire
  (let [on (str esc "[?1h" esc "=" esc "[?1000;1006h" esc "[?2004h" esc "[5 q")
        s (run 20 4 on)]
    (testing "the screen holds them"
      (is (= #{1 1000 1006 2004} (:modes s)))
      (is (true? (:keypad s)))
      (is (= 5 (:cursor-shape s))))
    (testing "a delta carries a change and nothing else"
      (let [d (scr/diff (run 20 4) s)]
        (doseq [m ["?1h" "?1000h" "?1006h" "?2004h" "5 q"]] (is (str/includes? d m) m))
        (is (str/includes? d (str esc "=")))
        (is (not (str/includes? d "?1002"))))
      (is (= "" (scr/diff s s)) "nothing changed, nothing sent")
      (is (str/includes? (scr/diff s (scr/feed s (str esc "[?2004l"))) "?2004l")))
    (testing "a full frame states every one, so a client's old state cannot survive"
      (let [p (scr/paint (run 20 4))]
        (doseq [m ["?1l" "?1000l" "?1002l" "?1003l" "?1006l" "?2004l"]] (is (str/includes? p m) m))
        (is (str/includes? p "0 q")))
      (let [p (scr/paint s)]
        (is (str/includes? p "?2004h"))
        (is (= (select-keys s [:modes :keypad :cursor-shape])
               (select-keys (scr/feed (scr/create 20 4) p) [:modes :keypad :cursor-shape])))))
    (testing "entering the alternate screen keeps them"
      (is (= (:modes s) (:modes (scr/feed s (str esc "[?1049h"))))))
    (testing "a mode nobody asked to pass does not stick"
      (is (= #{} (:modes (run 20 4 (str esc "[?9999h"))))))))
