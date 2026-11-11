(ns knowledge.world.retention-notice-test
  "Over the disk limit with nothing left to take is said to a person, once (punch list 2026-11-01)."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.retention :as retention]))

(deftest the-person-hears-once-when-nothing-more-can-go
  (let [over {:over 2000000000 :limit-gb 16 :blocked "only what a live thing needs is left"}
        under {:limit-gb 16}]
    (testing "over, with nothing left: said, with the size and the way out"
      (let [t (retention/notice-for nil over)]
        (is (str/includes? t "2.0 GB over its disk limit of 16 GB"))
        (is (str/includes? t "only what a live thing needs is left"))
        (is (str/includes? t "retention.set"))))
    (testing "the same standing on the next sweep: silence"
      (is (nil? (retention/notice-for over over))))
    (testing "what blocks it changed: said again"
      (is (some? (retention/notice-for over (assoc over :blocked "pinned under anchor/")))))
    (testing "back under: once"
      (is (str/includes? (retention/notice-for over under) "back under"))
      (is (nil? (retention/notice-for under under))))
    (testing "never over: nothing"
      (is (nil? (retention/notice-for nil under))))))
