(ns knowledge.world.act.unsafe-test
  "Unsafe mode: one Principal, a limited time, the record kept (decisions 2026-10-31)."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.act.unsafe :as unsafe]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.loop.ops :as lops]
            [knowledge.world.ws-client :as ws])
  (:import [java.util Date]))

(defn- field [world id k] (get-in (objects/fields (act/db world) id) [k :field/value]))
(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))
(defn- resolved [root n] (get-in (ws/request!! root :resolve {:name n :op :read}) [:result :object]))
(defn- ops-block [& lines] (str "```ops\n" (str/join "\n" lines) "\n```"))

(defn- later [minutes] (Date. (+ (System/currentTimeMillis) (* 60000 minutes))))

(def ^:private person-actions
  ["approval.unsafe" "approval.unsafe.off" "field.clear" "task.approve" "task.deny" "task.decide-many"])

(defn- person!
  "witness attached, may write `targets`, may call the person's actions, reads root.route. Returns the client."
  [world root witness targets]
  (doseq [a person-actions]
    (fx/grant! root (:id witness) #{(resolved root (str "action/" a))} #{:read :evaluate}))
  (fx/grant! root (:id witness) (set targets) #{:read :write})
  (fx/grant! root (:id witness) #{(:id witness)} #{:read})
  (fx/grant! root (:id witness) #{(resolved root "root.route")} #{:transmit})
  (fx/attach-as world (:token witness)))

(defn- process!
  "A process Principal whose only gate word is set-field, and which may make the Task that records it."
  [world root name]
  (let [id (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :process :name name}})
                   [:result :object])]
    (fx/grant! root id #{id} #{:read :write})
    (fx/grant! root id #{(resolved root "schema.Task")} #{:read :create})
    (fx/policy! world root {:object id :field :policy/approval :type [:list :string] :value ["set-field"] :reader :both})
    id))

(defn- session-of!
  "A socket session of Principal p."
  [world root p]
  (fx/attach-as world (get-in (ws/request!! root :act {:action :token.issue :args {:principal (str p) :label "t"}})
                              [:result :result :secret])))

(defn- note-op [p v] {:op :set-field :args {:object p :field :note/x :type :string :value v :reader :both}})

(defn- run-note!
  "p sets a Field of its own as a gated op, woken by message. Returns the op's result."
  [world p message v]
  (first (lops/execute! world p nil 1 [(note-op p v)] {:message message})))

(defn- said-by-person
  "A Message from witness, the wake of work a person asked for."
  [w root-id route]
  (get-in (ws/request!! w :send {:to #{root-id} :route route :text "go"}) [:result :message]))

(defn- said-by-root
  "A Message whose sender and thread are root's: a process that is not armed."
  [root witness route]
  (get-in (ws/request!! root :send {:to #{(:id witness)} :route route :text "go"}) [:result :message]))

(defn- pending? [r] (= :approval-pending (some-> (get-in r [:err :code]) name keyword)))

(defn- approval-tasks [world]
  (let [db (act/db world)]
    (sort-by #(or (:object/created-seq (objects/entity db %)) 0)
             (filter #(= "approval" (field world % :task/kind)) (objects/objects-of-class db :task)))))

(defn- listing [world] (or (field world (:world-id world) :world/unsafe) []))

(deftest arming-is-a-persons-and-it-is-bounded
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          u (process! world root "u")
          w (person! world root witness [u root-id])
          u-client (session-of! world root u)
          args {:principal (str u) :minutes 10}]
      (testing "not from a process's own session, on itself or another"
        (is (= :person-only (code (act! root "approval.unsafe" args))))
        (is (= :person-only (code (act! root "approval.unsafe" {:principal (str root-id) :minutes 10}))))
        (is (= :person-only (code (act! root "approval.unsafe.off" {:principal (str u)})))))
      (testing "not from a loop, root included, nor by writing the Field"
        (doseq [p [u root-id]]
          (fx/grant! root p #{(resolved root "action/approval.unsafe")} #{:read :evaluate})
          (let [[a] (lops/execute! world p nil 1 [{:op :act :args {:action :approval.unsafe :args {:principal (str p) :minutes 10}}}] {})]
            (is (= :person-only (keyword (name (get-in a [:err :code])))) (pr-str a))))
        (let [[b] (lops/execute! world root-id nil 1
                                 [{:op :set-field :args {:object root-id :field :policy/unsafe :type :map :reader :both
                                                         :value {:on true :until "2999-01-01T00:00:00Z"}}}]
                                 {})]
          (is (= :person-only (keyword (name (get-in b [:err :code])))) (pr-str b)))
        (is (= :person-only (code (ws/request!! root :set-field {:object root-id :field :policy/unsafe :type :map
                                                                 :value {:on true} :reader :both})))))
      (is (nil? (field world u :policy/unsafe)))
      (testing "a person arms a process and nobody else"
        (fx/grant! root (:id witness) #{(:id witness)} #{:write})
        (is (= :invalid-args (code (act! w "approval.unsafe" {:principal (str (:id witness)) :minutes 10})))
            "a person has no gate to lift, on themselves least of all")
        (is (= :invalid-args (code (act! w "approval.unsafe" {:minutes 10}))) "no all-Principals form"))
      (testing "the time is required and bounded"
        (doseq [bad [{:principal (str u)} {:principal (str u) :minutes 0} {:principal (str u) :minutes 481}]]
          (is (= :invalid-args (code (act! w "approval.unsafe" bad))) (pr-str bad))))
      (testing "not by writing the Field either, even a person: it would have no limit"
        (is (= :conflict (code (ws/request!! w :set-field {:object u :field :policy/unsafe :type :map :reader :both
                                                           :value {:on true :until "2999-01-01T00:00:00Z" :by (str (:id witness))
                                                                   :at "2020-01-01T00:00:00Z"}})))))
      (testing "a forged Field does not count"
        (let [now (Date.) by (str (:id witness))
              forged (fn [v] (fx/policy! world root {:object u :field :policy/unsafe :type :map :value v :reader :both})
                       (unsafe/live? (act/db world) u now))]
          (is (false? (forged {:on true :by by :at "2020-01-01T00:00:00Z"})) "no end")
          (is (false? (forged {:on true :by by :at "2026-01-01T00:00:00Z" :until "2999-01-01T00:00:00Z"})) "too long")
          (is (false? (forged {:on true :by by :at "2999-01-01T00:00:00Z" :until "2999-01-01T01:00:00Z"})) "not yet armed")
          (is (false? (forged {:on true :by (str root-id) :at (str (.toInstant now))
                               :until (str (.toInstant (later 5)))})) "not armed by a person")
          (act/append! world (log/pending :field-set u (:world-id world)
                                                               {:name :policy/unsafe :type :string :value "" :reader :both :cleared true}))))
      (is (:ok (act! w "approval.unsafe" {:principal (str u) :minutes 480})))
      (is (true? (unsafe/live? (act/db world) u (Date.))))
      (ws/close! w)
      (ws/close! u-client))))

(deftest the-gated-op-runs-with-its-record-and-the-mode-ends
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolved root "root.route")
          u (process! world root "u")
          w (person! world root witness [u])
          wake (said-by-person w root-id route)]
      (testing "without the mode, a card"
        (is (pending? (run-note! world u wake "before")))
        (is (nil? (field world u :note/x))))
      (let [r (act! w "approval.unsafe" {:principal (str u) :minutes 30})
            armed (get-in r [:result :result])]
        (is (:ok r) (pr-str r))
        (is (= unsafe/default-except (:except armed)))
        (is (= (str (:id witness)) (:by armed)))
        (is (= armed (assoc (field world u :policy/unsafe) :principal u))))
      (testing "with it, the op runs, nobody is asked, and the Task reads like an approval"
        (let [before (count (approval-tasks world))
              r (run-note! world u wake "during")
              t (last (approval-tasks world))]
          (is (:ok r) (pr-str r))
          (is (= "during" (field world u :note/x)))
          (is (= (inc before) (count (approval-tasks world))))
          (is (= "unsafe" (field world t :task/rule)))
          (is (= (:id witness) (field world t :task/decided-by)))
          (is (inst? (field world t :task/decided-at)))
          (is (= :done (field world t :task/state)))
          (is (= u (field world t :task/requested-by)))
          (is (= wake (field world t :task/message)))
          (is (true? (:ok (field world t :task/result))))
          (is (= t (:event/via (last (filter #(= :note/x (get-in % [:event/body :name]))
                                             (log/read-stream (:log world) u 0 10000)))))
              "the op's via is the Task")))
      (testing "off: the next one gets a card, and the Field leaves the fold"
        (is (:ok (act! w "approval.unsafe.off" {:principal (str u)})))
        (is (nil? (field world u :policy/unsafe)))
        (is (pending? (run-note! world u wake "after")))
        (is (= "during" (field world u :note/x)))
        (is (:ok (act! w "approval.unsafe.off" {:principal (str u)})) "off again is the same answer"))
      (ws/close! w))))

(deftest what-the-except-list-keeps-gated
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolved root "root.route")
          u (process! world root "u")
          w (person! world root witness [u])
          wake (said-by-person w root-id route)
          admits? (fn [op args] (some? (gates/admission (act/db world) u op args wake)))
          action (fn [n] {:action n :args {}})]
      (fx/policy! world root {:object u :field :policy/approval :type [:list :string]
                              :value ["set-field" "grant" "shell" "token.issue" "object.retire" "principal.create" "task.run"] :reader :both})
      (act! w "approval.unsafe" {:principal (str u) :minutes 30})
      (testing "the default list"
        (is (admits? :set-field {:object u :field :note/x}))
        (is (admits? :act (action :task.run)))
        (doseq [[op args] [[:grant {}] [:act (action :object.retire)] [:act (action :principal.create)]
                           [:act (action :token.issue)]]]
          (is (not (admits? op args)) (pr-str op args)))
        (testing "by the handler an alias reaches, and by the Field a set-field names"
          (let [o (get-in (ws/request!! root :act {:class :object :action :create
                                                   :args {:fields {:action/name "g2" :action/handler :object.retire}}})
                          [:result :object])]
            (is (:ok (ws/request!! root :bind {:namespace (objects/world-id (act/db world)) :name "action/g2" :object o})))
            (is (not (admits? :act (action :g2)))))
          (is (not (admits? :set-field {:object u :field :policy/approval}))))
        (testing "a card, in fact"
          (ws/request!! root :grant {:grantee u :scope #{u} :ops #{:read :delegate} :delegable true})
          (let [r (first (lops/execute! world u nil 1 [{:op :grant :args {:grantee u :scope #{u} :ops #{:read}}}] {:message wake}))]
            (is (pending? r) (pr-str r)))))
      (testing "a person's own list"
        (act! w "approval.unsafe" {:principal (str u) :minutes 30 :except ["shell"]})
        (is (not (admits? :act (action :task.run))) "shell and task.run are one family")
        (is (admits? :set-field {:object u :field :note/x})))
      (testing "a person's mark holds whatever the list says"
        (let [a (resolved root "action/task.run")]
          (act! w "approval.unsafe" {:principal (str u) :minutes 30 :except []})
          (is (admits? :act (action :task.run)))
          (fx/policy! world root {:object a :field :policy/never-unsafe :type :boolean :value true :reader :both})
          (is (not (admits? :act (action :task.run))))))
      (ws/close! w))))

(deftest it-belongs-to-one-principal
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolved root "root.route")
          u (process! world root "u")
          v (process! world root "v")
          w (person! world root witness [u v])
          from-person (said-by-person w root-id route)]
      (act! w "approval.unsafe" {:principal (str u) :minutes 30})
      (testing "another Principal stays gated"
        (is (:ok (run-note! world u from-person "u")))
        (is (pending? (run-note! world v from-person "v"))))
      (testing "what u asks of v is v's, under v's gates"
        (let [wake (get-in (ws/request!! (session-of! world root u)
                                         :send {:to #{(:id witness)} :route (do (fx/grant! root u #{route} #{:transmit}) route) :text "go"})
                           [:result :message])]
          (is (= u (field world wake :message/from)))
          (is (uuid? wake))
          (is (pending? (run-note! world v wake "from u")))
          (is (:ok (run-note! world u wake "from itself")) "and u's own work is still u's")))
      (testing "work a gated Principal starts does not get through an armed one"
        (is (pending? (run-note! world u (said-by-root root witness route) "from root"))))
      (testing "no wake, nothing to show whose work it is"
        (is (pending? (run-note! world u nil "no wake"))))
      (testing "an armed Principal may ask another armed one"
        (fx/grant! root (:id witness) #{root-id} #{:read :write})
        (act! w "approval.unsafe" {:principal (str root-id) :minutes 30})
        (is (:ok (run-note! world u (said-by-root root witness route) "from armed root"))))
      (ws/close! w))))

(deftest world-unsafe-tracks-every-armed-principal
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          u (process! world root "u")
          v (process! world root "v")
          w (person! world root witness [u v])
          bystander (process! world root "bystander")
          b (session-of! world root bystander)
          wid (:world-id world)]
      (ws/request!! b :subscribe {:sub "w" :stream wid :kinds #{:field-set} :after (:as-of (:result (ws/request!! b :subscribe {:sub "probe" :stream wid})))})
      (is (= [] (listing world)))
      (testing "arm"
        (act! w "approval.unsafe" {:principal (str u) :minutes 30})
        (act! w "approval.unsafe" {:principal (str v) :minutes 60})
        (is (= (sort [(str u) (str v)]) (sort (map :principal (listing world)))))
        (is (every? #(= (str (:id witness)) (:by %)) (listing world)))
        (is (= (get-in (objects/fields (act/db world) u) [:policy/unsafe :field/value :until])
               (:until (first (filter #(= (str u) (:principal %)) (listing world)))))))
      (testing "any session reads it, and nothing else of the world"
        (let [r (ws/request!! b :read {:object wid :field :world/unsafe})]
          (is (= (listing world) (get-in r [:result :object/fields :world/unsafe :field/value])) (pr-str r)))
        (is (= :unresolved (code (ws/request!! b :read {:object wid :field :world/name}))))
        (is (= :unresolved (code (ws/request!! b :read {:object wid})))))
      (testing "and sees it change on one subscription"
        (let [push (ws/await-push b "w" 5000)]
          (is (some #(= :world/unsafe (get-in % [:event/body :name])) (:events push)) (pr-str push))
          (is (every? #(= :world/unsafe (get-in % [:event/body :name])) (:events push)))))
      (testing "off"
        (act! w "approval.unsafe.off" {:principal (str u)})
        (is (= [(str v)] (mapv :principal (listing world)))))
      (testing "nobody writes it by hand, root included"
        (is (= :conflict (code (ws/request!! root :set-field {:object wid :field :world/unsafe :type [:list :map] :value [] :reader :both}))))
        (is (= :conflict (code (act! w "field.clear" {:object (str v) :field "policy/unsafe"}))) "not cleared past the listing"))
      (ws/close! b)
      (ws/close! w))))

(deftest time-running-out-ends-it-and-tells-who-armed-it
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          route (resolved root "root.route")
          u (process! world root "u")
          v (process! world root "v")
          w (person! world root witness [u v])
          wake (said-by-person w root-id route)]
      (ws/request!! w :subscribe {:sub "inbox" :stream (:id witness) :kinds #{:delivered}})
      (act! w "approval.unsafe" {:principal (str u) :minutes 1})
      (act! w "approval.unsafe" {:principal (str v) :minutes 60})
      (testing "checked at use, before any sweep"
        (is (true? (unsafe/live? (act/db world) u (later 0))))
        (is (false? (unsafe/live? (act/db world) u (later 2)))))
      (testing "the sweep leaves what is still armed"
        (is (= [] (gates/expire-unsafe! world (Date.))))
        (is (= 2 (count (listing world)))))
      (testing "the sweep clears it, the listing follows, and the person is told"
        (is (= [u] (gates/expire-unsafe! world (later 2))))
        (is (nil? (field world u :policy/unsafe)))
        (is (some? (field world v :policy/unsafe)))
        (is (= [(str v)] (mapv :principal (listing world))))
        (let [push (ws/await-push w "inbox" 5000)
              m (some #(get-in % [:event/body :message]) (:events push))]
          (is (= :status (some-> (field world m :message/kind) name keyword)))
          (is (= #{(:id witness)} (set (field world m :message/to))))
          (is (str/includes? (field world m :message/text) "unsafe mode on u"))))
      (testing "a gated op is back to a card"
        (is (pending? (run-note! world u wake "late"))))
      (ws/close! w))))

(deftest off-the-record-is-unaffected
  (fx/with-world [{:keys [world root witness]}]
    (let [u (process! world root "u")
          w (person! world root witness [u])]
      (act! w "approval.unsafe" {:principal (str u) :minutes 30})
      (testing "a scratch world neither gates nor admits: its own refusals are the rule there"
        (let [r (first (lops/execute! (assoc world :scratch {:principal u}) u nil 1
                                      [{:op :grant :args {:grantee u :scope #{u} :ops #{:read}}}] {}))]
          (is (= :scratch-readonly (keyword (name (get-in r [:err :code])))) (pr-str r))))
      (is (empty? (approval-tasks world)))
      (ws/close! w))))
