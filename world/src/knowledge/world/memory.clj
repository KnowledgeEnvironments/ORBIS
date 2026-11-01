(ns knowledge.world.memory
  "Memory for every Principal (sprint item W5; spec 4.3 memory fields carry
   authorship). A convention and defaults, no mechanism: memory/notes is the Principal's own durable
   notes, written by itself from inside its loop; memory/policy says when; memory/budget caps it;
   memory/retention-days is for the compaction a later pass adds. The serializer renders a
   Principal's memory/* Fields whole, first, every time.")

(def policy
  "At the end of a wake, if you learned something durable about this world, the people in it, or a project, update memory/notes; keep it under memory/budget characters; prefer facts with seqs.")

(def defaults
  {:memory/notes ""
   :memory/policy policy
   :memory/budget 4000
   :memory/retention-days 30})

(def types
  {:memory/notes :string :memory/policy :string :memory/budget :number :memory/retention-days :number})

(defn missing
  "The memory defaults a Principal's Fields lack."
  [fields]
  (into {} (remove (fn [[k _]] (contains? fields k))) defaults))
