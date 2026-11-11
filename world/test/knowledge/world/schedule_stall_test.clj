(ns knowledge.world.schedule-stall-test
  "A stalled child wakes its creator only for news (decisions 2026-10-31): a cap or a cancel the asker
   was told of, or the same reason again, spends none of the creator's turns."
  (:require [clojure.test :refer [deftest is]]
            [knowledge.world.schedule.core :as sched]))

(def old-news? @#'sched/old-news?)
(def t {:unless-told true})

(deftest what-the-asker-heard-is-not-news
  (is (old-news? t {:value "iteration cap 8 reached without a final reply (loop/max-iterations)"}))
  (is (old-news? t {:value "cancelled by person after 3 s"}))
  (is (old-news? t {:value "evaluation failed after 3 s: boom" :previous "evaluation failed after 3 s: boom"}))
  (is (not (old-news? t {:value "evaluation failed after 3 s: boom"})))
  (is (not (old-news? {} {:value "iteration cap 8 reached"})) "only a Trigger that asks for it"))
