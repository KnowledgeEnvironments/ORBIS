(ns knowledge.world.protocol.resume-test
  "The resume exit test (spec 18.1, protocol.md Session): a client attaches, subscribes, is pushed
   events, acks some, drops without detach, more events happen, it reattaches with its session
   and acked seqs, and receives exactly acked+1 .. head: no gap, no duplicate. Both wire forms.
   Variant 2 restarts the world between drop and reattach: the session is gone (resumed false),
   the client re-subscribes with after = acked, same assertion."
  (:require [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.system :as system]
            [knowledge.world.ws-client :as ws]))

(defn- set-fields!
  "n writes on root by client; returns the seqs."
  [client root-id n tag]
  (vec (for [i (range n)]
         (let [r (ws/request!! client :set-field {:object root-id :field :notes/exit :type :string :value (str tag "-" i) :reader :both})]
           (is (:ok r) (pr-str r))
           (get-in r [:result :seq])))))

(defn- contiguous [from to] (vec (range (inc from) (inc to))))

(defn- phase-one
  "Attach, subscribe to root's stream, cause events, receive, ack the first write, drop.
   Returns {:client :session :root-id :acked}."
  [world format]
  (let [c (fx/attach-root world format)
        root-id (:principal c)
        sub (ws/request!! c :subscribe {:sub "h" :stream root-id})
        _ (is (:ok sub) (pr-str sub))
        seqs (set-fields! c root-id 3 "before")
        pushed (ws/collect-pushed-seqs c "h" (last seqs) 5000)]
    (is (= (contiguous (get-in sub [:result :as-of]) (last pushed)) pushed) "first pushes are contiguous from as-of")
    (is (>= (last pushed) (last seqs)))
    (ws/ack! c "h" (first seqs))
    (Thread/sleep 50)
    (ws/abort! c)
    {:client c :session (:session c) :root-id root-id :acked (first seqs)}))

(defn- cause-more! [world root-id]
  (let [other (fx/attach-root world)]
    (set-fields! other root-id 3 "while-dropped")
    (ws/close! other)))

(defn- resume-same-world [world format]
  (let [{:keys [session root-id acked]} (phase-one world format)]
    (Thread/sleep 200)
    (cause-more! world root-id)
    (let [c (ws/connect (:port world) format)
          r (ws/request!! c :attach {:token (fx/root-token world) :session session :subs {"h" acked}})
          head (:seq r)]
      (is (:ok r) (pr-str r))
      (is (true? (get-in r [:result :resumed])) "the session was kept within its TTL")
      (is (= session (get-in r [:result :session])))
      (let [got (ws/collect-pushed-seqs c "h" head 5000)]
        (is (= (contiguous acked head) got) "exactly acked+1 .. head, no gap, no duplicate")
        (is (= (count got) (count (set got)))))
      (ws/close! c))))

(deftest resume-after-drop-transit
  (fx/with-world [{:keys [world]}] (resume-same-world world :transit)))

(deftest resume-after-drop-json
  (fx/with-world [{:keys [world]}] (resume-same-world world :json)))

(defn- resume-across-restart [format]
  (let [dir (fx/temp-dir)
        w1 (fx/start-world dir 0)
        {:keys [session root-id acked]} (phase-one w1 format)]
    (Thread/sleep 200)
    (system/stop w1)
    (let [w2 (fx/start-world dir 0)]
      (try
        (cause-more! w2 root-id)
        (let [c (ws/connect (:port w2) format)
              r (ws/request!! c :attach {:token (fx/root-token w2) :session session :subs {"h" acked}})]
          (is (:ok r) (pr-str r))
          (is (false? (get-in r [:result :resumed])) "sessions are transport state and do not survive a restart")
          (is (not= session (get-in r [:result :session])))
          (let [sub (ws/request!! c :subscribe {:sub "h" :stream root-id :after acked})
                head (:seq sub)
                got (ws/collect-pushed-seqs c "h" head 5000)]
            (is (:ok sub) (pr-str sub))
            (is (= (contiguous acked head) got) "exactly acked+1 .. head across a restart")
            (is (= (count got) (count (set got)))))
          (ws/close! c))
        (finally (system/stop w2) (fx/delete-tree! dir))))))

(deftest resume-across-restart-transit (resume-across-restart :transit))
(deftest resume-across-restart-json (resume-across-restart :json))

(deftest window-and-lagging
  (testing "a full window pauses the sub and the next push after ack says lagging"
    (fx/with-world [{:keys [world]}]
      (let [c (fx/attach-root world :transit)
            root-id (:principal c)
            sub (ws/request!! c :subscribe {:sub "w" :stream root-id :window 2})
            _ (is (:ok sub))
            seqs (set-fields! c root-id 4 "lag")
            first-push (ws/await-push c "w" 3000)]
        (is (<= (count (:events first-push)) 2))
        (is (nil? (ws/await-push c "w" 300)) "nothing more until an ack")
        (ws/ack! c "w" (last (map :event/seq (:events first-push))))
        (let [next (ws/await-push c "w" 3000)]
          (is (true? (:lagging next)) (pr-str (dissoc next :events)))
          (is (= (inc (last (map :event/seq (:events first-push)))) (:event/seq (first (:events next))))))
        (ws/ack! c "w" (last seqs))
        (ws/close! c)))))
