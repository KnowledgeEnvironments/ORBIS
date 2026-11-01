(ns knowledge.world.view.core-test
  "Views (plan W11, decisions 2026-10-30 'views'): present makes a Serialization of kind view and a
   Message of kind view naming it; a 10k-row table is clipped at 500 with view/truncated; big data
   goes to a blob and reads back by range; view.table-of and view.graph-of build the data from what
   the caller can read; a Principal without transmit on the Route is unresolved; a loop presents
   through the same actions and the recipient receives the view Message."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.evaluate.core-test :refer [until]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))

(defn- table [n]
  {:columns [{:key "name" :label "Name"} {:key "n" :label "N"}]
   :rows (vec (for [i (range n)] [(str "row-" i) i]))})

(deftest present-makes-a-view-serialization-and-a-view-message
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          r (ws/request!! root :present {:to #{(:id witness)} :route route :kind :table :title "two rows"
                                         :data (table 2) :about [root-id]})]
      (is (:ok r) (pr-str r))
      (let [{:keys [serialization message truncated]} (:result r)
            sf (fields root serialization)
            mf (fields root message)]
        (testing "the Serialization: kind view, the table, its title, provenance, inline data"
          (is (= :view (v sf :serialization/kind)))
          (is (= :table (v sf :view/kind)))
          (is (= "two rows" (v sf :view/title)))
          (is (= [root-id] (v sf :view/about)))
          (is (false? truncated))
          (is (nil? (v sf :view/truncated)))
          (is (map? (v sf :view/data)))
          (is (= 2 (count (:rows (v sf :view/data)))))
          (is (= ["name" "n"] (mapv :key (:columns (v sf :view/data)))))
          (is (string? (v sf :serialization/hash))))
        (testing "the Message: kind view, naming the view, text = title, on the Route, to the witness"
          (is (= :view (v mf :message/kind)))
          (is (= serialization (v mf :message/view)))
          (is (= "two rows" (v mf :message/text)))
          (is (= route (v mf :message/route)))
          (is (= #{(:id witness)} (v mf :message/to))))))))

(deftest a-huge-table-is-clipped-and-its-data-lives-in-a-blob-readable-by-range
  (fx/with-world [{:keys [root witness]}]
    (let [route (resolve-as root "root.route" :read)
          r (ws/request!! root :present {:to #{(:id witness)} :route route :kind :table :title "ten thousand"
                                         :data (table 10000)})]
      (is (:ok r) (pr-str r))
      (is (true? (get-in r [:result :truncated])))
      (let [ser (get-in r [:result :serialization])
            sf (fields root ser)
            data (v sf :view/data)]
        (is (true? (v sf :view/truncated)))
        (is (and (map? data) (string? (:blob/sha256 data)) (int? (:blob/size data))) "the data is a blob receipt")
        (let [page (ws/request!! root :read {:object ser :field :view/data :range [1 1]})
              text (get-in page [:result :object/fields :view/data :field/value])]
          (is (:ok page) (pr-str page))
          (is (string? text))
          (is (str/starts-with? text "{\"columns\""))
          (is (str/includes? text "\"total\":10000"))
          (is (= 500 (count (re-seq #"row-\d+" text)))))))))

(deftest table-of-and-graph-of-build-the-data-from-what-the-caller-reads
  (fx/with-world [{:keys [root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          world-id (resolve-as root "world" :read)
          r (act! root "view.table-of" {:objects [(str root-id) (str world-id) (str (:id witness))]
                                        :fields ["loop/stalled" "memory/budget"]
                                        :to [(str (:id witness))] :route (str route)})]
      (is (:ok r) (pr-str r))
      (let [sf (fields root (get-in r [:result :object]))
            data (v sf :view/data)]
        (testing "columns name, class, then the Fields asked; one row per Object"
          (is (= ["name" "class" "loop/stalled" "memory/budget"] (mapv :key (:columns data))))
          (is (= 3 (count (:rows data))))
          (let [by-name (into {} (map (fn [[n & more]] [n (vec more)])) (:rows data))]
            (is (= "principal" (first (by-name "root"))))
            (is (= 4000 (last (by-name "root"))) "root's memory/budget default")
            (is (= "" (last (by-name "test/witness"))) "a person has no memory Fields: an empty cell")))
        (is (= 3 (count (v sf :view/about)))))
      (let [g (act! root "view.graph-of" {:root (str root-id) :depth 1 :to [(str (:id witness))] :route (str route)})]
        (is (:ok g) (pr-str g))
        (let [sf (fields root (get-in g [:result :object]))
              {:keys [nodes edges]} (v sf :view/data)
              labels (set (map :label nodes))
              edge-labels (set (map :label edges))]
          (testing "root's model, adapters and routes are nodes, with edges named by the Field"
            (is (contains? labels "root"))
            (is (contains? labels "root.model"))
            (is (contains? labels "harness"))
            (is (contains? labels "root.route"))
            (is (contains? edge-labels "principal/models"))
            (is (contains? edge-labels "principal/adapters"))
            (is (contains? edge-labels "principal/routes")))
          (is (every? #(= (str root-id) (:from %)) edges) "depth 1: every edge leaves root"))))))

(deftest a-person-posts-a-view-and-an-attachment-is-one-op-and-a-read-can-skip-payloads
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (fx/grant! root (:id witness) #{(resolve-as root "schema.Object" :read)} #{:create :read})
      (testing "send with kind view names a view Object the person authored (a tagged Object is a view to the renderer)"
        (let [obj (ws/request!! w :act {:class :object :action :create
                                        :args {:fields {:view/kind :table :view/title "mine"
                                                        :view/data {:columns [{:key "a" :label "a"}] :rows [[1]]}}}})
              _ (is (:ok obj) (pr-str obj))
              view (get-in obj [:result :object])
              sent (ws/request!! w :send {:to #{root-id} :route route :text "mine" :kind :view :view view})
              _ (is (:ok sent) (pr-str sent))
              mf (fields root (get-in sent [:result :message]))]
          (is (= :view (v mf :message/kind)))
          (is (= view (v mf :message/view)))))
      (testing "a typed bytes Field at create: decoded from base64, blobbed above the threshold, one op"
        (let [big (.encodeToString (java.util.Base64/getEncoder) (byte-array (repeat 10000 (byte 7))))
              small (.encodeToString (java.util.Base64/getEncoder) (.getBytes "hello" "UTF-8"))
              r (ws/request!! root :act {:class :object :action :create
                                         :args {:fields {:image/mime "image/png"
                                                         :image/bytes {:type :bytes :value big}
                                                         :thumb/bytes {:type :bytes :value small}
                                                         :image/title "typed"}}})
              _ (is (:ok r) (pr-str r))
              id (get-in r [:result :object])
              full (ws/request!! root :read {:object id})
              fs (get-in full [:result :object/fields])
              blobs (get-in full [:result :object/blobs])]
          (is (= "typed" (v fs :image/title)))
          (is (= :bytes (get-in fs [:image/bytes :field/type])))
          (is (and (map? (v fs :image/bytes)) (= 10000 (:blob/size (v fs :image/bytes)))) "the big one is a receipt")
          (is (bytes? (v fs :thumb/bytes)) "the small one is inline bytes")
          (is (= 10000 (get-in blobs [:image/bytes :size])))
          (is (= "image/png" (get-in blobs [:image/bytes :mime])) "mime from the sibling image/mime Field")
          (is (= 5 (get-in blobs [:thumb/bytes :size])))
          (is (true? (get-in blobs [:thumb/bytes :inline])))
          (testing "bytes false leaves the payload out, the summary stays"
            (let [lean (ws/request!! root :read {:object id :bytes false})
                  lf (get-in lean [:result :object/fields])]
              (is (not (bytes? (v lf :thumb/bytes))))
              (is (= 5 (:blob/size (v lf :thumb/bytes))))
              (is (= 5 (get-in lean [:result :object/blobs :thumb/bytes :size])))))))
      (ws/close! w))))

(deftest a-principal-without-transmit-on-the-route-is-unresolved
  (fx/with-world [{:keys [world root witness]}]
    (let [route (resolve-as root "root.route" :read)
          w (fx/attach-as world (:token witness))
          ser-schema (resolve-as root "schema.Serialization" :read)]
      ;; create on the schema but no transmit on the Route: the view is refused before anything is written
      (fx/grant! root (:id witness) #{ser-schema} #{:create :read})
      (let [r (ws/request!! w :present {:to #{(:principal root)} :route route :kind :text :title "hi" :data {:markdown "hi"}})]
        (is (= :unresolved (get-in r [:err :code])) (pr-str r)))
      (ws/close! w))))

(deftest an-identical-view-op-later-in-the-wake-is-not-sent-twice
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      ;; the stub emits block k on iteration k: the same table twice, then prose
      (let [line (str "{\"op\": \"act\", \"args\": {\"action\": \"view.table-of\", \"args\": {\"objects\": [\"" root-id "\"], "
                      "\"fields\": [\"principal/driven-by\"], \"title\": \"twice?\"}}}")
            block (str "```ops\n" line "\n```")
            sent (ws/request!! w :send {:to #{root-id} :route route :text (str "show me:\n" block "\n" block)})
            _ (is (:ok sent) (pr-str sent))
            delivered (atom [])
            reply (until (fn []
                           (when-let [push (ws/await-push w "inbox" 1500)]
                             (swap! delivered into (keep #(when (= :delivered (:event/kind %)) (get-in % [:event/body :message])) (:events push))))
                           (some (fn [m] (let [mf (fields root m)]
                                           (when (and (nil? (v mf :message/kind)) (= (get-in sent [:result :message]) (v mf :message/in-reply-to))) m)))
                                 @delivered))
                         30000)]
        (is (some? reply) (pr-str @delivered))
        (let [views (filter (fn [m] (= :view (v (fields root m) :message/kind))) @delivered)]
          (is (= 1 (count views)) "one view Message for two identical ops in one wake")))
      (ws/close! w))))

(deftest a-loop-presents-a-table-and-the-recipient-receives-the-view-message
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolve-as root "root.route" :read)
          w (fx/attach-as world (:token witness))]
      (fx/grant! root (:id witness) #{(:id witness)} #{:read})
      (fx/grant! root (:id witness) #{route} #{:transmit})
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (let [line (str "{\"op\": \"act\", \"args\": {\"action\": \"view.table-of\", \"args\": {\"objects\": [\"" root-id "\"], "
                      "\"fields\": [\"principal/driven-by\", \"loop/stalled\"], \"title\": \"root at a glance\"}}}")
            sent (ws/request!! w :send {:to #{root-id} :route route :text (str "show me:\n```ops\n" line "\n```")})
            _ (is (:ok sent) (pr-str sent))
            delivered (atom [])
            view-msg (until (fn []
                              (when-let [push (ws/await-push w "inbox" 1500)]
                                (swap! delivered into (keep #(when (= :delivered (:event/kind %)) (get-in % [:event/body :message])) (:events push))))
                              (some (fn [m] (let [mf (fields root m)] (when (= :view (v mf :message/kind)) m))) @delivered))
                            20000)]
        (is (some? view-msg) (pr-str @delivered))
        (when view-msg
          (let [mf (fields root view-msg)
                sf (fields root (v mf :message/view))]
            (testing "the loop's view: to the asker on the asking Route, in reply to the ask, via the Evaluation"
              (is (= #{(:id witness)} (v mf :message/to)))
              (is (= route (v mf :message/route)))
              (is (= (get-in sent [:result :message]) (v mf :message/in-reply-to)))
              (is (= "root at a glance" (v mf :message/text)))
              (is (= :table (v sf :view/kind)))
              (is (= ["name" "class" "principal/driven-by" "loop/stalled"] (mapv :key (:columns (v sf :view/data)))))
              (is (= [root-id] (v sf :view/about))))
            (testing "the recipient reads the view the Message carries, with no Grant on it (resolution step 6b)"
              (let [wf (fields w (v mf :message/view))]
                (is (= :table (v wf :view/kind)) (pr-str (keys wf)))
                (is (= "root at a glance" (v wf :view/title))))))))
      (ws/close! w))))
