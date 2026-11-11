(ns knowledge.world.adapters.harness-test
  "The harness index (decisions 2026-10-30): built from the tracked files, grouped by top-level
   directory, docs headed, noisy trees counted, capped at index-cap by collapsing groups."
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.adapters.fs :as fs]
            [knowledge.world.adapters.harness :as harness]
            [knowledge.world.fixtures :as fx]))

(deftest the-repository-index
  (let [root (fs/harness-root {})
        text (harness/index-text root)]
    (is (some? root) "the tests run inside the repository")
    (is (str/starts-with? text "# harness index: "))
    (is (<= (count (.getBytes text "UTF-8")) harness/index-cap))
    (is (str/includes? text "world/src/knowledge/world/loop/core.clj ") "the world's own code is listed before anything else collapses")
    (is (str/includes? text "docs/protocol.md "))
    (is (re-find #"(?m)^docs/ethos\.md \d+  # Ethos" text) "docs carry their first heading")
    (is (re-find #"(?m)^## docs/figures: \d+ files" text) "noisy trees are counted, not listed")
    (is (re-find #"^[0-9a-f]{7,} (clean|dirty)$" (harness/head root)))))

(deftest the-index-is-capped-by-collapsing-groups
  (let [dir (fx/temp-dir)]
    (doseq [g ["alpha" "beta" "gamma"] i (range 150)]
      (let [f (io/file dir g (str "a-rather-long-file-name-to-fill-the-index-quickly-" i ".txt"))]
        (.mkdirs (.getParentFile f))
        (spit f (str i))))
    (testing "without git the walk is used, and the largest groups collapse until it fits"
      (let [text (harness/index-text dir)]
        (is (<= (count (.getBytes text "UTF-8")) harness/index-cap))
        (is (str/includes? text "450 tracked files"))
        (is (re-find #"(?m)^## (alpha|beta|gamma): 150 files, .* \(not listed\)$" text))
        (is (= "no git" (harness/head dir)))))
    (fx/delete-tree! dir)))
