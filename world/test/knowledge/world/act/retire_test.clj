(ns knowledge.world.act.retire-test
  "Delete means retire (decisions 2026-10-31): the log stays append-only, the Object stops resolving
   except by id for read, its names are free, and object.restore brings it back. field.clear takes
   a Field out of the fold and the prompt while its history stays."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.loop.core :as lcore]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- resolve-as [c n op] (get-in (ws/request!! c :resolve {:name n :op op}) [:result :object]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- code [r] (get-in r [:err :code]))
(defn- res [r] (let [x (:result r)] (or (:result x) x)))
(defn- rid [world] (objects/root-id (act/db world)))
(defn- thing!
  "An Object named n, created by the root session and bound in the world namespace."
  [root world n]
  (let [r (ws/request!! root :act {:class :object :action :create :args {:fields {:notes/x "hello"}}})
        id (or (get-in r [:result :object]) (:object (res r)))
        b (ws/request!! root :bind {:namespace (objects/world-id (act/db world)) :name n :object id})]
    (when-not (:ok b) (throw (ex-info "bind failed" {:r r :b b})))
    id))

(deftest retire-hides-an-object-and-frees-its-name
  (fx/with-world [{:keys [root world]}]
    (let [id (thing! root world "old.thing")
          r (act! root "object.retire" {:object (str id) :reason "done with it"})]
      (is (:ok r) (pr-str r))
      (testing "the name is gone, the id still reads, and the read says retired"
        (is (nil? (resolve-as root "old.thing" :read)))
        (let [rd (ws/request!! root :read {:object id})]
          (is (:ok rd) (pr-str rd))
          (is (true? (get-in rd [:result :object/retired])))))
      (testing "nothing else resolves"
        (is (some? (:err (ws/request!! root :set-field {:object id :field :notes/y :type :string :value "z"})))))
      (testing "the name can be used again"
        (let [again (thing! root world "old.thing")]
          (is (not= id again))
          (is (= again (resolve-as root "old.thing" :read)))))
      (testing "twice is refused"
        (is (= :conflict (code (act! root "object.retire" {:object (str id)}))))))))

(deftest restore-brings-it-back
  (fx/with-world [{:keys [root world]}]
    (let [id (thing! root world "keep.me")
          _ (act! root "object.retire" {:object "keep.me"})
          r (act! root "object.restore" {:object (str id)})]
      (is (:ok r) (pr-str r))
      (is (= id (resolve-as root "keep.me" :read)))
      (is (nil? (get-in (ws/request!! root :read {:object id}) [:result :object/retired])))
      (is (:ok (ws/request!! root :set-field {:object id :field :notes/y :type :string :value "z"})))
      (is (= :conflict (code (act! root "object.restore" {:object (str id)}))) "not retired any more")
      (testing "a name taken in the meantime is reported, not stolen"
        (act! root "object.retire" {:object (str id)})
        (let [other (thing! root world "keep.me")
              back (act! root "object.restore" {:object (str id)})]
          (is (:ok back) (pr-str back))
          (is (= ["keep.me"] (:names-taken (res back))))
          (is (= other (resolve-as root "keep.me" :read))))))))

(deftest what-the-world-needs-is-refused
  (fx/with-world [{:keys [root world]}]
    (let [db (act/db world)]
      (is (= :conflict (code (act! root "object.retire" {:object (str (objects/root-id db))}))))
      (is (= :conflict (code (act! root "object.retire" {:object (str (objects/world-id db))})))))))

(deftest a-used-object-names-its-dependents-unless-forced
  (fx/with-world [{:keys [root]}]
    (let [model (resolve-as root "root.model" :read)
          r (act! root "object.retire" {:object (str model)})]
      (is (= :conflict (code r)))
      (is (str/includes? (str (get-in r [:err :message])) "root") (pr-str r))
      (let [f (act! root "object.retire" {:object (str model) :force true})]
        (is (:ok f) (pr-str f))
        (is (seq (:dependents (res f))))))))

(deftest a-retired-route-refuses-send
  (fx/with-world [{:keys [root world]}]
    (let [made (act! root "route.create" {:name "spare.route" :members [(str (rid world))]})
          route (:route (res made))
          _ (act! root "object.retire" {:object (str route) :force true})
          r (ws/request!! root :send {:to #{(rid world)} :route route :text "hello"})]
      (is (uuid? route) (pr-str made))
      (is (= :conflict (code r)) (pr-str r)))))

(deftest a-cleared-field-leaves-reads-and-the-prompt
  (fx/with-world [{:keys [root world]}]
    (let [id (rid world)]
      (ws/request!! root :set-field {:object id :field :notes/secret :type :string :value "remember the milk"})
      (is (= "remember the milk" (get-in (fields root id) [:notes/secret :field/value])))
      (is (str/includes? (:body (ser/render (act/db world) {:principal id :working-set [id]})) "remember the milk"))
      (let [r (act! root "field.clear" {:object (str id) :field "notes/secret"})]
        (is (:ok r) (pr-str r)))
      (is (not (contains? (fields root id) :notes/secret)))
      (is (not (str/includes? (:body (ser/render (act/db world) {:principal id :working-set [id]})) "remember the milk")))
      (testing "history stays"
        (let [ev (get-in (ws/request!! root :history {:stream id :limit 1000}) [:result :events])]
          (is (some #(true? (get-in % [:event/body :cleared])) ev))))
      (is (= :conflict (code (act! root "field.clear" {:object (str id) :field "notes/secret"}))) "already gone")
      (is (= :conflict (code (act! root "field.clear" {:object (str id) :field "principal/namespace"}))))
      (testing "a set after a clear stands"
        (ws/request!! root :set-field {:object id :field :notes/secret :type :string :value "again"})
        (is (= "again" (get-in (fields root id) [:notes/secret :field/value])))))))

(deftest a-retired-object-leaves-the-prompt
  (fx/with-world [{:keys [root world]}]
    (let [id (thing! root world "gone.soon")]
      (is (str/includes? (:body (ser/render (act/db world) {:principal (rid world) :working-set [id]})) "hello"))
      (act! root "object.retire" {:object (str id)})
      (is (not (str/includes? (:body (ser/render (act/db world) {:principal (rid world) :working-set [id]})) "hello"))))))

(deftest a-snapshot-shows-retired-objects-only-when-asked
  (fx/with-world [{:keys [root world]}]
    (let [id (thing! root world "snap.thing")
          _ (act! root "object.retire" {:object (str id)})
          ids (fn [args] (set (map :object/id (get-in (ws/request!! root :history args) [:result :snapshot :objects]))))]
      (is (not (contains? (ids {:snapshot true :limit 1000}) id)))
      (is (contains? (ids {:snapshot true :limit 1000 :retired true}) id)))))

(deftest a-loop-needs-a-person-to-retire
  (fx/with-world [{:keys [root]}]
    (let [action (resolve-as root "action/object.retire" :read)]
      (is (true? (get-in (fields root action) [:action/gated :field/value]))))))

(deftest a-retired-adapter-refuses-runs
  (fx/with-world [{:keys [root world]}]
    (let [shell (resolve-as root "shell.local" :read)
          _ (act! root "object.retire" {:object (str shell) :force true})
          r (ws/request!! root :act {:object shell :action :run :args {:cmd "echo no"}})]
      (is (= :conflict (code r)) (pr-str r)))))

(deftest a-retired-principal-is-out-of-the-world
  (fx/with-world [{:keys [root world witness]}]
    (let [made (act! root "principal.create" {:name "leaver" :charter "x"
                                              :grant {:scope {:prefix "leaver"} :ops ["read"]}})
          id (:principal (res made))
          route (:route (res made))
          r (act! root "object.retire" {:object (str id)})]
      (is (:ok r) (pr-str r))
      (testing "nothing can be sent to it"
        (is (= :conflict (code (ws/request!! root :send {:to #{id} :route route :text "hello"})))))
      (testing "a wake already queued is dropped and counted"
        (lcore/cycle! world id {:message (random-uuid) :route route})
        (is (= 1 (get-in (objects/fields (act/db world) id) [:loop/dropped :field/value])))))))

(deftest a-retired-object-keeps-what-it-was-called
  (fx/with-world [{:keys [world root]}]
    (let [id (thing! root world "keepsake")]
      (is (:ok (act! root "object.retire" {:object (str id)})))
      (is (nil? (resolve-as root "keepsake" :read)) "the name is free")
      (is (= ["keepsake"] (get-in (fields root id) [:object/last-names :field/value])))
      (let [snap (ws/request!! root :history {:snapshot true :retired true :limit 1000})
            row (first (filter #(= id (or (:object/id %) (:object %))) (get-in snap [:result :snapshot :objects])))]
        (is (= ["keepsake"] (get-in row [:object/fields :object/last-names :field/value])) (pr-str (keys row)))))))

(deftest a-read-only-mount-says-so
  (fx/with-world [{:keys [world root]}]
    (let [db (fn [] (act/db world))
          dir (fx/temp-dir)
          harness (objects/lookup (db) (:world-id world) "harness")
          r (act! root "workspace.mount" {:principal "root" :path dir :name "shelf" :shell false :read-only true})
          fs (:fs (res r))]
      (is (:ok r) (pr-str r))
      (is (true? (get-in (objects/fields (db) harness) [:adapter/read-only :field/value])))
      (is (true? (get-in (objects/fields (db) (if (string? fs) (parse-uuid fs) fs)) [:adapter/read-only :field/value]))))))
