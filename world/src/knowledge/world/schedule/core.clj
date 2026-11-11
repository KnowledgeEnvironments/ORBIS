(ns knowledge.world.schedule.core
  "The scheduler (spec 5.5 Trigger, plan §19.1 W8, decisions 2026-10-30 'triggers'): one component
   that drives every armed Trigger from the log, on one thread, with no state outside the Trigger's
   Fields, so a restarted world picks up exactly where it was.

   A Trigger is an Object of class :trigger owned by the Principal that created it. Its kind says
   when it fires: :interval every trigger/every-s seconds, :at once at trigger/at, :on-change on a
   field-set of trigger/watch {object field?}, :on-message on a delivery to trigger/watch {object}.
   Firing is one thing in every case: a Message from the owner (to trigger/to, itself by default, on
   trigger/route, its first Route by default) carrying trigger/text, message/kind :trigger and
   message/trigger, via the Trigger, plus a `fired` event on the Trigger's and the owner's streams.
   The owner's loop wakes on it like on any Message (knowledge.world.loop.core admits a Trigger's
   Message although via is set; a quiet reply to a heartbeat is dropped, not sent).

   Bookkeeping Fields the kernel keeps on the Trigger (the world is the actor, as for the shipped
   Fields): trigger/armed-at, trigger/last-tick (the last slot used, fired or skipped), trigger/last-fired,
   trigger/fired-count, trigger/skipped (an interval slot that fell while the owner's loop was busy,
   loop/iteration > 0, is skipped and counted, never queued), trigger/last-error (a send that did not
   resolve). An :at Trigger and one past trigger/max-fires disarm themselves.

   Ops: arm {trigger} and disarm {trigger} (:write on the Trigger); arm with the spec's {predicate
   evaluation} creates the Trigger first (schedule → :interval, state → :on-change).

   One thread: a virtual thread owns a queue; the log subscription (on the writer thread, which never
   appends) enqueues the events a Trigger may watch; the thread drains them, fires what they match,
   then ticks the clock-driven Triggers, every tick-ms at most. Internal jobs ride the same clock:
   the approval and unsafe-mode expiry sweeps (knowledge.world.act.gates) every expire-every-ms, the Model
   probe (knowledge.world.models/probe-all!) every models/probe-every-ms, and the retention sweep, which
   this clock only starts: it runs on a thread of its own (knowledge.world.retention)."
  (:require [clojure.string :as str]
            [knowledge.world.act.core :as act]
            [knowledge.world.act.gates :as gates]
            [knowledge.world.delegate :as delegate]
            [knowledge.world.gateway.core :as gateway]
            [knowledge.world.models :as models]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.notify :as notify]
            [knowledge.world.retention :as retention]
            [knowledge.world.resolve.core :as resolve]
            [knowledge.world.route.core :as route]
            [taoensso.timbre :as timbre])
  (:import [java.util Date]
           [java.util.concurrent LinkedBlockingQueue TimeUnit]))

(def tick-ms 250)
(def expire-every-ms 60000)
(def delegate-every-ms 1000)

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))
(defn- kw [v] (cond (keyword? v) v (string? v) (keyword v) :else nil)) ; keeps a namespace: loop/stalled stays loop/stalled
(defn- ms ^long [^Date d] (if d (.getTime d) 0))

(defn trigger-state
  "The Trigger as the scheduler reads it, or nil when the object is not a Trigger."
  [db t]
  (let [s (objects/state db t)]
    (when (= :trigger (:object/class s))
      (let [f (fn [k] (get-in s [:object/fields k :field/value]))]
        {:id t :owner (f :trigger/owner) :kind (kw (f :trigger/kind)) :armed (true? (f :trigger/armed))
         :every-s (f :trigger/every-s) :at (f :trigger/at) :watch (f :trigger/watch)
         :route (f :trigger/route) :to (or (f :trigger/to) (f :trigger/owner)) :text (f :trigger/text)
         :armed-at (f :trigger/armed-at) :last-tick (f :trigger/last-tick) :last-fired (f :trigger/last-fired)
         :fired-count (long (or (f :trigger/fired-count) 0)) :max-fires (f :trigger/max-fires)
         :unless-empty (true? (f :trigger/unless-empty))
         :unless-told (true? (f :trigger/unless-told))}))))

(def ^:private told-re
  "A loop/stalled the asker has already heard: the wake's end told them, so nobody need look into it.
   Measured: a child stopped at its cap woke root for up to 8 turns on no question, about half the
   tokens of a failed task."
  #"^(iteration cap|cancelled by)\b")

(defn- old-news?
  "A stall the watcher need not wake for: the asker was told, or the Field did not change."
  [t body]
  (and (:unless-told t)
       (or (re-find told-re (str (:value body))) (= (:value body) (:previous body)))))

(defn armed-triggers
  "Every armed Trigger, as trigger-state maps."
  [db]
  (->> (objects/objects-of-class db :trigger)
       (remove #(objects/retired? db %))
       (map #(trigger-state db %))
       (filter :armed)
       (sort-by (comp str :id))
       vec))

(defn due?
  "Is a clock-driven Trigger due at now? :interval when every-s has passed since its last slot
   (last-tick, else armed-at); :at when its instant has come and it has not fired. Pure."
  [{:keys [kind every-s at last-tick armed-at fired-count]} ^Date now]
  (case kind
    :interval (and (number? every-s) (pos? every-s)
                   (>= (ms now) (+ (max (ms last-tick) (ms armed-at)) (long (* 1000 (double every-s))))))
    :at (and (inst? at) (>= (ms now) (ms at)) (zero? (long (or fired-count 0))))
    false))

(defn busy?
  "Is the owner's loop in a wake (loop/iteration > 0)?"
  [db owner]
  (let [i (field db owner :loop/iteration)]
    (and (number? i) (pos? i))))

(defn- note!
  "A bookkeeping Field on the Trigger, the world as actor, as for the shipped Fields."
  [world t k type v]
  (log/append!! (:log world) (log/pending :field-set t (:world-id world) {:name k :type type :value v :reader :both} :via t)))

(defn- first-route [db owner] (first (field db owner :principal/routes)))

(defn disarm!*
  "Disarmed, the world as actor (self-disarm after :at or max-fires)."
  [world t]
  (note! world t :trigger/armed :boolean false)
  (note! world t :trigger/state :keyword :disarmed))

(defn fire!
  "Fire Trigger t now for reason: the owner's Message (kind trigger, via t), the `fired` event, the
   books. extra, when given, is appended to trigger/text (an on-change value). Returns
   {:fired message-seq} or {:error reason}."
  [world {:keys [id owner kind to route text fired-count max-fires] :as t} ^Date now reason extra]
  (let [db (act/db world)
        route (or route (first-route db owner))
        body (str (or text (case kind :interval "heartbeat" :at "it is time" :on-change "a watched Field changed" :on-message "a watched Principal was written to" "trigger"))
                  (when (some? extra) (str "\n" extra)))
        sent (if route
               (route/send! world owner {:to #{to} :route route :text body :via id :hop 0 :kind :trigger :trigger id})
               act/unresolved)
        message (get-in sent [:ok :message])]
    (note! world id :trigger/last-tick :instant now)
    (if-let [e (:err sent)]
      (do (note! world id :trigger/last-error :string (str (name (:code e)) ": " (:message e)))
          (timbre/warn "trigger fired but its Message did not send" {:trigger id :owner owner :err e})
          {:error e})
      (let [n (inc (long fired-count))]
        (log/append!! (:log world) (log/pending :fired id owner {:trigger id :owner owner :message message :to to :reason (str reason)}
                                                :streams #{owner} :via id))
        (note! world id :trigger/last-fired :instant now)
        (note! world id :trigger/fired-count :number n)
        (when (or (= :at kind) (and (number? max-fires) (>= n (long max-fires))))
          (disarm!* world id))
        {:fired (get-in sent [:ok :seq]) :message message}))))

(defn- skip! [world {:keys [id]} ^Date now]
  (let [db (act/db world)]
    (note! world id :trigger/last-tick :instant now)
    (note! world id :trigger/skipped :number (inc (long (or (field db id :trigger/skipped) 0))))))

(defn tick!
  "Fire every due clock-driven Trigger whose owner is not busy; skip (and count) the busy ones.
   Returns {:fired [...] :skipped [...]}. Takes now so a test can drive the clock."
  [world ^Date now]
  (let [db (act/db world)]
    (reduce (fn [acc t]
              (if (and (#{:interval :at} (:kind t)) (due? t now))
                (if (busy? db (:owner t))
                  (do (skip! world t now) (update acc :skipped conj (:id t)))
                  (do (fire! world t now (name (:kind t)) nil) (update acc :fired conj (:id t))))
                acc))
            {:fired [] :skipped []}
            (armed-triggers db))))

(defn- blank-value? [v] (or (nil? v) (and (string? v) (str/blank? v)) (and (coll? v) (empty? v))))

(defn- watched-by
  "The armed event-driven Triggers an event matches: a field-set on watch.object (and watch.field
   when named), or a delivery to watch.object. A Trigger's own bookkeeping never matches."
  [db {:keys [event/kind event/stream event/body event/via]}]
  (for [t (armed-triggers db)
        ;; a Message a Trigger sent (via a Trigger) wakes no on-message Trigger: no cascade
        :when (not (and (= kind :delivered) via (= :trigger (:object/class (objects/state db via)))))
        :let [{:keys [object field]} (:watch t)]
        :when object
        :when (case (:kind t)
                :on-change (and (= kind :field-set) (= stream object)
                                (or (nil? field) (= (kw field) (:name body)))
                                (not (and (:unless-empty t) (blank-value? (:value body))))
                                (not (old-news? t body)))
                :on-message (and (= kind :delivered) (= (:recipient body) object))
                false)]
    t))

(defn on-events!
  "Fire the event-driven Triggers a committed batch matches. Returns the Trigger ids fired."
  [world events ^Date now]
  (let [db (act/db world)]
    (vec (for [e events
               :when (#{:field-set :delivered} (:event/kind e))
               t (watched-by db e)
               :let [extra (when (= :field-set (:event/kind e))
                             (str (some-> (get-in e [:event/body :name]) str (subs 1)) " = " (pr-str (get-in e [:event/body :value]))
                                  " (seq " (:event/seq e) ")"))]]
           (do (fire! world t now (name (:kind t)) extra) (:id t))))))

;; ---------------------------------------------------------------- ops

(defn- predicate->args
  "The spec's arm predicate as create args: schedule → :interval, state → :on-change."
  [{:keys [kind every-ms cron object field]}]
  (case (kw kind)
    :schedule (cond (number? every-ms) {:kind :interval :every-s (/ (double every-ms) 1000.0)}
                    cron (throw (ex-info "cron schedules are not built yet" {:code :not-implemented}))
                    :else (throw (ex-info "a schedule predicate needs every-ms" {:code :invalid})))
    :state {:kind :on-change :watch {:object object :field field}}
    (throw (ex-info (str "predicate kind " (some-> kind name) " is not built yet") {:code :not-implemented}))))

(defn arm!
  "arm {trigger}: armed, armed-at now (:write on the Trigger). arm {predicate evaluation as? once?}
   (spec 5.5): create the Trigger from the predicate as p, then arm it. Returns {:ok {:trigger :seq}}."
  [world p {:keys [trigger predicate once as] :as args}]
  (let [db (act/db world)]
    (cond
      (and as (not= as p)) act/unresolved
      trigger
      (if (= :trigger (:object/class (objects/state db trigger)))
        (if-let [{:keys [grant]} (resolve/resolve! world db p (act/id-name trigger) :write)]
          (let [ap (fn [k type v] (act/append! world (log/pending :field-set trigger p {:name k :type type :value v :reader :both} :delegation grant)))]
            (ap :trigger/armed :boolean true)
            (ap :trigger/armed-at :instant (Date.))
            {:ok {:trigger trigger :seq (ap :trigger/state :keyword :armed)}})
          act/unresolved)
        act/unresolved)
      predicate
      (let [cargs (try (predicate->args predicate) (catch clojure.lang.ExceptionInfo e (act/err (or (:code (ex-data e)) :invalid) (ex-message e))))]
        (if (:err cargs)
          cargs
          (let [{:keys [ok err]} (act/create! world p :trigger (cond-> (assoc cargs :armed true) once (assoc :max-fires 1)))]
            (if err {:err err} {:ok {:trigger (:object ok) :seq (:seq ok)}}))))
      :else (act/err :invalid "arm needs a trigger id or a predicate"))))

(defn disarm!
  "disarm {trigger}: :write on the Trigger. Returns {:ok {:seq}}."
  [world p {:keys [trigger]}]
  (let [db (act/db world)]
    (if (and trigger (= :trigger (:object/class (objects/state db trigger))))
      (if-let [{:keys [grant]} (resolve/resolve! world db p (act/id-name trigger) :write)]
        (do (act/append! world (log/pending :field-set trigger p {:name :trigger/armed :type :boolean :value false :reader :both} :delegation grant))
            {:ok {:seq (act/append! world (log/pending :field-set trigger p {:name :trigger/state :type :keyword :value :disarmed :reader :both} :delegation grant))}})
        act/unresolved)
      act/unresolved)))

;; ---------------------------------------------------------------- component

(defn start!
  "Subscribe to the log and run the scheduler thread. Returns the component."
  [world]
  (let [queue (LinkedBlockingQueue.)
        running (atom true)
        key (log/subscribe! (:log world)
                            (fn [batch]
                              (when (and @running (some #(#{:field-set :delivered} (:event/kind %)) batch))
                                (.offer queue batch))))
        last-expire (atom (System/currentTimeMillis))
        last-delegate (atom (System/currentTimeMillis))
        ;; the Model probe, first 15 s after start, then every probe-every-ms
        last-probe (atom (- (+ (System/currentTimeMillis) 15000) models/probe-every-ms))
        last-sweep (atom (System/currentTimeMillis))
        sweep-every (long (get-in world [:config :retention :every-ms] (:every-ms retention/defaults)))
        thread (-> (Thread/ofVirtual) (.name "ke-scheduler")
                   (.start (fn []
                             (while @running
                               (try
                                 (when-let [batch (.poll queue tick-ms TimeUnit/MILLISECONDS)]
                                   (loop [b batch]
                                     (on-events! world b (Date.))
                                     (when-let [more (.poll queue)] (recur more))))
                                 (tick! world (Date.))
                                 (when (> (- (System/currentTimeMillis) @last-delegate) delegate-every-ms)
                                   (reset! last-delegate (System/currentTimeMillis))
                                   (delegate/sweep! world (Date.))
                                   (gateway/poll-all! world))
                                 (when (> (- (System/currentTimeMillis) @last-expire) expire-every-ms)
                                   (reset! last-expire (System/currentTimeMillis))
                                   (gates/expire! world)
                                   (gates/expire-unsafe! world)
                                   (notify/flush! world))
                                 (when (> (- (System/currentTimeMillis) @last-probe) models/probe-every-ms)
                                   (reset! last-probe (System/currentTimeMillis))
                                   (models/probe-all! world))
                                 (when (> (- (System/currentTimeMillis) @last-sweep) sweep-every)
                                   (reset! last-sweep (System/currentTimeMillis))
                                   (retention/kick! world))
                                 (catch InterruptedException _ (reset! running false))
                                 (catch Throwable t (timbre/warn t "scheduler pass failed")))))))]
    (timbre/info "scheduler started" {:armed (count (armed-triggers (act/db world)))})
    {:queue queue :running running :key key :thread thread}))

(defn stop! [{:keys [running key ^Thread thread]} world]
  (when running
    (reset! running false)
    (log/unsubscribe! (:log world) key)
    (when thread (.interrupt thread) (.join thread 2000))))
