(ns knowledge.world.act.auth-test
  "Credentials as Objects and pairing: token.issue hands a session the secret and a
   loop only a file; attach with it names the token Object on the attached event; an expired or
   revoked token is refused with the reason; the legacy single hash still works and gains a token
   Object labelled legacy; token.list; pair.begin / pair.claim once, wrong codes counted, expiry;
   hello before attach."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.actions :as actions]
            [knowledge.world.act.core :as act]
            [knowledge.world.boot.kernel-zero :as kz]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.protocol.session :as session]
            [knowledge.world.tokens :as tokens]
            [knowledge.world.ws-client :as ws]))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args} 20000))
(defn- res-of [r] (get-in r [:result :result]))
(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- v [fs k] (get-in fs [k :field/value]))
(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn- attach-raw [world token]
  (let [c (ws/connect (:port world) :transit)
        r (ws/request!! c :attach {:token token :client-kind :test})]
    [c r]))

(deftest tokens-are-objects-and-attach-names-them
  (fx/with-world [{:keys [world root witness]}]
    (let [db (act/db world)
          root-id (:principal root)]
      (testing "root's own credential and the witness's are token Objects from birth"
        (let [mine (tokens/tokens-of db root-id)
              theirs (tokens/tokens-of db (:id witness))]
          (is (= 1 (count mine)))
          (is (= "root.token" (field db (first mine) :token/label)))
          (is (= 1 (count theirs)))
          (is (str/includes? (str (field db (first theirs) :token/label)) "test/witness"))
          (is (= root-id (field db (first theirs) :token/issued-by)))
          (is (not-any? #(str/includes? (pr-str (objects/fields db %)) (:token witness)) theirs) "the secret is on no Field")))
      (testing "token.issue as a session: the secret in the result, a second active token for the witness"
        (let [r (act! root "token.issue" {:principal (str (:id witness)) :label "phone"})
              res (res-of r)]
          (is (:ok r) (pr-str r))
          (is (string? (:secret res)))
          (is (uuid? (:token res)))
          (is (= 2 (count (tokens/tokens-of (act/db world) (:id witness)))))
          (testing "attach with it names the token Object on the attached event and touches last-used"
            (let [[c a] (attach-raw world (:secret res))]
              (is (:ok a) (pr-str a))
              (is (= (:id witness) (get-in a [:result :principal])))
              (let [ev (last (filter #(= :attached (:event/kind %))
                                     (get-in (ws/request!! root :history {:stream (:id witness) :limit 1000}) [:result :events])))]
                (is (= (:token res) (get-in ev [:event/body :token]))))
              (is (inst? (field (act/db world) (:token res) :token/last-used)))
              (ws/close! c)))
          (testing "token.revoke: the secret stops authenticating with the reason"
            (let [rr (act! root "token.revoke" {:token (str (:token res)) :reason "lost phone"})]
              (is (:ok rr) (pr-str rr))
              (let [[c a] (attach-raw world (:secret res))]
                (is (= :unauthenticated (get-in a [:err :code])))
                (is (str/includes? (get-in a [:err :message]) "revoked"))
                (ws/close! c))
              (is (= :conflict (get-in (act! root "token.revoke" {:token (str (:token res))}) [:err :code])) "twice is a conflict")))))
      (testing "a token with a ttl expires: attach is refused as expired"
        (let [res (res-of (act! root "token.issue" {:principal "test/witness" :label "short" :ttl-s 1}))]
          (is (inst? (:expires-at res)))
          (let [[c a] (attach-raw world (:secret res))] (is (:ok a) "still live") (ws/close! c))
          (Thread/sleep 1200)
          (let [[c a] (attach-raw world (:secret res))]
            (is (= :unauthenticated (get-in a [:err :code])))
            (is (str/includes? (get-in a [:err :message]) "expired"))
            (ws/close! c))))
      (testing "a loop never gets the secret: the file path instead, 0600 under data/tokens"
        (let [{:keys [ok err]} (actions/perform-action! world root-id "token.issue" {:principal "test/witness" :label "agent made"} {:session? false})]
          (is (nil? err) (pr-str err))
          (is (nil? (:secret (:result ok))))
          (let [path (:secret-written-to (:result ok))]
            (is (and path (.exists (io/file path))))
            (is (str/includes? path "/tokens/"))
            (let [[c a] (attach-raw world (str/trim (slurp path)))] (is (:ok a)) (ws/close! c)))))
      (testing "the witness, given write on itself and the action, issues its own; a bystander may not"
        (fx/grant! root (:id witness) #{(:id witness)} #{:read :write})
        (fx/grant! root (:id witness) #{(objects/lookup db (:world-id world) "action/token.issue")} #{:read :evaluate})
        (let [w (fx/attach-as world (:token witness))
              r (act! w "token.issue" {:principal (str (:id witness)) :label "self"})]
          (is (:ok r) (pr-str r))
          (ws/close! w))
        (let [other (get-in (ws/request!! root :act {:class :principal :action :create :args {:driven-by :person :name "test/other"}}) [:result])
              _ (fx/grant! root (:object other) #{(objects/lookup db (:world-id world) "action/token.issue")} #{:read :evaluate})
              c (fx/attach-as world (get-in other [:result :token]))
              r (act! c "token.issue" {:principal (str (:id witness)) :label "theft"})]
          (is (= :unresolved (get-in r [:err :code])) (pr-str r))
          (ws/close! c)))
      (testing "token.list: rows for the witness, statuses named"
        (let [r (act! root "token.list" {:principal "test/witness"})
              res (res-of r)]
          (is (:ok r) (pr-str r))
          (is (<= 4 (:count res)))
          (is (= #{"active" "revoked" "expired"} (set (map #(nth % 2) (:rows res)))))))
      (testing "the legacy hash alone still attaches and gains a token Object labelled legacy at upgrade"
        (let [h (field (act/db world) root-id :principal/token-hash)]
          (is (string? h))
          ;; a legacy-only Principal: a Principal whose hash has no token Object; simulate by a fresh hash
          (let [secret (session/new-token)
                p-id (:id witness)]
            (act/append! world (log/pending :field-set p-id root-id
                                                                 {:name :principal/token-hash :type :string
                                                                  :value (session/token-hash secret) :reader :agent}))
            (is (= [p-id] (tokens/legacy-without-object (act/db world))))
            (let [[c a] (attach-raw world secret)]
              (is (:ok a) "the legacy hash authenticates")
              (is (nil? (get-in (last (filter #(= :attached (:event/kind %))
                                               (get-in (ws/request!! root :history {:stream p-id :limit 1000}) [:result :events])))
                                [:event/body :token])))
              (ws/close! c))
            (let [done (kz/step! world :token-objects)]
              (is (contains? (:principals done) p-id))
              (is (= "legacy" (field (act/db world) (get (:principals done) p-id) :token/label)))
              (is (empty? (tokens/legacy-without-object (act/db world)))))))))))

(deftest pairing-trades-a-code-once
  (fx/with-world [{:keys [world root witness]}]
    (testing "hello answers before attach"
      (let [c (ws/connect (:port world) :json)
            r (ws/request!! c :hello {})]
        (is (:ok r) (pr-str r))
        (is (= (:world-id world) (get-in r [:result :world])))
        (is (string? (get-in r [:result :name])))
        (is (str/starts-with? (get-in r [:result :url]) "ws://"))
        (is (number? (get-in r [:result :head])))
        (is (number? (get-in r [:result :storage :world-bytes])))
        (is (pos? (get-in r [:result :storage :disk-total-bytes])))
        (is (not (neg? (get-in r [:result :storage :disk-free-bytes]))))
        (ws/close! c)))
    (testing "pair.begin is session only"
      (let [{:keys [err]} (actions/perform-action! world (:principal root) "pair.begin" {:principal "test/witness" :label "phone"} {:session? false})]
        (is (= :session-only (:code err)))))
    (let [r (act! root "pair.begin" {:principal "test/witness" :label "laptop" :ttl-s 60})
          res (res-of r)]
      (is (:ok r) (pr-str r))
      (is (re-matches #"[0-9]{6}" (:pairing-code res)))
      (is (uuid? (:pairing res)))
      (testing "a wrong code is refused and counted; the right one yields a credential of the witness"
        (let [c (ws/connect (:port world) :transit)
              bad (ws/request!! c :pair.claim {:pairing-code "000000" :label "x"})
              good (ws/request!! c :pair.claim {:pairing-code (:pairing-code res) :label "laptop"})]
          (is (= :unauthenticated (get-in bad [:err :code])) (pr-str bad))
          (is (:ok good) (pr-str good))
          (is (= (:id witness) (get-in good [:result :principal])))
          (let [secret (get-in good [:result :secret])
                a (ws/request!! c :attach {:token secret :client-kind :listener})]
            (is (:ok a) (pr-str a))
            (is (= (:id witness) (get-in a [:result :principal])))
            (is (= (get-in good [:result :token]) (get-in (last (filter #(= :attached (:event/kind %))
                                                                       (get-in (ws/request!! root :history {:stream (:id witness) :limit 1000}) [:result :events])))
                                                          [:event/body :token]))))
          (ws/close! c)
          (testing "the pairing is consumed: the same code fails now"
            (let [c2 (ws/connect (:port world) :transit)]
              (is (= :unauthenticated (get-in (ws/request!! c2 :pair.claim {:pairing-code (:pairing-code res)}) [:err :code])))
              (ws/close! c2)))
          (testing "token.list shows the paired device by its label"
            (is (some #(= "laptop" (first %)) (:rows (res-of (act! root "token.list" {:principal "test/witness"}))))))))
      (testing "five wrong codes within a minute close the socket"
        (let [c (ws/connect (:port world) :transit)
              answers (doall (for [_ (range 6)]
                               (try (get-in (ws/request!! c :pair.claim {:pairing-code "999999"} 3000) [:err :code])
                                    (catch Exception _ :closed))))]
          (is (= :rate-limited (nth answers 5)) (pr-str answers))
          (is (not= ::none (deref (:closed c) 3000 ::none)) "the server closed the socket")))
      (testing "an expired pairing is dead"
        (let [res (res-of (act! root "pair.begin" {:principal "test/witness" :label "slow" :ttl-s 1}))]
          (Thread/sleep 1200)
          (let [c (ws/connect (:port world) :transit)]
            (is (= :unauthenticated (get-in (ws/request!! c :pair.claim {:pairing-code (:pairing-code res)}) [:err :code])))
            (ws/close! c)))))))
