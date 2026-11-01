(ns knowledge.world.scratch.policy
  "What an off-the-record session may do: an allow-list, so a new op is refused until someone
   decides otherwise. Reading stays inside resolution as the Principal; a Field write lands in the
   in-memory child and vanishes; anything that reaches past the world process is refused, including
   a shell, since a shell cannot be proven read-only."
  (:require [knowledge.world.fold.objects :as objects]))

(def reading-ops #{:read :resolve :history :search})
(def inside-actions #{"search.find" "view.present" "view.table-of" "view.graph-of" "cost.report"
                      "doc.show" "doc.candidates" "doc.overview" "doc.contents" "field.diff"})

(defn- refusal [what]
  {:code :scratch-readonly
   :message (str what ": this is an off-the-record session; nothing here persists and you cannot act outside it")})

(defn- only-the-person?
  "A send may reach the person who opened the session and nobody else. Absent to means the default,
   the wake's sender, which is that person."
  [world to]
  (let [them (str (:scratch-person world))]
    (or (nil? to) (= #{them} (set (map str (if (coll? to) to [to])))))))

(defn refused
  "The err for op inside this scratch, or nil when it may run."
  [world db op args]
  (cond
    (reading-ops op) nil
    (= :set-field op) (when (:object/backing (objects/entity db (:object args)))
                        (refusal "a Field backed by a file would reach the disk"))
    (#{:send :present} op) (when-not (only-the-person? world (:to args))
                             (refusal "a Message to anyone else leaves the session"))
    (= :act op) (let [a (some-> (:action args) name)]
                  (when-not (and (nil? (:object args)) (nil? (:class args)) (inside-actions a)
                                 (only-the-person? world (get-in args [:args :to])))
                    (refusal (str "act " a))))
    :else (refusal (name op))))
