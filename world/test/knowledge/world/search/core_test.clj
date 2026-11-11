(ns knowledge.world.search.core-test
  "Search: hits only within the caller's Grants and visible Fields; Message texts;
   bound names; since; the limit clamp; the resolved summary event; the action search.find."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.search.core :as search]
            [knowledge.world.ws-client :as ws]))

(defn- create-object! [c name fields]
  (let [r (ws/request!! c :act {:class :object :action :create :args {:name name :fields fields}})]
    (when-not (:ok r) (throw (ex-info "create failed" r)))
    (get-in r [:result :object])))

(defn- last-events [world n]
  (let [head (log/head (:log world))]
    (log/read-from (:log world) (max 1 (- head n)) (inc n))))

(deftest search-is-scoped-and-ordered
  (fx/with-world [{:keys [world root witness]}]
    (let [root-id (:principal root)
          wid (:world-id world)
          fox (create-object! root "den.fox" {:notes/habitat "the quick brown fox sleeps in a den under the oak"})
          owl (create-object! root "den.owl" {:notes/habitat "a brown owl nests in the same oak, hunting at night"})
          secret (create-object! root "vault.plan" {:notes/plan "brown paper plan nobody should read"})
          _ (ws/request!! root :bind {:namespace wid :name "den.fox" :object fox})
          _ (ws/request!! root :bind {:namespace wid :name "den.owl" :object owl})
          _ (ws/request!! root :bind {:namespace wid :name "vault.plan" :object secret})
          ;; the witness reads den.* only
          _ (fx/grant! root (:id witness) {:prefix "den"} [:read])
          c (fx/attach-as world (:token witness))]
      (testing "as root: every hit, newest first, with snippets"
        (let [r (ws/request!! root :search {:text "brown"})
              hits (get-in r [:result :hits])]
          (is (:ok r) (pr-str r))
          (is (= 3 (get-in r [:result :total])))
          (is (= [secret owl fox] (mapv :object hits)) "newest first")
          (is (every? #(re-find #"(?i)brown" (:snippet %)) hits))
          (is (= :field (:kind (first hits))))))
      (testing "as the witness: only den.* hits, the vault never"
        (let [r (ws/request!! c :search {:text "brown"})
              hits (get-in r [:result :hits])]
          (is (= #{fox owl} (set (map :object hits))) (pr-str hits))
          (is (= 2 (get-in r [:result :total])))))
      (testing "a name is a hit of kind object"
        (let [r (ws/request!! c :search {:text "den.owl" :kinds #{:object}})
              hits (get-in r [:result :hits])]
          (is (= [owl] (mapv :object hits)))
          (is (= :object (:kind (first hits))))
          (is (= "den.owl" (:name (first hits))))))
      (testing "a Message text is a hit of kind message, for its recipient"
        (let [route (first (get-in (ws/request!! root :read {:object root-id}) [:result :object/fields :principal/routes :field/value]))
              s (ws/request!! root :send {:to #{(:id witness)} :route route :text "the fox den needs a brown door"})
              _ (is (:ok s) (pr-str s))
              r (ws/request!! c :search {:text "brown door" :kinds #{:message}})
              hits (get-in r [:result :hits])]
          (is (= [(get-in s [:result :message])] (mapv :object hits)) (pr-str hits))
          (is (= :message (:kind (first hits))))
          (is (= :message/text (:field (first hits))))))
      (testing "since narrows to what was set at or after a seq"
        (let [head (log/head (:log world))
              later (create-object! root "den.badger" {:notes/habitat "a brown badger digs"})
              _ (ws/request!! root :bind {:namespace wid :name "den.badger" :object later})
              r (ws/request!! c :search {:text "brown" :since head})]
          (is (= [later] (mapv :object (get-in r [:result :hits]))) (pr-str r))))
      (testing "the limit is clamped, never refused; blank text is invalid"
        (let [r (ws/request!! root :search {:text "a" :limit 5000})]
          (is (:ok r) (pr-str r))
          (is (<= (count (get-in r [:result :hits])) search/max-limit)))
        (is (= :invalid (get-in (ws/request!! root :search {:text "  "}) [:err :code]))))
      (testing "a token hash is never a hit, even for root"
        (let [hash (get-in (objects/fields (act/db world) (:id witness)) [:principal/token-hash :field/value])
              r (ws/request!! root :search {:text (subs (str hash) 0 12)})]
          (is (zero? (get-in r [:result :total])) "principal/token-hash is never a search result")))
      (testing "one resolved summary event per search"
        (ws/request!! c :search {:text "oak"})
        (let [ev (last (filter #(= :resolved (:event/kind %)) (last-events world 5)))]
          (is (= "search:oak" (get-in ev [:event/body :name])))
          (is (= 2 (get-in ev [:event/body :count])))
          (is (= (:id witness) (:event/actor ev)))))
      (testing "the action search.find answers the same, once the witness may evaluate it"
        (fx/grant! root (:id witness) #{(objects/lookup (act/db world) wid "action/search.find")} [:read :evaluate])
        (let [r (ws/request!! c :act {:action :search.find :args {:text "oak" :limit 1}})]
          (is (:ok r) (pr-str r))
          (is (= 1 (count (get-in r [:result :result :hits]))))
          (is (= 2 (get-in r [:result :result :total])))))
      (ws/close! c))))

(deftest separators-and-case-are-one-and-a-loops-record-ranks-last
  (fx/with-world [{:keys [world root]}]
    (let [db-search #(search/search (act/db world) (:principal root) {:text %})
          person (create-object! root "someone" {:real_name "Morgan" :notes/a-real-name "Ada"})
          _ (ws/request!! root :set-field {:object person :field :notes/text :type :string :value "the Real_Name of it"})
          ;; a later Evaluation that repeats the words
          e (create-object! root "later" {:evaluation/thinking "the person asked for the real name"})]
      (testing "a Field's name finds it, whatever the separators"
        (let [{:keys [hits]} (db-search "real name")
              by (group-by :field hits)]
          (is (contains? by :real_name) (pr-str (map :field hits)))
          (is (contains? by :notes/a-real-name))
          (is (contains? by :notes/text) "and the text")))
      (testing "a search written with the separator finds the same"
        (is (contains? (set (map :field (:hits (db-search "real_name")))) :evaluation/thinking)))
      (testing "the loop's record ranks below the Fields it repeats, though it is newest"
        (let [hits (:hits (search/search (act/db world) (:principal root) {:text "real name" :limit 100}))
              at (fn [o] (first (keep-indexed #(when (= o (:object %2)) %1) hits)))]
          (is (< (at person) (at e)) (pr-str (map (juxt :field :seq) hits)))
          (is (= person (:object (first hits))))))
      (testing "the word message does not find every Message by its Field's name"
        (let [route (first (get-in (ws/request!! root :read {:object (:principal root)}) [:result :object/fields :principal/routes :field/value]))]
          (ws/request!! root :send {:to #{(:principal root)} :route route :text "nothing of note"})
          (is (not-any? #(= :message/text (:field %)) (:hits (db-search "message")))))))))
