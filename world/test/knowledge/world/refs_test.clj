(ns knowledge.world.refs-test
  "References: handles, the way in, the way out, what a reader is handed."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.conversation :as conv]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.refs :as refs]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))
(defn- id [c n] (resolve-as c n))
(defn- cp-sub [^String s a b] (subs s (.offsetByCodePoints s 0 a) (.offsetByCodePoints s 0 b)))

(deftest handles-are-the-shortest-unique-prefix
  (let [a "a1b2c3d4-0000-4000-8000-000000000001"
        b "a1b2ffff-0000-4000-8000-000000000002"
        c "c7000000-0000-4000-8000-000000000003"]
    (testing "two characters at least, longer on collision"
      (let [t (refs/handles {} [a b c])]
        (is (= {"a1b2c" a "a1b2f" b "c7" c} t))))
    (testing "an id alone takes two"
      (is (= {"a1" a} (refs/handles {} [a]))))
    (testing "a wake's table keeps its meaning when a later id shares the prefix"
      (let [t1 (refs/handles {} [a c])
            t2 (refs/handles t1 [b])]
        (is (= a (t2 "a1")) "a stays a1")
        (is (= b (t2 "a1b2f")) "b grows past a")
        (is (= t1 (select-keys t2 (keys t1))))))
    (testing "a collision past the first group: the dashes do not count"
      (let [d "a1b2c3d4-5000-4000-8000-000000000004"
            e "a1b2c3d4-6000-4000-8000-000000000005"
            t (refs/handles {} [d e])]
        (is (= {"a1b2c3d45" d "a1b2c3d46" e} t))))
    (testing "shorten replaces only what the table holds"
      (is (= "see c7 and 00000000-0000-4000-8000-00000000000f"
             (refs/shorten (str "see " c " and 00000000-0000-4000-8000-00000000000f") (refs/handles {} [c])))))))

(deftest text-in-and-out
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          model (id root "root.model")
          db (act/db world)
          table (refs/handles {} [p model])
          h (fn [o] (some (fn [[k v]] (when (= v (str o)) k)) table))]
      (testing "the display text holds the name, the span says where"
        (let [{:keys [text refs unresolved]} (refs/parse db p table (str "ask [[" (h p) "]] now"))]
          (is (= "ask root now" text))
          (is (= [{:start 4 :end 8 :kind :object :object p :name "root"}] refs))
          (is (empty? unresolved))))
      (testing "the form a prompt shows parses too"
        (is (= "root" (:text (refs/parse db p table (str "[[" (h p) "|root|principal]]"))))))
      (testing "a Field and an event"
        (let [{:keys [text refs]} (refs/parse db p table (str "[[" (h p) ".loop/stalled]] at [[seq:680]]"))]
          (is (= "root.loop/stalled at seq:680" text))
          (is (= [:field :seq] (mapv :kind refs)))
          (is (= "loop/stalled" (:field (first refs))))
          (is (= 680 (:seq (second refs))))))
      (testing "an unknown handle stays as written and is listed"
        (let [{:keys [text refs unresolved]} (refs/parse db p table "see [[ff00]] and a1 in a sentence")]
          (is (= "see [[ff00]] and a1 in a sentence" text))
          (is (empty? refs))
          (is (= ["[[ff00]]"] unresolved))))
      (testing "code is literal, fenced or inline, closed or running off the end"
        (let [lit (str "```\n[[" (h p) "]]\n```\n`[[" (h p) "]]`")]
          (is (= lit (:text (refs/parse db p table lit)))))
        (is (empty? (:unresolved (refs/parse db p table "```\n[[nope]]"))))
        (is (empty? (:refs (refs/parse db p table (str "```clj\n[[" (h p) "]]"))))))
      (testing "offsets are code points, past text outside the BMP"
        (let [raw (str "日本語 😀 [[" (h p) "]] 😀 [[" (h model) "]]!")
              {:keys [text refs]} (refs/parse db p table raw)
              [a b] refs]
          (is (= 6 (:start a)) "six code points, seven chars")
          (is (= "root" (cp-sub text (:start a) (:end a))))
          (is (= "root.model" (cp-sub text (:start b) (:end b))))))
      (testing "render and parse are a round trip through a new table"
        (let [raw (str "日本語 😀 [[" (h p) "]] and [[" (h p) ".loop/stalled]] at [[seq:9]] 😀")
              {:keys [text refs]} (refs/parse db p table raw)
              out (refs/render db p text refs)
              t2 (refs/handles {} (refs/ids-in db out))
              back (refs/parse db p t2 (refs/shorten out t2))]
          (is (str/includes? (refs/shorten out t2) (str "[[" (first (keys t2)) "|root]]")))
          (is (= text (:text back)))
          (is (= (map #(dissoc % :name) refs) (map #(dissoc % :name) (:refs back))))))
      (testing "text with no spans, as every old Message, comes back as it is"
        (is (= "plain [[text]]" (refs/render db p "plain [[text]]" nil)))
        (is (= "plain" (refs/text-of db p {:message/text {:field/value "plain"}} :message/text))))
      (testing "a span that no longer fits is left out, the rest stays"
        (is (= "hi" (refs/render db p "hi" [{:start 5 :end 9 :kind :object :object p :name "root"}])))))))

(deftest a-reader-gets-only-the-spans-it-can-resolve
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root)
          secret (get-in (ws/request!! root :act {:class :object :action :create :args {:name "vault.plan" :fields {:notes/x "y"}}}) [:result :object])
          route (id root "root.route")
          _ (fx/grant! root (:id witness) #{route} #{:read :transmit})
          text "see vault.plan and root"
          spans [{:start 4 :end 14 :kind :object :object secret :name "vault.plan"}
                 {:start 19 :end 23 :kind :object :object p :name "root"}]
          c (fx/attach-as world (:token witness))
          _ (ws/request!! c :subscribe {:sub "in" :stream (:id witness) :kinds #{:created :delivered}})
          _ (fx/grant! root (:id witness) #{p} #{:read})
          sent (ws/request!! root :send {:to #{(:id witness)} :route route :text text :refs spans})
          m (get-in sent [:result :message])]
      (is (:ok sent) (pr-str sent))
      (testing "the sender keeps both"
        (is (= 2 (count (get-in (ws/request!! root :read {:object m}) [:result :object/fields :message/refs :field/value])))))
      (testing "read: the text whole, the span to what it cannot resolve gone"
        (let [f (get-in (ws/request!! c :read {:object m}) [:result :object/fields])]
          (is (= text (get-in f [:message/text :field/value])))
          (is (= [p] (mapv :object (get-in f [:message/refs :field/value]))))))
      (testing "history, on the Message's stream"
        (let [e (first (filter #(= :created (:event/kind %)) (get-in (ws/request!! c :history {:stream m}) [:result :events])))]
          (is (= [p] (mapv :object (get-in e [:event/body :fields :message/refs]))) (pr-str e))))
      (testing "the snapshot"
        (let [objs (get-in (ws/request!! c :history {:snapshot true}) [:result :snapshot :objects])
              o (first (filter #(= m (:object/id %)) objs))]
          (is (= [p] (mapv :object (get-in o [:object/fields :message/refs :field/value]))) (pr-str (keys o)))))
      (testing "a push"
        (let [e (->> (repeatedly 4 #(ws/await-push c "in" 2000)) (mapcat :events)
                     (filter #(= m (get-in % [:event/body :id]))) first)]
          (when e (is (= [p] (mapv :object (get-in e [:event/body :fields :message/refs]))) (pr-str e)))
          (is (not (str/includes? (pr-str (refs/visible-event (act/db world) (:id witness)
                                                              {:event/kind :created :event/body {:fields {:message/refs spans}}}))
                                  (str secret))))))
      (testing "a person's send is checked against the text and against resolution, and tags agree"
        (is (:err (ws/request!! c :send {:to #{p} :route route :text "hi" :refs [{:start 0 :end 40 :kind :object :object p :name "root"}]})))
        (is (:err (ws/request!! c :send {:to #{p} :route route :text "vault.plan"
                                         :refs [{:start 0 :end 10 :kind :object :object secret :name "vault.plan"}]})))
        (let [s (ws/request!! c :send {:to #{p} :route route :text "root" :refs [{:start 0 :end 4 :kind :object :object p :name "root"}]})
              f (get-in (ws/request!! c :read {:object (get-in s [:result :message])}) [:result :object/fields])]
          (is (:ok s) (pr-str s))
          (is (= [p] (get-in f [:message/tags :field/value]))))))))

(deftest short-ids-in-ops
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          route (id root "root.route")
          adapter (id root "root.model")
          db (act/db world)
          table (refs/handles {} [p route adapter])
          h (fn [o] (some (fn [[k v]] (when (= v (str o)) k)) table))
          ex (fn [op args] (first (lops/expand-ops db p table [{:op op :args args}])))]
      (testing "every op shape, nested action args and lists"
        (is (= (str p) (get-in (ex :read {:object (h p)}) [:args :object])))
        (is (= (str "/" p) (get-in (ex :read {:name (h p)}) [:args :name])) "read by name takes a handle")
        (is (= [(str p)] (get-in (ex :send {:to [(h p)] :route (h route) :text "x"}) [:args :to])))
        (is (= (str route) (get-in (ex :send {:to [(h p)] :route (h route) :text "x"}) [:args :route])))
        (is (= (str adapter) (get-in (ex :act {:action "task.run" :args {:adapter (h adapter) :cmd (h p)}}) [:args :args :adapter])))
        (is (= (h p) (get-in (ex :act {:action "task.run" :args {:adapter (h adapter) :cmd (h p)}}) [:args :args :cmd]))
            "a command is not an id position")
        (is (= [(str p) (str route)] (get-in (ex :act {:action "view.table-of" :args {:objects [(h p) (h route)]}}) [:args :args :objects])))
        (is (= (str p) (get-in (ex :set-field {:object "root" :field "notes/a" :from {:object (h p)}}) [:args :from :object])))
        (is (= (str p) (get-in (ex :grant {:principal (h p) :ops ["read"]}) [:args :principal]))))
      (testing "a full id passes, and a name where an id goes is that id"
        (is (= (str p) (get-in (ex :read {:object (str p)}) [:args :object])))
        (is (= (str p) (get-in (ex :read {:object "root"}) [:args :object])))
        (is (uuid? (parse-uuid (get-in (ex :act {:object "root.model" :action "run"}) [:args :object])))))
      (testing "an unknown handle answers err unknown-id, saying what it could mean"
        (let [{:keys [err]} (ex :read {:object "ffff"})]
          (is (= :unknown-id (:code err))))
        (let [long-form (subs (str p) 0 8)
              {:keys [err]} (ex :read {:object long-form})]
          (is (= :unknown-id (:code err)) "a prefix is not a handle")
          (is (str/includes? (str (:could-mean err)) (str p)))))
      (testing "a stale handle from an older wake errors, it never resolves to something else"
        (let [old (refs/handles {} [p])
              stale (first (keys old))
              ;; the new wake's table has other ids and not that handle
              fresh (dissoc (refs/handles {} [route adapter]) stale)
              {:keys [err]} (first (lops/expand-ops db p fresh [{:op :read :args {:object stale}}]))]
          (is (= :unknown-id (:code err)))))
      (testing "text in an op becomes display text and spans"
        (let [a (:args (ex :send {:to [(h p)] :route (h route) :text (str "[[" (h p) "]] and [[zz9]]")}))]
          (is (= "root and [[zz9]]" (:text a)))
          (is (= p (:object (first (:refs a)))))
          (is (= ["[[zz9]]"] (:unresolved-refs a)))))
      (testing "no table, no change: a session's ops are not a wake's"
        (is (= {:op :read :args {:object "ab"}} (first (lops/expand-ops db p nil [{:op :read :args {:object "ab"}}]))))))))

(deftest a-prompt-speaks-in-handles
  (fx/with-world [{:keys [world root]}]
    (let [p (:principal root)
          model (id root "root.model")
          r (ws/request!! root :serialize {:working-set #{p} :model model})
          s (:result (ws/request!! root :read {:object (get-in r [:result :serialization])}))
          text (ser/bytes-of world s)
          ids (ser/ids-of world s)]
      (is (not (re-find refs/uuid-re text)) "no id in full")
      (is (seq ids))
      (is (every? #(<= 2 (count %)) (keys ids)))
      (is (some #{(str p)} (vals ids))))))

(deftest a-wake-writes-references-and-keeps-its-table
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root)
          route (id root "root.route")
          w (fx/attach-as world (:token witness))
          _ (fx/grant! root (:id witness) #{(:id witness) route} #{:read :transmit})
          _ (fx/grant! root p #{route} #{:transmit})
          line (str "{\"op\": \"send\", \"args\": {\"to\": [\"" (:id witness) "\"], \"route\": \"" route
                    "\", \"text\": \"see [[" p "]] and [[zz9]]\"}}")
          sent (ws/request!! w :send {:to #{p} :route route :text (str "ask:\n```ops\n" line "\n```")})
          texts (fn [] (let [db (act/db world)]
                         (for [m (objects/objects-of-class db :message)
                               :let [f (objects/fields db m)]
                               :when (and (= p (get-in f [:message/from :field/value]))
                                          (str/starts-with? (str (get-in f [:message/text :field/value])) "see root"))]
                           f)))]
      (is (:ok sent) (pr-str sent))
      (is (until #(seq (texts)) 8000) "the op's text was sent, through its handle")
      (let [f (first (texts))]
        (is (= "see root and [[zz9]]" (get-in f [:message/text :field/value])) "the log holds no handle")
        (is (= [p] (mapv :object (get-in f [:message/refs :field/value]))))
        (is (= ["[[zz9]]"] (get-in f [:message/unresolved-refs :field/value]))))
      (testing "every Serialization of the wake keeps the handles of the one before"
        (is (until #(<= 2 (count (filter (fn [s] (seq (get-in (objects/fields (act/db world) s) [:serialization/ids :field/value])))
                                         (objects/objects-of-class (act/db world) :serialization)))) 8000))
        ;; within one wake: a table is a wake's, so two wakes may give one id different handles, and with
        ;; two hex characters to go on random ids collide often enough that comparing across wakes flaked
        (let [db (act/db world)
              fv (fn [e k] (get-in (objects/fields db e) [k :field/value]))
              wakes (->> (objects/objects-with-field db :evaluation/message)
                         (group-by #(fv % :evaluation/message))
                         vals)]
          (doseq [evaluations wakes
                  :let [tables (->> evaluations
                                    (sort-by #(:object/created-seq (objects/entity db %)))
                                    (keep #(fv % :evaluation/serialization))
                                    (map #(ser/ids-of world (objects/state db %)))
                                    (filter seq))]
                  [a b] (partition 2 1 tables)
                  [h i] a]
            (is (= i (b h)) "a handle never changes its meaning")))))))

(deftest a-summary-keeps-its-references
  (fx/with-world [{:keys [world root witness]}]
    (let [p (:principal root)
          route (id root "root.route")
          w (fx/attach-as world (:token witness))
          _ (fx/grant! root (:id witness) #{(:id witness) route} #{:read :transmit})
          say (fn [t] (get-in (ws/request!! w :send {:to #{p} :route route :text t}) [:result :message]))
          answered (fn [m] (until #(some (fn [x] (= m (get-in (objects/fields (act/db world) x) [:message/in-reply-to :field/value])))
                                         (objects/objects-of-class (act/db world) :message)) 8000))
          lines (vec (for [i (range 7)] (let [m (say (str "line " i))] (answered m) m)))
          t (get-in (objects/fields (act/db world) (first lines)) [:message/thread :field/value])
          t (or t (first lines))
          e (random-uuid)
          table {"a1" (str p)}
          r (with-redefs [evaluate/evaluate! (fn [_ _] {:ok {:evaluation e :ids table
                                                             :done (doto (promise) (deliver {:state :done :text "[[a1|root]] kept the line, [[seq:7]]"}))}})]
              (conv/compress! world p t nil))
          f (objects/fields (act/db world) t)]
      (is (:ok r) (pr-str r))
      (is (= "root kept the line, seq:7" (get-in f [:conversation/summary :field/value])))
      (is (= [:object :seq] (mapv :kind (get-in f [:conversation/summary/refs :field/value]))))
      (testing "and it renders as references again, for the next prompt to shorten"
        (let [body (:body (ser/render (act/db world) {:principal p :working-set #{p (peek lines)} :route route :message (peek lines)}))]
          (is (str/includes? body (str "[[" p "|root]] kept the line, [[seq:7]]")) body))))))

(deftest a-reference-never-reaches-a-person-raw
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          p (:principal root)]
      (testing "three parts with no table: the name resolves it"
        (let [{:keys [text]} (refs/parse db p {} "ask [[0000|root|principal]] now")]
          (is (= "ask root now" text))))
      (testing "nothing resolves: what it was called, without the brackets"
        (let [{:keys [text unresolved]} (refs/parse db p {} "ask [[0000|nobody here|principal]] now")]
          (is (= "ask nobody here now" text))
          (is (= ["[[0000|nobody here|principal]]"] unresolved))))
      (testing "a handle from another wake is still its id's prefix"
        (let [h (subs (str p) 0 8)
              {:keys [text refs]} (refs/parse db p {} (str "ask [[" h "]] now"))]
          (is (= "ask root now" text))
          (is (= p (:object (first refs)))))))))

(deftest a-name-beside-its-reference-reads-once
  (fx/with-world [{:keys [world root]}]
    (let [db (act/db world)
          id (:principal root)
          n (refs/display-name db id id)
          table {"aa" (str id)}
          {:keys [text refs]} (refs/parse db id table (str "through " n " [[aa]] today"))]
      (is (= (str "through " n " today") text))
      (is (= 1 (count refs)))
      (is (= (str "see " n) (:text (refs/parse db id table "see [[aa]]")))))))
