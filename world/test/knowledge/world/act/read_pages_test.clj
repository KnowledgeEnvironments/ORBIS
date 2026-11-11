(ns knowledge.world.act.read-pages-test
  "A read pages by bytes as well as by lines: a Serialization is one long line."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is]]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- field-of [r k] (get-in r [:result :object/fields k :field/value]))

(deftest a-one-line-field-pages-by-bytes
  (fx/with-world [{:keys [root]}]
    (let [p (:principal root)
          text (str (str/join (repeat 20 "abcde")) "\u00e9" (str/join (repeat 20 "fghij")))
          _ (ws/request!! root :set-field {:object p :field :notes/long :type :string :value text :reader :both})
          page (ws/request!! root :read {:object p :field :notes/long :byte-range [0 50]})
          next-page (ws/request!! root :read {:object p :field :notes/long :byte-range [50 100]})
          split (ws/request!! root :read {:object p :field :notes/long :byte-range [101 110]})]
      (is (= (subs text 0 50) (field-of page :notes/long)))
      (is (= (count (.getBytes text "UTF-8")) (field-of page :notes/long-size)))
      (is (= [0 50] (field-of page :notes/long-byte-range)))
      (is (= (subs text 50 100) (field-of next-page :notes/long)))
      (is (= "fghijfgh" (field-of split :notes/long)) "a page starting inside a character moves past it")
      (is (not (str/includes? (field-of split :notes/long) "\ufffd"))))))

(deftest a-page-is-capped
  (fx/with-world [{:keys [root]}]
    (let [p (:principal root)
          big (str/join (repeat 100000 "x"))
          _ (ws/request!! root :set-field {:object p :field :notes/big :type :string :value big :reader :both})
          r (ws/request!! root :read {:object p :field :notes/big :byte-range [0 100000]})]
      (is (= act/byte-page-cap (count (field-of r :notes/big))))
      (is (true? (field-of r :notes/big-range-truncated))))))
