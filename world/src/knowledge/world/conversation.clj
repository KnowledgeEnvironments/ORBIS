(ns knowledge.world.conversation
  "Conversations as things: a conversation is a message/thread (decisions 2026-10-31). Directives a
   person types (/new, /compress, /resume) are handled here by the kernel and never reach a model.
   Compression is a summarising Evaluation over a conversation's older Messages, by the Principal's
   Model with reasoning off, written to conversation/summary on the thread root with via the
   Evaluation. Nothing is deleted: the Messages stay on the log, and a later summary covers the one
   before it."
  (:require [knowledge.world.models :as models]
            [clojure.java.io :as io]
            [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.evaluate.core :as evaluate]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.refs :as refs]
            [knowledge.world.route.core :as route]
            [knowledge.world.serialize.core :as ser]
            [knowledge.world.thread :as thread]
            [taoensso.timbre :as timbre]))

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- kind [db m] (some-> (field db m :message/kind) name keyword))
(defn- seq-of [db m] (or (:object/created-seq (objects/entity db m)) 0))
(defn- spoken? [db m] (not (contains? #{:directive :status :trigger :approval :approval-result} (kind db m))))

(def shipped-compressor (delay (slurp (io/resource "knowledge/world/boot/instructions/compressor.md"))))

(def default-summary-tokens "Measured summaries ran to 1,300 tokens, which saved little." 400)

(defn summary-tokens
  "The size a summary should stay under: serialize/summary-tokens on the Principal, else the default."
  [db p]
  (let [v (field db p :serialize/summary-tokens)]
    (if (and (number? v) (pos? v)) (long v) default-summary-tokens)))

(defn compress-at
  "Tokens of unsummarised conversation at which a wake compresses first: serialize/compress-at on the
   Principal, else 70 percent of what its budget, its Model's whole window where that is known, leaves
   after the prompt's standing part. Measured
   on a real conversation: at 70 percent of the whole budget Messages were being dropped for six
   wakes before the first summary. 0 is off."
  ([db p] (compress-at db p nil))
  ([db p thread]
  (let [v (field db p :serialize/compress-at)]
    (if (number? v)
      (long v)
      (let [budget (:serialize/budget-tokens (ser/policy db p thread))
            standing (long (:standing-tokens (ser/render db {:principal p :working-set #{p}}) 0))]
        (long (* 0.7 (max 0 (- budget standing)))))))))

(defn- uncovered
  "The thread's spoken Messages after its summary, oldest first."
  [db thread-id]
  (let [through (long (or (field db thread-id :conversation/through) 0))]
    (filter #(and (spoken? db %) (> (seq-of db %) through)) (thread/messages db thread-id))))

(defn tokens-est
  "What the unsummarised conversation weighs, at the serializer's chars per token."
  [db thread-id]
  (quot (reduce + 0 (map #(count (str (field db % :message/text))) (uncovered db thread-id))) ser/chars-per-token))

(defn- speaker-name [db id] (or (first (sort (map second (objects/names db id)))) (subs (str id) 0 8)))

(defn- transcript [db p messages]
  (str/join "\n\n" (for [m messages]
                     (str (speaker-name db (field db m :message/from)) ": "
                          (refs/text-of db p (objects/fields db m) :message/text)))))

(defn- model-of [db p]
  (some (fn [e] (when (= :model (field db e :model/kind)) e)) (sort-by str (field db p :principal/models))))

(defn compress!
  "Summarise the conversation's older Messages, leaving the newest serialize/messages-verbatim. An
   earlier summary goes in and is covered by the new one. {:ok {:evaluation :through :messages}},
   {:nothing reason} or {:err}. Synchronous; p's lane or a caller's thread."
  [world p thread-id {:keys [note]}]
  (let [db (act/db world)
        ;; what stays out of a summary, whatever a wide window lets a prompt carry
        keep-n (long (:serialize/messages-verbatim ser/policy-defaults))
        all (vec (uncovered db thread-id))
        older (vec (drop-last keep-n all))
        model (or (:model (models/for-wake db (objects/state db p) thread-id)) (model-of db p))
        prior (refs/text-of db p (objects/fields db thread-id) :conversation/summary)]
    (cond
      (empty? older) {:nothing "nothing old enough to compress"}
      (nil? model) {:nothing "no Model"}
      :else
      (let [prompt (or (not-empty (str (field db p :serialize/compressor))) @shipped-compressor)
            body (str (when prior (str "Summary so far:\n" prior "\n\nWhat was said since:\n\n"))
                      (transcript db p older))
            size (str "Keep the summary under " (summary-tokens db p) " tokens.")
            rendered {:template prompt :acting (if note (str size " Asked for: " note) size) :body body
                      :message-text "Write the summary now." :objects [] :reads [] :depth {} :dropped 0}
            {:keys [ok err]} (evaluate/evaluate! world {:principal p :working-set (set older) :model model
                                                        :reasoning :off :rendered rendered :sync? true})]
        (if err
          {:err err}
          (let [outcome (deref (:done ok) 5000 nil)
                {text :text spans :refs} (refs/parse (act/db world) p (:ids ok) (some-> (:text outcome) str str/trim))
                through (seq-of db (last older))
                from (or (field db thread-id :conversation/from) (seq-of db (first older)))
                put! (fn [k t v] (act/append! world (log/pending :field-set thread-id p {:name k :type t :value v :reader :both}
                                                                 :via (:evaluation ok))))]
            (if (and (= :done (:state outcome)) (seq text))
              (do (put! :conversation/summary :string text)
                  (when (or (seq spans) prior) (put! (refs/refs-key :conversation/summary) [:list :map] spans))
                  (put! :conversation/from :number from)
                  (put! :conversation/through :number through)
                  {:ok {:evaluation (:evaluation ok) :through through :messages (count older)}})
              {:err {:code :invalid :message (str "the summary failed: " (:error outcome (:state outcome)))}})))))))

(defn maybe-compress!
  "A wake's first step: true when this conversation passed the threshold and is now compressed."
  [world p message]
  (try
    (let [db (act/db world)
          t (thread/thread-of db message)
          at (compress-at db p t)]
      (and (pos? at) (> (tokens-est db t) at) (some? (:ok (compress! world p t nil)))))
    (catch Exception e (timbre/warn e "compression failed; the wake goes on") false)))

;; ---------------------------------------------------------------- directives

(def directive-help
  "/new starts a fresh conversation. /compress [note] summarises this one now. /resume <thread> reopens one. /think and /fast go at the start of a line.")

(defn- status! [world from to route in-reply-to text]
  (let [{:keys [err]} (route/send! world from {:to #{to} :route route :text text :kind :status :in-reply-to in-reply-to})]
    (when err (timbre/warn "a directive's status could not be sent" {:err err}))))

(defn handle-directive!
  "Answer a directive Message: {:message :route :to} with route/send!'s directive map {:name :args :open}."
  [world p {:keys [message route to name args open]}]
  (let [from (or (first (sort-by str to)) p)
        say (fn [t] (status! world from p route message t))]
    (case name
      "new" (say "new conversation; the next line starts fresh")
      "resume" (if (field (act/db world) message :message/resumes)
                 (say (str "resumed " (or (route/title (act/db world) (field (act/db world) message :message/resumes)) "that conversation")))
                 (say "no conversation of yours on this Route matches that"))
      "compress" (if-not open
                   (say "nothing open to compress")
                   (future
                     (let [{:keys [ok err nothing]} (compress! world from open {:note args})]
                       (say (cond ok (str "compressed " (:messages ok) " messages; they stay on the log")
                                  nothing nothing
                                  :else (str "compress failed: " (:message err)))))))
      (say (str "/" name " is not a directive. " directive-help)))))
