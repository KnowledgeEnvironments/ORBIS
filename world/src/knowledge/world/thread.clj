(ns knowledge.world.thread
  "Conversations. A Message's thread is the root Message of its chain
   (message/thread, set by route/send!); the first Message of a chain is its own thread, and
   message/asked-by names who started it. These queries are what the loop and the serializer
   use to show a Principal the whole exchange it is part of: what was asked, what it asked
   others, which of those came back. Pure over the fold."
  (:require [datascript.core :as d]
            [knowledge.world.authority :as authority]
            [knowledge.world.fold.objects :as objects]))

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn thread-of
  "The thread a Message belongs to: its message/thread Field, else itself."
  [db message]
  (or (field db message :message/thread) message))

(defn asked-by
  "Who started the thread: message/asked-by on the Message, else the thread root's sender."
  [db message]
  (or (field db message :message/asked-by)
      (field db (thread-of db message) :message/from)))

(defn- by-field
  "Message ids whose Field k holds v, with their created seq."
  [db k v]
  (d/q '[:find ?id ?s :in $ ?k ?v :where
         [?f :field/name ?k] [?f :field/value ?v]
         [?o :object/fields ?f] [?o :object/class :message] [?o :object/id ?id] [?o :object/created-seq ?s]]
       db k v))

(defn messages
  "Every Message of a thread, oldest first (by created seq), the root included."
  [db thread]
  (->> (by-field db :message/thread thread) (sort-by second) (mapv first)))

(defn answer-to
  "The first Message in reply to `message`, or nil: how a loop knows a send of its own came back."
  [db message]
  (some->> (by-field db :message/in-reply-to message) (sort-by second) first first))

(defn question?
  "Does a Message await an answer: it is addressed to someone other than the sender of the Message
   it replies to (or replies to none). A reply to the asker awaits nothing; a question sent to a
   third party in-reply-to the asker's Message, as loops write them, does. What a person says always
   does: their next line in a conversation replies to the last thing said and still wants an answer."
  [db message]
  (let [parent (field db message :message/in-reply-to)
        to (set (field db message :message/to))]
    (or (nil? parent)
        (authority/person? db (field db message :message/from))
        (not (contains? to (field db parent :message/from))))))

(defn outstanding
  "The questions p asked in a thread that nothing has answered yet, oldest first."
  [db thread p]
  (->> (messages db thread)
       (filter #(and (= p (field db % :message/from)) (question? db %)))
       (remove #(answer-to db %))
       vec))

(defn status?
  "A reply the kernel marked interim (message/kind status): sent while an op of its wake waited for
   approval, so it reports a state, not the answer."
  [db message]
  (= :status (some-> (field db message :message/kind) name keyword)))

(defn current-exchange
  "The Messages of a thread since a person last spoke, that line included; all of them when no person
   did. A conversation runs for hours, so `what this thread already did` means what this exchange did:
   a person asking again has asked again."
  [db thread]
  (let [all (messages db thread)
        ;; a line someone typed: a decision on a card comes from a person too, but continues the exchange
        person? (fn [m] (and (nil? (field db m :message/kind))
                             (authority/person? db (field db m :message/from))))
        latest (last (filter person? all))]
    (if latest (vec (drop-while #(not= latest %) all)) all)))

(declare answered-by?)

(defn open-question
  "The newest Message of a thread put to p that p has not answered, or nil. A conversation holds many
   questions, so its root is not the one a result answers."
  [db thread p]
  (->> (rseq (messages db thread))
       (filter #(and (not= p (field db % :message/from))
                     (contains? (set (field db % :message/to)) p)
                     (question? db %)
                     (not (contains? #{:status :approval :approval-result :trigger}
                                     (some-> (field db % :message/kind) name keyword)))))
       first
       ((fn [m] (when (and m (not (answered-by? db m p))) m)))))

(def ^:private not-an-answer #{:status :approval :approval-result :trigger})

(defn replied-since?
  "Has p said anything that counts, anywhere in this thread, after `message`? A reply to the approval
   that came of a request answers the request too, though it is in-reply-to the decision."
  [db thread message p]
  (let [seq-of #(:object/created-seq (objects/entity db %))
        after (long (or (seq-of message) 0))]
    (boolean (some #(and (= p (field db % :message/from))
                         (> (long (or (seq-of %) 0)) after)
                         (not (contains? not-an-answer (some-> (field db % :message/kind) name keyword))))
                   (messages db thread)))))

(defn answered-by?
  "Has p replied to `message` with an answer: a Message of p's in-reply-to it that is neither a
   status note nor kernel traffic (an approval card, a decision, a Trigger firing)? Those sit in the
   thread without answering anything."
  [db message p]
  (boolean (some #(and (= p (field db % :message/from))
                       (not (contains? not-an-answer (some-> (field db % :message/kind) name keyword))))
                 (map first (by-field db :message/in-reply-to message)))))
