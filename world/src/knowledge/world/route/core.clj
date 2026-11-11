(ns knowledge.world.route.core
  "Messages on Routes (spec 5.2, 5.7, 9.1, 9.5). `send!` creates a Message Object, appends `sent`
   to the sender's stream and the Route's, and `delivered` to each recipient's stream and the
   Route's, in Route order. Resolution: the sender resolves the Route for op :transmit (spec 5.9).
   The wire op `send` and the resident loop both come here (ground rule 4)."
  (:require [clojure.string :as str]
            [knowledge.world.authority :as authority]
            [datascript.core :as d]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.image :as image]
            [knowledge.world.log.core :as log]
            [knowledge.world.refs :as refs]
            [knowledge.world.resolve.core :as resolve]))

(defn route-state
  "The Route's state or nil when the object is not a Route."
  [db route]
  (let [s (objects/state db route)]
    (when (= :route (:object/class s)) s)))

(def default-conversation-idle-s
  "How long a conversation on a Route stays open with nobody speaking (serialize/conversation-idle-s
   on a recipient overrides it; decisions 2026-10-31 'a conversation is a conversation')."
  1800)

(defn- route-messages
  "Message ids on a Route, newest first."
  [db route]
  (->> (d/q '[:find ?id ?s :in $ ?route :where
              [?f :field/name :message/route] [?f :field/value ?route]
              [?o :object/fields ?f] [?o :object/class :message] [?o :object/id ?id] [?o :object/created-seq ?s]]
            db route)
       (sort-by second >)
       (map first)))

(defn- idle-limit-s [db to]
  (or (some (fn [r] (let [v (get-in (objects/fields db r) [:serialize/conversation-idle-s :field/value])]
                      (when (number? v) v)))
            to)
      default-conversation-idle-s))

(defn- directive-of
  "A whole-line /word from a person, unless it is /think or /fast, which ride on a line to the model
   (serialize/strip-directives). A path is not a directive: /Users/x has a second slash. {:name :args}."
  [text]
  (when-let [[_ w a] (re-matches #"\s*/([A-Za-z][\w-]*)(?:[ \t]+([^\n]*?))?\s*" (str text))]
    (when-not (#{"think" "fast"} w) {:name w :args (when (seq a) a)})))

(defn title
  "conversation/title on the thread root, else the first line of the first person's Message."
  [db thread]
  (let [f (fn [m k] (get-in (objects/fields db m) [k :field/value]))]
    (or (f thread :conversation/title)
        (some-> (f thread :message/text) str/split-lines first (as-> t (subs t 0 (min 80 (count t))))))))

(defn- find-thread
  "A thread p asked on this Route, by id or the first characters of it."
  [db p route word]
  (let [f (fn [m k] (get-in (objects/fields db m) [k :field/value]))]
    (when (seq word)
      (some (fn [m] (let [t (or (f m :message/thread) m)]
                      (when (and (= p (f m :message/asked-by)) (str/starts-with? (str t) word)) t)))
            (route-messages db route)))))

(defn open-conversation
  "The conversation a person's next line on a Route continues: read back from the newest Message, /new
   closes, /resume names one, else the newest thread p asked if its last Message is younger than the
   idle limit. {:thread id :in-reply-to id-or-nil}, or nil. in-reply-to is the last thing someone else
   said in it. The person never had to thread anything: every line used to start a thread of its own,
   and the model lost the conversation between them."
  [db p route to]
  (let [f (fn [m k] (get-in (objects/fields db m) [k :field/value]))
        talk (remove #(= :trigger (some-> (f % :message/kind) name keyword)) (route-messages db route))
        found (some (fn [m]
                      (let [d (f m :message/directive)]
                        (cond
                          (and (= d "new") (= p (f m :message/from))) ::closed
                          (and (= d "resume") (= p (f m :message/from)) (f m :message/resumes)) {:thread (f m :message/resumes) :resumed true}
                          ;; a directive and the status answering it are no conversation
                          (or d (f (or (f m :message/thread) m) :message/directive)) nil
                          (= p (f m :message/asked-by)) {:thread (or (f m :message/thread) m)})))
                    talk)]
    (when (map? found)
      (let [thread (:thread found)
            in-thread (filter #(= thread (f % :message/thread)) talk)
            at (some-> (first in-thread) (as-> m (get-in (objects/fields db m) [:message/text :field/set-at])))
            age-s (when at (/ (- (System/currentTimeMillis) (.getTime ^java.util.Date at)) 1000.0))]
        (when (or (:resumed found) (and age-s (< age-s (idle-limit-s db to))))
          {:thread thread
           :in-reply-to (first (remove #(= p (f % :message/from)) in-thread))})))))

(defn directive-line?
  "Would this text from p be a directive (a person's whole-line /word, the kernel's)?"
  [db p text]
  (boolean (and (authority/person? db p) (directive-of text))))

(defn send!
  "{:to #{principal ids} :route id :text :tags [ids] :in-reply-to id}; via is the Evaluation
   that produced the Message, when one did, and hop how many loop-to-loop legs it has travelled
   (message/hop, decisions 2026-10-30 'loops talk to loops'; absent for a session's Message).
   thread is the conversation the sender's loop is working for: a Message that
   answers none inherits it, so the answers that come back at hop 2 share the asker's thread.
   A person's plain line joins the open conversation on the Route (open-conversation).
   Returns {:ok {:message id :seq}} or {:err ...}."
  [world p {:keys [to route text refs unresolved-refs tags in-reply-to via hop reasoning kind task trigger thread view gateway expires-at device session images]}]
  (let [db (act/db world)
        images (image/attachments world images)
        to (set to)
        ;; a person types lines, not threads: a plain line continues the conversation on this Route
        plain-line? (and (nil? in-reply-to) (nil? thread) (nil? via) (nil? kind)
                         (authority/person? db p)
                         (route-state db route))
        directive (when plain-line? (directive-of text))
        open (when plain-line? (open-conversation db p route to))
        joined (when-not directive open)
        kind (if directive :directive kind)
        resumes (when (= "resume" (:name directive)) (find-thread db p route (:args directive)))
        in-reply-to (or in-reply-to (:in-reply-to joined))
        thread (or thread (:thread joined))
        ;; the thread is the parent's thread (or the parent itself) when the Message
        ;; answers one, else the thread the sender's loop is working for, else this Message itself
        parent (when in-reply-to (objects/fields db in-reply-to))
        thread-id (or (when parent (or (get-in parent [:message/thread :field/value]) in-reply-to))
                      (when (uuid? thread) thread))
        asked-by (if thread-id
                   (or (get-in (objects/fields db thread-id) [:message/asked-by :field/value])
                       (get-in (objects/fields db thread-id) [:message/from :field/value])
                       p)
                   p)]
    (cond
      (nil? (route-state db route)) act/unresolved
      (not (every? #(objects/exists? db %) to)) act/unresolved
      (objects/retired? db route) (act/err :conflict "that Route is retired")
      (some #(objects/retired? db %) to) (act/err :conflict "a recipient is retired")
      (refs/valid-spans db p text refs) (act/err :invalid (refs/valid-spans db p text refs))
      (:err images) images
      :else
      (if-let [{:keys [grant]} (resolve/resolve! world db p (act/id-name route) :transmit)]
        (let [message (random-uuid)
              fields (cond-> {:message/from p :message/to to :message/route route :message/text text
                              :message/tags (vec tags)
                              :message/thread (or thread-id message) :message/asked-by asked-by}
                       in-reply-to (assoc :message/in-reply-to in-reply-to)
                       (seq refs) (assoc :message/refs (vec refs))
                       (seq images) (assoc :message/images (vec images))
                       (seq unresolved-refs) (assoc :message/unresolved-refs (vec unresolved-refs))
                       ;; reasoning is asked for (decisions 2026-10-30): the sender's explicit request
                       (#{:on :off} (some-> reasoning name keyword)) (assoc :message/reasoning (keyword (name reasoning)))
                       (and via (number? hop)) (assoc :message/hop (long hop))
                       ;; approval gates (decisions 2026-10-30): a card or a decision about a Task
                       (#{:approval :approval-result :trigger :view :status :request :directive :steer :gateway} (some-> kind name keyword)) (assoc :message/kind (keyword (name kind)))
                       directive (assoc :message/directive (:name directive))
                       ;; where a text from outside came from: who it claims, never who it is
                       (map? gateway) (assoc :message/gateway gateway)
                       resumes (assoc :message/resumes resumes)
                       (uuid? task) (assoc :message/task task)
                       ;; the card says when its Task expires
                       (instance? java.util.Date expires-at) (assoc :message/expires-at expires-at)
                       ;; a Trigger's firing, the Message of its owner to itself (or trigger/to)
                       (uuid? trigger) (assoc :message/trigger trigger)
                       ;; views: a Message of kind :view names the view Serialization it carries
                       (uuid? view) (assoc :message/view view)
                       ;; which client sent it (decisions 2026-10-31): from is the Principal, device the
                       ;; token's label, session the attach; a loop passes neither
                       (string? device) (assoc :message/device device)
                       (string? session) (assoc :message/session session))
              ap (fn [e] (act/append! world e))]
          (ap (log/pending :created message p {:class :message :fields fields} :delegation grant :via via))
          (let [sent (ap (log/pending :sent p p {:message message :route route} :streams #{route} :delegation grant :via via))
                seq (reduce (fn [_ r]
                              (ap (log/pending :delivered r p {:message message :route route :recipient r}
                                               :streams #{route} :delegation grant :via via)))
                            sent to)]
            {:ok (cond-> {:message message :seq seq}
                   (not-empty unresolved-refs) (assoc :unresolved-refs (vec unresolved-refs))
                   directive (assoc :directive (assoc directive :open (:thread open) :route route :to to :message message)))}))
        act/unresolved))))
