(ns knowledge.world.serialize.directives-test
  (:require [clojure.test :refer [deftest is]]
            [knowledge.world.serialize.core :as ser]))

(deftest directives-are-for-the-loop-not-the-model
  (is (= "what do you know?" (ser/strip-directives "/think what do you know?")))
  (is (= "what do you know?" (ser/strip-directives "  /fast   what do you know?")))
  (is (= "first\nsecond" (ser/strip-directives "/think\nfirst\nsecond")) "a directive on its own line leaves the rest")
  (is (= "a /think in the middle stays" (ser/strip-directives "a /think in the middle stays")) "only at the start of a line")
  (is (= "/thinker is a word" (ser/strip-directives "/thinker is a word")))
  (is (= "" (ser/strip-directives nil))))
