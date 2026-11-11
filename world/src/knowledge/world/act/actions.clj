(ns knowledge.world.act.actions
  "Action types (plan §19.1 W3, decisions 2026-10-30 'named actions'). An action is an Object bound
   in the world namespace as `action/<name>` with the Fields action/name, action/schema (the Malli
   form of its args, as EDN text), action/handler (the keyword a world-side handler answers to),
   action/gated (false until approval gates arrive) and instructions/own (one paragraph for the
   model: when to use it). `act {action \"<name>\" args {...}}` with no object resolves the action
   Object for :evaluate (ground rule 4), validates the args against action/schema, and dispatches
   to the handler as the caller. Handlers (knowledge.world.act.handlers) compose the existing
   primitives only: create!, bind!, set-field!, grant!, perform!. Nothing here is a new class.

   The Acting section of every Serialization lists the actions a Principal can resolve, with their
   argument shapes, so the model composes from a few named gestures instead of a dozen raw ops;
   root did the first project setup by hand in eight iterations and 46k prompt tokens, which is
   what these collapse."
  (:require [clojure.edn :as edn]
            [clojure.string :as str]
            [clojure.walk :as walk]
            [malli.core :as m]
            [malli.error :as me]
            [knowledge.world.act.core :as act]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.projects :as projects]
            [knowledge.world.resolve.core :as resolve]))

(def id-string
  "An Object id in the JSON form (a string pattern, so the form round-trips through EDN)."
  [:re "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"])

(def grant-ops ["read" "reference" "transmit" "write" "delegate" "evaluate" "create"])

(def scope-schema
  [:or [:enum "everything"]
   [:map [:prefix {:optional true} :string] [:class {:optional true} :string] [:field-prefix {:optional true} :string]]
   [:vector id-string]])

(def match-schema
  "What a standing approval rule matches on a shell run (knowledge.world.act.rules)."
  [:map [:programs {:optional true} [:vector :string]]
   [:preset {:optional true} [:enum "read-only" "test"]]
   [:args-prefix {:optional true} [:vector :string]]
   [:paths {:optional true} [:vector :string]]])

(def people-only
  "Actions a model has no use for, kept out of the Acting section: a loop cannot decide a gate or
   write a rule, so naming them costs tokens and invites a refusal."
  #{"task.approve" "task.deny" "task.decide-many" "approval.rule.add" "approval.rule.remove"
    "approval.rule.list" "approval.rule.test" "approval.unsafe" "approval.unsafe.off"
    "adapter.unconfine" "adapter.confine" "pane.share" "pane.unshare"
    "model.choose" "model.chosen" "model.signin.begin" "model.signin.finish" "model.signout" "conversation.rename"
    "notify.list" "notify.read" "image.read" "image.open" "image.show" "tex.render" "tex.read" "tex.open" "tex.log"
    ;; the manual costs one line of the Acting text, not four of Actions
    "doc.show" "doc.candidates" "doc.overview" "doc.contents" "field.diff" "conversation.compressor" "path.complete"
    ;; for a model too, but only when maintaining: instructions/harness names it
    "world.restart" "release.make"})

(def definitions
  "The shipped actions: name, handler, args schema (Malli form, data), instructions."
  [{:name "principal.create"
    :hint "a new Principal: charter, Route, your Model, narrower Grant"
    :handler :principal.create
    :schema [:map
             [:name :string]
             [:charter {:optional true} :string]
             ;; temporary: for a Task, or for a time. Retired then, Grants and all
             [:for {:optional true} :string]
             [:ttl-s {:optional true} :int]
             ;; a developer is made from the shipped template and owns a harness subtree,
             ;; mounted by this action itself (root kept reusing its own mount instead)
             [:template {:optional true} [:enum "developer"]]
             [:subtree {:optional true} :string]
             [:check {:optional true} :string]
             [:driven-by {:optional true} [:enum "process" "person"]]
             [:model {:optional true} id-string]
             [:routes-from {:optional true} id-string]
             [:grant [:map [:scope scope-schema] [:ops [:vector (into [:enum] grant-ops)]]]]
             [:actions {:optional true} [:vector :string]]]
    :instructions (str "Make a new Principal in one gesture: it gets instructions/charter from `charter`, a Route "
                       "`<you>.<name>` between you and it (route/members both, in both principal/routes), your model "
                       "Model bound with an evaluate Grant (or `model`), loop/accepts-from = you, empty memory/notes, "
                       "and the Grant you name, which must be strictly narrower than yours (err narrower-required "
                       "otherwise). A process Principal's token is written to data/<name>.token, never returned to a "
                       "loop. `actions` names further actions it may call (task.run is given by default). Write the "
                       "charter after reading what the Principal is for; one paragraph that says what it owns and "
                       "that it asks you on its Route for anything beyond that. `grant.scope {prefix <name>}` covers its own "
                       "names (<name>, <name>.fs, <name>.shell, its Route); `{class \"task\"}`, `field-prefix` and their "
                       "combinations cover every Object that matches; never include delegate in ops. "
                       "A developer of this harness: `template \"developer\"` with `subtree` (listener or world) and "
                       "optionally `check` (its test command); the charter is filled from the shipped template and the "
                       "subtree is mounted for the new Principal as <name> with a shell at the repository root, in this "
                       "one action: do not mount it yourself first.")}
   {:name "delegate"
    :hint "hand one Task to a temporary subagent, with a deadline; the result wakes you"
    :handler :delegate
    :gated true
    :schema [:map [:task :string] [:charter {:optional true} :string] [:deadline-s {:optional true} :int]
             [:ttl-s {:optional true} :int] [:grant {:optional true} :map]]
    :instructions (str "`task` is what to do, whole: the subagent knows nothing else. It reads your mounts unless `grant` says less, "
                       "and can write nothing. `deadline-s` defaults to 900: you hear once if it passes. It is retired when the "
                       "Task ends, and its record stays on the log. For long-running work use principal.create.")}
   {:name "task.progress"
    :hint "how a Task is going, on the Task, waking nobody"
    :handler :task.progress
    :public true
    :schema [:map [:text :string] [:task {:optional true} :string]]
    :instructions "Writes task/progress on your Task, or on `task`. Nobody wakes for it; anyone who can read the Task sees it."}
   {:name "task.ask"
    :hint "ask your parent a question; your Task stays open"
    :handler :task.ask
    :public true
    :schema [:map [:text :string]]
    :instructions "For a subagent that cannot go on. It wakes your parent, ends this wake, and the answer wakes you."}
   {:name "projects"
    :hint "every project you can see: what it is, its last commit, its Principal's last work"
    :handler :projects
    :public true
    :schema [:map]
    :instructions (str "A project is a mount, read from the record and the project itself: {name path owner state contents "
                       "about git tests last-run last-said}. It runs no command and needs no card.")}
   {:name "project.add"
    :hint "a directory as a project, Principal to Grant, in one op"
    :handler :project.add
    :gated true
    :schema [:map [:name {:optional true} :string] [:path {:optional true} :string] [:url {:optional true} :string]
             [:charter {:optional true} :string] [:model {:optional true} :string]]
    :instructions (str "`name` is short and becomes the Principal, the mount and the prefix of a Grant strictly narrower than "
                       "yours. `path` is a directory on this machine; you need no read of it first. Or `url`, a git remote "
                       "such as git@github.com:me/thing.git: the world clones it with the machine's key to projects/<name> under "
                       "its data directory, and `name` defaults from the URL. One op per project. The result has the "
                       "first lines of its README and its test command. Give `charter`, or set instructions/charter from the "
                       "README afterwards: one paragraph on what it owns.")}
   {:name "workspace.mount"
    :hint "a directory as a mount + shell for a Principal, with its Grants"
    :handler :workspace.mount
    :schema [:map
             [:principal :string]
             [:path :string]
             [:name :string]
             [:shell {:optional true} :boolean]
             [:read-only {:optional true} :boolean]]
    :instructions (str "Give an existing Principal (`principal`: its id or bound name; principal.create it first) a working "
                       "directory: an fs Adapter at `path` mounted as `name` (so "
                       "`<name>/<file>` resolves for it), a shell Adapter rooted there as `<name>.shell` unless "
                       "`shell` is false, both in its principal/adapters, with Grants on the mount prefix (read, "
                       "reference, and write unless `read-only`) and on the Adapters (evaluate on the shell). "
                       "The path must lie inside one of your own mounts unless your Grant is scope everything "
                       "(err outside-mount).")}
   {:name "route.create"
    :hint "a Route between Principals that share none"
    :handler :route.create
    :schema [:map [:name :string] [:members [:vector id-string]]]
    :instructions (str "A Route named `name` between `members` on the listener transport: route/members is set "
                       "and the Route is added to each member's principal/routes that you may write. Use it to "
                       "connect two Principals that do not share a Route yet.")}
   {:name "loop.dequeue"
    :hint "take a waiting Message out of a Principal's queue"
    :handler :loop.dequeue
    :schema [:map [:principal id-string] [:message id-string]]
    :instructions (str "Remove a wake from loop/queue before it starts. Its sender may, or anyone who can write "
                       "the Principal. The Message stays on the log, marked message/dequeued.")}
   {:name "loop.reorder"
    :hint "move a waiting Message in a Principal's queue"
    :handler :loop.reorder
    :schema [:map [:principal id-string] [:message id-string] [:position :int]]
    :instructions (str "Move a wake within loop/queue; position 0 is next. Its sender may, or anyone who can "
                       "write the Principal.")}
   {:name "object.rename"
    :hint "rename an Object"
    :handler :object.rename
    :schema [:map [:object :string] [:name :string]]
    :instructions (str "Give an Object a new name (id or current name; you must write it). The old name stops "
                       "resolving at once; a taken name answers err conflict. Names that merely start with the "
                       "old one (root.route) stay as they are and come back as `related`, and Grants scoped to "
                       "the old name's prefix as `prefix-grants`; rename or reissue those yourself.")}
   {:name "object.retire"
    :hint "delete an Object; a person decides"
    :handler :object.retire
    :gated true
    :schema [:map [:object :string] [:reason {:optional true} :string] [:force {:optional true} :boolean]]
    :instructions (str "Delete an Object (id or name; you must write it). It stops resolving and its names become free, "
                       "but the log keeps it and it reads back by id. A Model, Adapter or Route a live Principal still "
                       "uses is refused with the list of them unless `force`; root, the world and the first person "
                       "cannot be retired. From a loop this waits for a person. object.restore undoes it.")}
   {:name "object.restore"
    :hint "bring back a retired Object"
    :handler :object.restore
    :schema [:map [:object :string]]
    :instructions (str "Undo object.retire (by id; you must be able to write it). Its old names come back where they are "
                       "still free; taken ones are listed as `names-taken`.")}
   {:name "field.clear"
    :hint "remove a Field from an Object"
    :handler :field.clear
    :schema [:map [:object :string] [:field :string]]
    :instructions (str "Remove a Field (you must write the Object). It leaves reads and your prompt; the log keeps its "
                       "history. To blank a value but keep the Field, use set-field.")}
   {:name "route.join"
    :hint "add a Route to a Principal"
    :handler :route.join
    :schema [:map [:route id-string] [:principal id-string]]
    :instructions "Add an existing Route to a Principal's principal/routes (you must write that Principal)."}
   {:name "world.diff"
    :hint "what this fork changed since it was forked (only in a fork)"
    :handler :world.diff
    :schema [:map [:after {:optional true} :int] [:limit {:optional true} :int]]
    :instructions (str "Only in a fork of another world: the events this world added since the fork, with a summary "
                       "(objects created, Fields set by object, Messages, Evaluations). Not a fork -> err conflict.")}
   {:name "harness.index"
    :hint "refresh harness/index and harness/head on yourself"
    :handler :harness.index
    :schema [:map [:adapter {:optional true} id-string]]
    :instructions "Refresh harness/index and harness/head on yourself from the `harness` mount (or `adapter`)."}
   {:name "approval.rule.add"
    :hint "a person's standing rule: gated ops that fit it run without a card"
    :handler :approval.rule.add
    :schema [:map [:principal :string] [:action :string]
             [:match {:optional true} match-schema]
             [:until {:optional true} :string]
             [:uses {:optional true} :int]
             [:note {:optional true} :string]]
    :instructions (str "Person only. `action` is the gate word (task.run, shell, fs:write, grant, or a named action); "
                       "`match` narrows a shell run: `preset` read-only or test, `programs`, `args-prefix`, `paths`. "
                       "An operator, a substitution or a path out of the Adapter's root never matches: such a run gets "
                       "a card. `until` (ISO instant) and `uses` end the rule.")}
   {:name "approval.rule.remove"
    :hint "remove a standing rule"
    :handler :approval.rule.remove
    :schema [:map [:principal :string] [:rule :string]]
    :instructions "Person only. Remove a rule from a Principal's policy/approval-rules."}
   {:name "approval.rule.list"
    :hint "the standing rules, as a table"
    :handler :approval.rule.list
    :schema [:map [:principal {:optional true} :string]
             [:to {:optional true} [:vector id-string]] [:route {:optional true} id-string]]
    :instructions "Person only. Every Principal's rules, or one's: id, action, match, uses, expiry, who made it."}
   {:name "approval.rule.test"
    :hint "would this command match: a dry run with the reason"
    :handler :approval.rule.test
    :schema [:map [:principal :string] [:cmd :string]
             [:rule {:optional true} :string]
             [:match {:optional true} match-schema]
             [:adapter {:optional true} :string]
             [:cwd {:optional true} :string]]
    :instructions (str "A dry run: does `cmd` fit `match` (or the Principal's rule `rule`, or any of its rules)? "
                       "Answers {matches reason}; nothing is saved or run.")}
   {:name "notify"
    :hint "get a person's attention, without a Route"
    :handler :notify
    :public true
    :schema [:map [:text :string] [:level {:optional true} [:enum "info" "attention" "urgent"]]
             [:object {:optional true} :string]]
    :instructions (str "Tells the person who owns you, Route or not; it does not wake anyone. `level` info by default; urgent "
                       "is for what must not wait. `object` names something you can read. Six a minute, then counted.")}
   {:name "notify.list"
    :hint "your notifications, newest first"
    :handler :notify.list
    :public true
    :schema [:map [:unread {:optional true} :boolean] [:limit {:optional true} [:int {:min 1 :max 200}]]]
    :instructions "Yours only: {collection unread rows}. `unread` true leaves out what you have read."}
   {:name "notify.read"
    :hint "mark notifications read"
    :handler :notify.read
    :public true
    :schema [:map [:notifications {:optional true} [:vector id-string]]]
    :instructions "Marks the ones named read, or all of yours when none are named."}
   {:name "image.put"
    :hint "store an image for send"
    :handler :image.put
    :public true
    :schema [:map [:path {:optional true} :string] [:name {:optional true} :string] [:mount {:optional true} :string]
             [:base64 {:optional true} :string] [:upload {:optional true} :string] [:more {:optional true} :boolean]]
    :instructions (str "`path` a file under a mount or shell root, or `name` a mounted file such as harness/docs/a.png. "
                       "Answers `sha256`: `send` it with `images [sha256]`. If your Model takes images you see it on your next turn. "
                       "A client sends `base64`, in pieces under one `upload` id with `more` true until the last.")}
   {:name "image.show"
    :hint "look at an image a Message carried"
    :handler :image.show
    :public true
    :schema [:map [:message :string] [:index {:optional true} :int]]
    :instructions "The image comes on your next turn if your Model takes images; otherwise you get its name, type and size."}
   {:name "image.read"
    :hint "an image's bytes"
    :handler :image.read
    :public true
    :schema [:map [:message :string] [:index {:optional true} :int] [:offset {:optional true} :int]]
    :instructions "Base64, a chunk at a time: {mime size offset base64 more}. For clients."}
   {:name "tex.render"
    :hint "typeset a formula, a TeX fragment or a whole document"
    :handler :tex.render
    :public true
    :schema [:map [:source :string]]
    :instructions "A client's call, a person's own session only. A formula, a text fragment, or a whole document's first page. {sha256 width height pages kind}, width and height in points, or err renderer-preparing while packages are fetched once."}
   {:name "tex.log"
    :hint "what TeX said about a source that failed"
    :handler :tex.log
    :public true
    :schema [:map [:source :string]]
    :instructions "A client's call, a person's own session only. {log}."}
   {:name "tex.open"
    :hint "a typeset source's first page, as a pane"
    :handler :tex.open
    :public true
    :schema [:map [:source :string] [:title {:optional true} :string]]
    :instructions "A client's call, a person's own session only, after tex.render. {pane kind}."}
   {:name "tex.read"
    :hint "a typeset formula's bytes"
    :handler :tex.read
    :public true
    :schema [:map [:source :string] [:offset {:optional true} :int]]
    :instructions "Base64 PNG, a chunk at a time: {size offset base64 more}."}
   {:name "image.open"
    :hint "open a Message's image in a pane"
    :handler :image.open
    :public true
    :schema [:map [:message :string] [:index {:optional true} :int]]
    :instructions "An image pane on what the Message carried."}
   {:name "path.complete"
    :hint "what a path under a mount could go on to be"
    :handler :path.complete
    :public true
    :schema [:map [:mount :string] [:path {:optional true} :string]]
    :instructions (str "Lists the directory `path` ends in, keeping the names its last part begins: "
                       "{mount dir stem entries}, a directory ending in /. It creates nothing, and needs read on the mount.")}
   {:name "conversation.rename"
    :hint "rename a conversation"
    :handler :conversation.rename
    :public true
    :schema [:map [:thread id-string] [:title :string]]
    :instructions "Set the title shown for a conversation. A person's own session only; the person must be able to write its root Message."
   }
   {:name "conversation.compressor"
    :hint "give a Principal its own summary prompt, from the shipped one"
    :handler :conversation.compressor
    :schema [:map [:principal :string]]
    :instructions "Writes the shipped prompt into serialize/compressor on that Principal, to edit. It needs write on it."}
   {:name "doc.show"
    :hint "a manual topic, whole"
    :handler :doc.show
    :public true
    :schema [:map [:topic :string]]
    :instructions (str "Show Documentation: the topic named, by name, title, or action, Field or class name. Answers "
                       "{topic title type book format body previous next}. No such topic answers {found false candidates}.")}
   {:name "doc.candidates"
    :hint "the manual's topics that answer some words"
    :handler :doc.candidates
    :public true
    :schema [:map [:text :string]
             [:type {:optional true} [:enum "concept" "section" "action" "field" "class"]]
             [:limit {:optional true} [:int {:min 1 :max 50}]]]
    :instructions "Show Candidates: {candidates [{topic title type book snippet}] total}, best first."}
   {:name "doc.overview"
    :hint "a manual topic in a paragraph, or the manual's books"
    :handler :doc.overview
    :public true
    :schema [:map [:topic {:optional true} :string]]
    :instructions "Show Overview: a topic's first paragraph and its neighbours, or with no topic, the books."}
   {:name "doc.contents"
    :hint "the table of contents of a book of the manual"
    :handler :doc.contents
    :public true
    :schema [:map [:book {:optional true} :string]]
    :instructions "Show Table of Contents: every topic of a book, or with no book, the books."}
   {:name "field.diff"
    :hint "what changed in a Field"
    :handler :field.diff
    :public true
    :schema [:map [:object :string] [:field :string] [:since {:optional true} :int]]
    :instructions (str "The last change to a Field as a line diff, or everything after seq `since`: "
                       "{object field changed changes by seq diff}. You need read on the Object.")}
   {:name "release.make"
    :hint "a release of a commit in the working copy"
    :handler :release.make
    :schema [:map [:rev {:optional true} :string]]
    :instructions (str "Makes a read-only release under releases/ from `rev` (default HEAD) in the working copy, and "
                       "answers its commit, for world.restart. Your shell cannot write there, and that is the point.")}
   {:name "world.restart"
    :hint "restart this world, on this release or a new one"
    :handler :world.restart
    :gated true
    :schema [:map [:reason :string] [:release {:optional true} :string]]
    :instructions (str "Asks the supervisor to bring the world back, on `release` (a commit under releases/) or the running "
                       "one. Your wake carries on afterwards. A new release always waits for a person's own "
                       "approval; a failed health check returns to the last good release by itself.")}
   {:name "approval.unsafe"
    :hint "one Principal's gated ops run without a card, for a limited time"
    :handler :approval.unsafe
    :schema [:map [:principal :string] [:minutes [:int {:min 1 :max 480}]]
             [:except {:optional true} [:vector :string]]]
    :instructions (str "Person only. For `minutes` (at most 480) a gated op this Principal asks for itself runs without a "
                       "card, each with a Task as its record. `except` lists what still gets a card; `*` at the end "
                       "of a word is a prefix; left out, grant, object.retire, principal.create, token.* and policy/*. "
                       "Other Principals keep their gates, and so does work another gated Principal asks of this one.")}
   {:name "approval.unsafe.off"
    :hint "end a Principal's unsafe mode"
    :handler :approval.unsafe.off
    :schema [:map [:principal :string]]
    :instructions "Person only. Gated ops of this Principal need approval again, starting with the next one."}
   {:name "retention.set"
    :hint "how much disk a world's blobs and log may take; a person's own session"
    :handler :retention.set
    :schema [:map [:gb [:or [:enum "infinite"] :int :double]] [:confirm {:optional true} :boolean]]
    :instructions (str "Person only, from their own session. `gb` is the limit on blobs and log segments together, in GiB, "
                       "or \"infinite\" for no pruning. Over it the least recently used blobs go first, then the oldest "
                       "log segments, and what they held does not come back. A limit below what the world uses now answers "
                       "with what the next sweep would take and sets nothing until `confirm` is true. Saved in "
                       "retention/limit-gb on the world; retention/last-sweep says what the last sweep took.")}
   {:name "adapter.unconfine"
    :hint "run one Adapter's shells outside the sandbox, for a limited time"
    :handler :adapter.unconfine
    :gated true
    :schema [:map [:adapter :string] [:minutes [:int {:min 1 :max 480}]]]
    :instructions (str "Person only, and from a loop always a person's own card: no rule and no unsafe mode admits it. "
                       "For `minutes` (at most 480) this Adapter's commands and terminals can read what the world's "
                       "user can, credentials included. It shows in world/unsafe. approval.unsafe.off ends it.")}
   {:name "adapter.confine"
    :hint "what one Adapter's sandbox lets through: network, extra read paths"
    :handler :adapter.confine
    :schema [:map [:adapter :string]
             [:network {:optional true} [:enum "model" "none" "all"]]
             [:read {:optional true} [:vector :string]]
             [:ssh {:optional true} :boolean]
             [:leash {:optional true} :boolean]]
    :instructions (str "A person's, or root's: what a Principal needs, it asks root for. Tightens or opens one shell. `network`: all (the default), model (loopback and the "
                       "Models' ports), or none. `ssh`: use of this machine's keys through its agent, on by default; the key "
                       "itself is never readable. `read`: paths outside the root its commands may read. `leash` false puts root's own "
                       "shell in the box: a person's alone.")}
   {:name "task.decide-many"
    :hint "approve or deny several pending approval Tasks at once"
    :handler :task.decide-many
    :schema [:map
             [:tasks {:optional true} [:vector id-string]]
             [:filter {:optional true} [:map [:requested-by {:optional true} :string]
                                        [:action {:optional true} :string]
                                        [:thread {:optional true} id-string]]]
             [:decision [:enum "approve" "deny"]]
             [:reason {:optional true} :string]]
    :instructions (str "Person only. Each Task is decided exactly as task.approve or task.deny would: the requester "
                       "never decides its own, and only a named approver decides. Answers one result per Task.")}
   {:name "task.approve"
    :hint "approve a pending approval Task: its op runs as the requester"
    :handler :task.approve
    :schema [:map [:task id-string] [:note {:optional true} :string]
             [:remember {:optional true} [:map [:match {:optional true} match-schema]
                                          [:until {:optional true} :string]
                                          [:uses {:optional true} :int]]]]
    :instructions (str "Decide a pending approval Task you were asked about (message/kind approval, message/task): "
                       "the op it holds runs as the Principal that asked, under that Principal's Grants, with the "
                       "Task as via; the Task moves approved then done or failed, task/result holds the outcome, and the "
                       "requester is told on its Route (message/kind approval-result). err conflict when it is no longer pending.")}
   {:name "task.deny"
    :hint "deny a pending approval Task with a reason"
    :handler :task.deny
    :schema [:map [:task id-string] [:reason :string]]
    :instructions (str "Refuse a pending approval Task: it moves to denied with task/reason and the requester is told "
                       "the reason on its Route. Nothing runs.")}
   {:name "pane.open"
    :hint "open a live terminal, file manager, editor or viewer; answers its pane id"
    :handler :pane.open
    :schema [:map
             [:kind [:enum "terminal" "files" "editor" "markdown" "latex" "image"]]
             [:target {:optional true} :string]
             [:adapter {:optional true} :string]
             [:path {:optional true} :string]
             [:object {:optional true} :string]
             [:field {:optional true} :string]
             [:title {:optional true} :string]
             [:cols {:optional true} :int]
             [:rows {:optional true} :int]
             [:cwd {:optional true} :string]]
    :instructions (str "Open a pane, a live surface that belongs to the world and outlives every client. `kind` terminal "
                       "needs `target`, a shell Adapter (`<you>.shell`) you may evaluate. files, editor, markdown, latex and "
                       "image take `target`, an fs Adapter or `<mount>/<path>`, or `path` inside one of your mounts. "
                       "kind editor with `object` and `field` opens a text Field instead: `save` writes it with set-field. "
                       "A person you are answering gets the pane in front of them.")}
   {:name "pane.list"
    :hint "the panes you can see"
    :handler :pane.list
    :schema [:map [:principal {:optional true} :string]]
    :instructions "The panes you may read: kind, state, title, owner. `principal` narrows to one owner's."}
   {:name "pane.close"
    :hint "end a pane and its process"
    :handler :pane.close
    :schema [:map [:pane id-string]]
    :instructions "End a pane: a terminal's process is killed, its record and scrollback stay. A client leaving does not do this."}
   {:name "pane.resize"
    :hint "new terminal size"
    :handler :pane.resize
    :schema [:map [:pane id-string] [:cols [:int {:min 2 :max 500}]] [:rows [:int {:min 2 :max 300}]]]
    :instructions "A new size for a terminal: the program is told and every attached client repaints."}
   {:name "pane.read"
    :hint "what a pane shows now, as text"
    :handler :pane.read
    :schema [:map [:pane id-string]
             [:what {:optional true} [:enum "screen" "scrollback" "buffer" "listing" "bytes"]]
             [:range {:optional true} [:vector :int]]
             [:field {:optional true} :string]
             [:offset {:optional true} :int]
             [:limit {:optional true} :int]]
    :instructions (str "What a pane holds now, exactly: a terminal's `screen` (default) or `scrollback`, an editor's `buffer` "
                       "with its `version`, a file pane's `listing`. `bytes` is for clients drawing an image or a page: "
                       "base64 chunks of `field` (pane/image, pane/pdf) from `offset`, with `more`. Reads are never gated.")}
   {:name "pane.send"
    :hint "type into a terminal by the pane id pane.open answered"
    :handler :pane.send
    :schema [:map [:pane id-string] [:text {:optional true} :string] [:keys {:optional true} [:vector :string]]
             [:raw {:optional true} :boolean] [:wait-ms {:optional true} :int]]
    :instructions (str "Bytes to a terminal: `text`, or `keys` by name (enter, tab, esc, up, down, left, right, pgup, pgdn, "
                       "ctrl-c ...). A command needs its newline. Needs :evaluate on the shell Adapter; from a model it "
                       "passes the approval gate like a shell command, and the result's `screen` is the terminal once "
                       "output goes quiet (`wait-ms`, default 8000): report from that, or pane.read for more.")}
   {:name "pane.share"
    :hint "give one agent your terminal, to read or to type, for a time"
    :handler :pane.share
    :schema [:map [:pane id-string] [:principal :string] [:mode [:enum "read" "type"]] [:minutes [:int {:min 1 :max 480}]]]
    :instructions (str "A person's own session only. A terminal a person opened runs outside the box and no agent can "
                       "see it; this gives one agent that terminal. `read`: pane.read and the stream. `type`: pane.send "
                       "and pane.frob too, each raising a card unless a person's standing rule admits it, and unsafe mode "
                       "never does. Ends by itself after `minutes`, at most 480, or by pane.unshare. It shows in the red "
                       "indicator while it lasts.")}
   {:name "pane.unshare"
    :hint "end an agent's share of your terminal"
    :handler :pane.unshare
    :schema [:map [:pane id-string] [:principal {:optional true} :string]]
    :instructions (str "A person's own session only. Ends `principal`'s share, or every share of this pane with no "
                       "`principal`. Already ended answers the same.")}
   {:name "pane.frob"
    :hint "a verb on a file, editor or viewer pane"
    :handler :pane.frob
    :schema [:map [:pane id-string] [:verb :string] [:args {:optional true} :map]]
    :instructions (str "A verb from the pane's table. files: cd {path}, list {path?}, select {names}, open {name, as?}, "
                       "mkdir {name}, rename, copy, move {from to}, delete {name}. editor: goto {line}, find {text}, "
                       "insert {line|at, text, version?}, replace {find, with, all?, version?}, undo, redo, save {force?}, revert. "
                       "markdown, latex: scroll {line}, goto {line}, render. Whoever edits holds the buffer until saving; "
                       "anyone else gives the `version` they read, or gets err conflict. "
                       "save, mkdir, rename, copy, move and delete change the disk, so a model's pass the gate.")}
   {:name "file.open"
    :hint "open a file in a person's pane"
    :handler :file.open
    :schema [:map
             [:path {:optional true} :string]
             [:name {:optional true} :string]
             [:as {:optional true} [:enum "editor" "markdown" "latex" "image" "files"]]
             [:title {:optional true} :string]]
    :instructions (str "Show a file: `name` (`<mount>/<path>`) or `path` inside a mount you can read. The pane's kind follows "
                       "the file (text editor, markdown, latex, image, a directory's file manager), or `as`, and it goes to "
                       "the person you are answering. Reading is ungated.")}
   {:name "view.present"
    :hint "a view a client renders"
    :handler :view.present
    :schema [:map
             [:kind [:enum "text" "table" "tree" "graph" "image" "tex"]]
             [:title :string]
             [:data :map]
             [:to {:optional true} [:vector id-string]]
             [:route {:optional true} id-string]
             [:about {:optional true} [:vector id-string]]
             [:in-reply-to {:optional true} id-string]]
    :instructions (str "Present a view to people: a Serialization of kind view (view/kind, view/title, view/data) sent "
                       "as a Message of kind view on `route` to `to` (both default to the Message you are answering: "
                       "its Route and its sender). Their client renders it; your reply text still says what it shows. "
                       "Data shapes: text {markdown}; table {columns [{key label align?}], rows [[...]]} (500 rows at "
                       "most, more is clipped and view/truncated set); tree {root {label children [...] ref?}}; graph "
                       "{nodes [{id label class? ref?}], edges [{from to label?}]}; image {mime, base64} up to 256 KiB. "
                       "`about` lists the Objects the view was made from. Prefer view.table-of and view.graph-of, which "
                       "read the Fields for you.")}
   {:name "view.table-of"
    :hint "a table of Objects' Fields (ids or a class)"
    :handler :view.table-of
    :schema [:map
             [:objects {:optional true} [:vector id-string]]
             [:class {:optional true} [:enum "object" "principal" "group" "message" "task" "evaluation" "trigger" "model" "route" "grant" "adapter" "serialization"]]
             [:fields [:vector :string]]
             [:title {:optional true} :string]
             [:to {:optional true} [:vector id-string]]
             [:route {:optional true} id-string]]
    :instructions (str "Show people a table of Objects: `objects` (ids) or `class` (every Object of that class you can "
                       "read, e.g. class principal for all Principals); one row per Object, columns name, class and each "
                       "Field in `fields` (e.g. principal/driven-by, loop/stalled, memory/budget), values read now and "
                       "rendered (references as names). Sent as a view Message on `route` to `to` (defaults: the Message "
                       "you are answering). Use it instead of enumerating or typing a table yourself; say in your reply "
                       "what it shows.")}
   {:name "view.graph-of"
    :hint "an Object's reference graph"
    :handler :view.graph-of
    :schema [:map
             [:root id-string]
             [:depth {:optional true} [:enum 1 2]]
             [:title {:optional true} :string]
             [:to {:optional true} [:vector id-string]]
             [:route {:optional true} id-string]]
    :instructions (str "Show people the graph around an Object: nodes are the Object and everything its ref Fields name "
                       "(models, adapters, routes, grants ...), edges labelled by the Field, depth 1 (default) or 2, "
                       "only what you can read, at most 200 nodes. Sent as a view Message on `route` to `to` (defaults: "
                       "the Message you are answering).")}
   {:name "task.run"
    :hint "run a command through a shell Adapter as a Task"
    :handler :task.run
    :schema [:map
             [:adapter :string]
             [:cmd :string]
             [:cwd {:optional true} :string]
             [:timeout-s {:optional true} :int]]
    :instructions (str "Run a command through a shell Adapter (`adapter` is its id or bound name, e.g. shell.local "
                       "or atlas.shell) as a Task you own: task/exit, task/stdout, task/stderr land on it and the "
                       "result carries the first 2 KiB. Default timeout 120 s, cwd the Adapter's directory.")}
   ;; the Model catalogue, cost accounting, search
   {:name "gateway.create"
    :hint "join a message service to this world; a person's own session"
    :handler :gateway.create
    :schema [:map [:name :string] [:kind [:enum "bluebubbles"]] [:url :string]
             [:key-env {:optional true} :string] [:poll-s {:optional true} :int]]
    :instructions (str "A message service as an Adapter, read from now on, so no history comes in. `key-env` names the "
                       "environment variable holding its password (err secret-in-args for a literal one). Nobody can "
                       "reach anyone through it until gateway.bind.")}
   {:name "gateway.bind"
    :hint "let one handle reach one agent; a person's own session"
    :handler :gateway.bind
    :schema [:map [:gateway :string] [:handle :string] [:agent :string]]
    :instructions (str "Texts from `handle` reach `agent` as Messages from the gateway. They wake it and carry no authority: "
                       "a text cannot approve, arm or grant. Anyone not bound is dropped, unread.")}
   {:name "gateway.unbind"
    :hint "stop a handle reaching anyone; a person's own session"
    :handler :gateway.unbind
    :schema [:map [:gateway :string] [:handle :string]]
    :instructions "Texts from `handle` are dropped again."}
   {:name "gateway.enable"
    :hint "turn a gateway's reading and sending on or off; a person's own session"
    :handler :gateway.enable
    :schema [:map [:gateway :string] [:on :boolean]]
    :instructions "Off, it reads nothing and sends nothing, and keeps its bindings."}
   {:name "gateway.list"
    :hint "the message gateways you can see, their status and bindings"
    :handler :gateway.list
    :public true
    :schema [:map]
    :instructions "{gateways [{gateway name kind status enabled dropped bindings}]}. `dropped` counts texts from nobody bound."}
   {:name "gateway.send"
    :hint "text a bound person back; goes to a person's approval first"
    :handler :gateway.send
    :gated true
    :schema [:map [:text :string] [:gateway {:optional true} :string] [:handle {:optional true} :string]]
    :instructions (str "To the person a gateway Message came from, with no other args; or `gateway` and `handle`. "
                       "Only the agent a handle is bound to may text it.")}
   {:name "speech.transcribe"
    :hint "turn a recording into text, through a Model with model/stt; a person's own session"
    :handler :speech.transcribe
    :schema [:map [:audio :string] [:mime {:optional true} :string] [:model {:optional true} :string]]
    :instructions (str "`audio` is base64, two minutes at most. Answers {text}. The sound goes to the Model and back; nothing "
                       "is recorded, and the text joins the record only if a person sends it.")}
   {:name "speech.say"
    :hint "turn text into speech, through a Model with model/tts; a person's own session"
    :handler :speech.say
    :schema [:map [:text :string] [:model {:optional true} :string]]
    :instructions "Answers {audio mime}, base64 WAV, to the session that asked. Nothing is recorded."}
   {:name "model.choose"
    :hint "which Model an agent or a conversation runs on, and its fallback"
    :handler :model.choose
    :schema [:map [:principal :string] [:model {:optional true} :string] [:id {:optional true} :string]
             [:thread {:optional true} :string] [:fallback {:optional true} :boolean]
             [:clear {:optional true} :boolean]]
    :instructions (str "Person only. `model` is a Model, `id` one of its model/available. With `thread` it holds for "
                       "that conversation, else for the Principal. `fallback` true sets the Model used when the chosen "
                       "one is down. `clear` removes the choice.")}
   {:name "model.chosen"
    :hint "what a Principal or a conversation runs on now"
    :handler :model.chosen
    :schema [:map [:principal :string] [:thread {:optional true} :string]]
    :instructions "Which Model and model id a wake would use, where that choice comes from, and any fallback in force."}
   {:name "model.signin.begin"
    :hint "start Sign in with ChatGPT from a person's client"
    :handler :model.signin.begin
    :schema [:map [:port {:optional true} [:int {:min 1024 :max 65535}]] [:model {:optional true} :string]]
    :instructions (str "Person only, from a client that can open a browser. `port` is the client's loopback listener. "
                       "Answers {attempt, url}; the client opens url, and hands the callback to model.signin.finish.")}
   {:name "model.signin.finish"
    :hint "finish Sign in with ChatGPT with what the callback brought"
    :handler :model.signin.finish
    :schema [:map [:attempt :string] [:state {:optional true} :string] [:code {:optional true} :string]
             [:client-id {:optional true} :string] [:error {:optional true} :string]]
    :instructions (str "Person only. The world checks state, exchanges the code, validates the ID token and the plan "
                       "scope, keeps the tokens where no shell reaches, and makes or renews the Model. Answers "
                       "{model, name, models, first}.")}
   {:name "model.signout"
    :hint "end a Model's ChatGPT session and clear its tokens"
    :handler :model.signout
    :schema [:map [:model :string]]
    :instructions "Person only. Ends the renewable session at OpenAI and clears the tokens. Answers {revoked}."}
   {:name "model.create"
    :hint "a new Model: url, model id, the env var holding its key, prices"
    :handler :model.create
    :schema [:map
             [:name :string]
             [:url :string]
             [:model-id :string]
             [:key-env {:optional true} :string]
             [:reasoning {:optional true} [:enum "off" "on" "auto"]]
             [:price-in {:optional true} [:or :int :double]]
             [:price-out {:optional true} [:or :int :double]]
             [:context-tokens {:optional true} :int]
             [:label {:optional true} :string]
             [:stt {:optional true} :string]
             [:tts {:optional true} :string]
             [:voice {:optional true} :string]
             [:images {:optional true} :boolean]]
    :instructions (str "Add a Model to the catalogue: an OpenAI-compatible endpoint at `url` (ending in /v1) "
                       "serving `model`, bound as `name`. The key is never an argument and never a Field: `key-env` "
                       "names the environment variable the world reads it from at call time (err secret-in-args for a "
                       "literal credential). `price-in`/`price-out` are per million tokens (0 by default), "
                       "`context-tokens` the window. The Model is probed at once (model/status, model/available). "
                       "A Principal uses it when its principal/models names it and it holds evaluate on it. "
                       "`stt` and `tts` name what the endpoint serves for speech, `voice` the voice.")}
   {:name "model.probe"
    :hint "probe a Model's endpoint: status and model list"
    :handler :model.probe
    :schema [:map [:model :string]]
    :instructions (str "GET <url>/models on a Model (`model`: id or bound name) and record model/status (ok, "
                       "unreachable, unauthenticated), model/available and model/probed-at. The scheduler does this "
                       "every five minutes; call it when a Model seems down.")}
   {:name "cost.report"
    :hint "a table of what evaluations cost, by principal, model or day"
    :handler :cost.report
    :schema [:map
             [:by {:optional true} [:enum "principal" "model" "day"]]
             [:since {:optional true} :string]
             [:to {:optional true} [:vector id-string]]
             [:route {:optional true} id-string]]
    :instructions (str "Show people what evaluations cost: one row per principal (default), model or day, with the "
                       "count, tokens in, tokens out and the money at each Model's prices, over the Evaluations you "
                       "may read since `since` (an ISO instant) or all time. Sent as a table view on `route` to `to` "
                       "(defaults: the Message you are answering). Every Principal, Model and the world also carry "
                       "running Fields cost/evaluations, cost/tokens-in, cost/tokens-out, cost/total, cost/since.")}
   {:name "code.search"
    :hint "find lines in your mounted files"
    :handler :code.search
    :public true
    :schema [:map [:pattern :string] [:regex {:optional true} :boolean] [:path {:optional true} :string] [:glob {:optional true} :string] [:limit {:optional true} :int]]
    :instructions (str "Lines matching `pattern` under your mounts, best first: source before documents, a file's own name "
                       "before its contents. `pattern` is literal unless `regex` is true. A phrase matches however it is spelled (boot sector, BootSector, boot_sector); "
                       "when no line has it whole, lines with any of its words stand in and `matched` says so. `path` narrows to a "
                       "directory, `glob` to names such as *.cpp. `next` is the read that follows from the first hit.")}
   {:name "code.outline"
    :hint "the shape of a mounted file or directory"
    :handler :code.outline
    :public true
    :schema [:map [:name {:optional true} :string]]
    :instructions (str "A file's headings, classes and functions with line numbers, or a directory's folders and file counts. "
                       "No `name` is every mount. Use it before reading a large file.")}
   {:name "file.edit"
    :hint "change one exact piece of a mounted file"
    :handler :file.edit
    :public true
    :schema [:map [:name :string] [:new :string] [:old {:optional true} :string] [:all {:optional true} :boolean]
             [:at {:optional true} [:enum "top" "end"]]]
    :instructions (str "Replace `old`, exact and found once, with `new` in the file `name` (`<mount>/<path>`), and answer the diff. "
                       "No match or several answer an error and change nothing; `all` true replaces every match. With no `old`, `at` "
                       "top or end inserts `new` as whole lines. With only `new`, a path that does not exist is created. The gate is fs:write.")}
   {:name "search.find"
    :hint "find Objects and Fields by words"
    :handler :search.find
    :schema [:map
             [:text :string]
             [:limit {:optional true} :int]
             [:kinds {:optional true} [:vector [:enum "field" "message" "object"]]]
             [:since {:optional true} :int]]
    :instructions (str "Find things by words: substring and word match over Field values, Message texts and bound "
                       "names, among what you may read, newest first. Each hit names the Object (and Field) with a "
                       "snippet and its seq; read the Object for the rest. Use it before saying something is not "
                       "on record. `kinds` narrows to field, message or object; `since` is a seq.")}
   ;; credentials as Objects, pairing, other worlds
   {:name "token.issue"
    :hint "a new credential for a Principal: label, optional ttl"
    :handler :token.issue
    :schema [:map [:principal :string] [:label :string] [:ttl-s {:optional true} :int]]
    :instructions (str "Mint a credential for `principal` (id or name) labelled for the device or purpose that will use "
                       "it, expiring after `ttl-s` seconds when given. You need delegate or write on that Principal. A "
                       "person at a listener receives the secret in the result; when you call this from your loop the "
                       "secret is written to a 0600 file under data/tokens/ and you get its path, never the secret.")}
   {:name "token.revoke"
    :hint "revoke a credential (a token Object)"
    :handler :token.revoke
    :schema [:map [:token id-string] [:reason {:optional true} :string]]
    :instructions (str "Revoke the token Object `token` with a reason; its secret stops authenticating at once. You "
                       "need delegate or write on the Principal it belongs to.")}
   {:name "token.list"
    :hint "the credentials of a Principal, or of every Principal you read, as a table"
    :handler :token.list
    :schema [:map [:principal {:optional true} :string] [:to {:optional true} [:vector id-string]] [:route {:optional true} id-string]]
    :instructions (str "List credentials: label, principal, status (active, expired, revoked), issued, expires, last used, "
                       "revoked, token id, for `principal` or for every Principal you may read. Sent as a table view "
                       "when there is someone to send it to; the rows are in the result too.")}
   {:name "pair.begin"
    :hint "a six-digit code a phone trades for a credential (session only)"
    :handler :pair.begin
    :schema [:map [:principal :string] [:label :string] [:ttl-s {:optional true} :int]]
    :instructions (str "Start pairing a device: a six-digit code valid for `ttl-s` seconds (300) that, sent as "
                       "`pair.claim {pairing-code label}` on a fresh socket before attach, yields a credential of `principal`. "
                       "Only a person at a listener may begin a pairing (err session-only from a loop).")}
   {:name "world.link"
    :hint "another world as an Adapter plus a bridge Principal that relays"
    :handler :world.link
    :schema [:map [:name :string] [:url :string] [:key-env {:optional true} :string] [:token-file {:optional true} :string]]
    :instructions (str "Reach another world: an Adapter of kind world at `url` (its ws endpoint) whose credential is the "
                       "environment variable `key-env` names, or the file `token-file` (never an argument, never a "
                       "Field), bound `world.<name>`; and a Principal `bridge.<name>` on a Route with you. A Message to the "
                       "bridge is forwarded to root there and the answer comes back on that Route; `world.send` does "
                       "the same for one line.")}
   {:name "world.send"
    :hint "send a line to a Principal on another world; the answer comes back as a Message"
    :handler :world.send
    :schema [:map [:world :string] [:to {:optional true} :string] [:route {:optional true} :string] [:text :string] [:timeout-s {:optional true} :int]]
    :instructions (str "Send `text` to `to` (a name there, root by default) on the world `world` (the Adapter or bridge "
                       "name) and get the reply as a Message from `bridge.<name>` on your Route with it, in-reply-to "
                       "what you were answering; `timeout-s` (120) bounds the wait. Nothing is retried.")}])

(defn required-args
  "The required arg keys of a shipped action (its Malli map form without the {:optional true}
   entries), or nil for an unknown name. the repeat-of rule compares these only, so
   an optional arg added on a retry (in-reply-to, title) no longer defeats it."
  [action-name]
  (when-let [d (some #(when (= (:name %) (str action-name)) %) definitions)]
    (set (for [entry (rest (:schema d))
               :when (vector? entry)
               :let [[k props] entry]
               :when (not (and (map? props) (:optional props)))]
           k))))

(defn bound-name [n] (str "action/" n))

(defn- plain
  "Args as the JSON form expects them: keywords to strings, uuids to strings, sets to vectors."
  [v]
  (walk/postwalk (fn [x] (cond (keyword? x) (subs (str x) 1)
                               (uuid? x) (str x)
                               (set? x) (vec (sort-by str x))
                               :else x))
                 v))

(defn- keywordize
  "Map keys as keywords (the plain pass turned them into strings)."
  [v]
  (walk/postwalk (fn [x] (if (map? x) (into {} (map (fn [[k v]] [(if (string? k) (keyword k) k) v])) x) x)) v))

(defn normalize-args
  "Wire args (JSON or Transit form) -> the validated shape: keyword keys, string values."
  [args]
  (keywordize (plain (or args {}))))

;; ---------------------------------------------------------------- the registry in the world

(defn action-objects
  "name -> action Object id for every action bound in the world namespace."
  [db]
  (let [wid (objects/world-id db)]
    (into {} (for [[n id] (objects/bound-names db wid)
                   :when (str/starts-with? n "action/")]
               [(subs n (count "action/")) id]))))

(defn missing
  "The shipped definitions not yet bound in the world."
  [db]
  (let [have (action-objects db)]
    (remove #(contains? have (:name %)) definitions)))

(defn ensure!
  "Create and bind, as p (root at boot), every shipped action the world lacks. Idempotent.
   Returns the names created."
  [world p]
  (let [wid (:world-id world)]
    (vec (for [{:keys [name hint handler schema instructions gated public]} (missing (act/db world))]
           (let [{:keys [ok err]} (act/create! world p :object
                                               {:fields (cond-> {:action/name name
                                                                 :action/hint hint
                                                                 :action/schema (pr-str schema)
                                                                 :action/handler handler
                                                                 :action/gated (boolean gated)
                                                                 :instructions/own instructions}
                                                          public (assoc :action/public true))})]
             (when err (throw (ex-info (str "action " name ": " (:message err)) {:err err})))
             (let [{:keys [err]} (act/bind! world p {:namespace wid :name (bound-name name) :object (:object ok)})]
               (when err (throw (ex-info (str "action " name " bind: " (:message err)) {:err err}))))
             name)))))

(defn- field [db id k] (get-in (objects/fields db id) [k :field/value]))

(defn schema-of
  "The Malli form of an action Object's args."
  [db id]
  (some-> (field db id :action/schema) edn/read-string))

;; ---------------------------------------------------------------- rendering for the Acting section

(defn- type-text
  "A compact reading of a Malli form: string, int, bool, a|b, [x], {k v, ...}."
  [form]
  (cond
    (= form :string) "string"
    (= form :int) "int"
    (= form :boolean) "bool"
    (= form id-string) "id"
    (keyword? form) (name form)
    (and (vector? form) (= :enum (first form))) (str/join "|" (map str (rest form)))
    (and (vector? form) (= :vector (first form))) (str "[" (type-text (second form)) "]")
    (and (vector? form) (= :or (first form))) (str/join "|" (map type-text (rest form)))
    (and (vector? form) (= :re (first form))) "id"
    (and (vector? form) (= :map (first form)))
    (str "{" (str/join ", " (for [[k & more] (rest form)
                                  :let [opts (when (map? (first more)) (first more))
                                        s (if opts (second more) (first more))]]
                              (str (name k) (when (:optional opts) "?") " " (type-text s))))
         "}")
    :else (pr-str form)))

(defn available
  "The actions p may call: [{:name :hint :schema :instructions}] for every action Object p resolves for :evaluate."
  [db p]
  (vec (for [[n id] (sort-by key (action-objects db))
             :when (resolve/resolves? db p (bound-name n) :evaluate)]
         {:name n :hint (field db id :action/hint) :schema (schema-of db id) :instructions (field db id :instructions/own)})))

(defn- adapters-line
  "The Principal's own Adapters by bound name and kind, so task.run and read have the right names."
  [db p]
  (let [wid (objects/world-id db)
        as (for [a (get-in (objects/fields db p) [:principal/adapters :field/value])
                 :let [names (objects/names db a)
                       n (or (some (fn [[ns nn]] (when (= ns wid) nn)) (sort-by second names))
                             (some (fn [[_ nn]] nn) (sort-by second names)))
                       binds (field db a :adapter/binds)]
                 :when n]
             (str n " (" (case binds :host "shell" :filesystem "mount" (some-> binds name)) ")"
                  (when-let [t (field db a :adapter/test)] (str ", tests: `" t "`"))))]
    (when (seq as) (str "Your adapters: " (str/join ", " as) "."))))

(def acting-block-chars
  "What the Actions block may cost. It names actions and says what each is for; the shape of an
   action's args is one read away, so a long catalogue does not eat the wake budget."
  2300)

(defn shape-text
  "An action/schema as stored (EDN text) in the words the prompt uses: {name type, opt? type}."
  [edn-text]
  (try (type-text (edn/read-string edn-text)) (catch Exception _ edn-text)))

(def ^:private as-tools
  "Actions a model holds as tools of their own and the Acting text names, so a line each here would
   only push the block past its bound."
  #{"code.search" "code.outline" "file.edit" "projects" "project.add" "delegate" "task.progress" "task.ask"
    "task.run" "gateway.send"})

(defn acting-text
  "The # Actions block for the Acting section, or nil when p can call none: one line per action with
   its purpose, and the Principal's own Adapters by name. An action's args and its paragraph are on
   its Object (action/schema, instructions/own), which `read {name action/<name>}` returns. Bounded
   by acting-block-chars: past it the rest are listed by name."
  [db p]
  (when-let [as (seq (remove #(or (people-only (:name %)) (as-tools (:name %))) (available db p)))]
    (let [head (str "# Actions\n"
                    "Call one: `{\"op\":\"act\",\"args\":{\"action\":\"<name>\",\"args\":{...}}}`. Before first use, "
                    "`{\"op\":\"read\",\"args\":{\"name\":\"action/<name>\"}}` gives its args.\n")
          known (projects/line db p (some #(= "project.add" (:name %)) (available db p)))
          tail (str (when-let [l (adapters-line db p)] (str "\n" l)) (when known (str "\n" known))
                    (when-let [l (projects/source-line db p)] (str "\n" l)))
          line (fn [{:keys [name hint]}] (str "- " name (when hint (str ": " hint))))
          room (- acting-block-chars (count head) (count tail))
          ;; as many purposes as fit, then the rest by name: the block never loses them all
          names-cost (fn [xs] (reduce + 7 (map #(+ 2 (count (:name %))) xs)))
          [told rest-of] (loop [left room todo as out []]
                           (let [l (some-> (first todo) line)]
                             (if (and l (< (+ (inc (count l)) (names-cost (next todo))) left))
                               (recur (- left (inc (count l))) (next todo) (conj out l))
                               [out todo])))]
      (str head (str/join "\n" told)
           (when (seq rest-of) (str (when (seq told) "\n") "Also: " (str/join ", " (map :name rest-of))))
           tail))))

;; ---------------------------------------------------------------- dispatch

(defmulti handle!
  "An action's handler: (handle! handler world p args opts) -> {:ok {:result ... :object? :seq?}} or
   {:err}. opts is {:via <evaluation or nil> :session? <true for a socket session, false for a loop>}.
   The defmethods live in knowledge.world.act.handlers."
  (fn [handler _world _p _args _opts] handler))

(defmethod handle! :default [handler _ _ _ _]
  (act/err :not-implemented (str "action handler " (some-> handler name) " is not built yet")))

(defn validate-args
  "err invalid-args when args do not fit the named action's schema, else nil (run
   before an op is gated, so a person never approves an op that cannot run)."
  [db n args]
  (when-let [object (get (action-objects db) n)]
    (let [schema (schema-of db object) args (normalize-args args)]
      (when (and schema (not (m/validate schema args)))
        (act/err :invalid-args (str n " args: " (pr-str (me/humanize (m/explain schema args)))))))))

(defmulti preflight!
  "Cheap checks an action can make before it is gated: {:err ...} or nil. The
   default is none; principal.create refuses :delegate and a Grant not strictly narrower here."
  (fn [n _world _p _args] (keyword (str n))))
(defmethod preflight! :default [_ _ _ _] nil)

(defn preflight
  "Schema, then the action's own checks, for `act {action args}` as p; nil when fine."
  [world db p args]
  (let [n (let [x (:action args)] (if (keyword? x) (name x) (str x)))
        inner (normalize-args (if (map? (:args args)) (:args args) {}))]
    (or (validate-args db n inner)
        (preflight! n world p inner))))

(defn rule-line?
  "Would a person's standing rule have admitted this text as a whole command line on this terminal?"
  [db p pane text]
  (let [args {:action "pane.send" :args {:pane (str pane) :text text}}]
    (boolean ((requiring-resolve 'knowledge.world.act.rules/admit) db p "shell" :act args))))

(defn perform-action!
  "`act {action args}`: resolve `action/<name>` for :evaluate as p, validate, dispatch.
   opts {:via :session?} as for handle!."
  [world p action args opts]
  (let [db (act/db world)
        n (if (keyword? action) (name action) (str action))
        objects (action-objects db)]
    (cond
      (not (contains? objects n))
      (act/err :unknown-action (str "no action named " n "; known: " (str/join ", " (sort (keys objects)))))

      :else
      (if-let [{:keys [object]} (resolve/resolve! world db p (bound-name n) :evaluate)]
        (let [schema (schema-of db object)
              args (normalize-args args)]
          (if (and schema (not (m/validate schema args)))
            (act/err :invalid-args (str n " args: " (pr-str (me/humanize (m/explain schema args)))))
            (try (handle! (field db object :action/handler) world p args (or opts {}))
                 (catch clojure.lang.ExceptionInfo e
                   (if-let [err (:err (ex-data e))] {:err err} (act/err :invalid (ex-message e)))))))
        act/unresolved))))
