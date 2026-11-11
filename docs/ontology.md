#'user/entries
#'user/tname
# Ontology

Generated from `schema/src/knowledge/schema/*.clj` on 2026-10-29. Spec sections 3, 4, 5. Do not edit by hand.

## Classes

Fourteen classes. Kernel: event, field, object, principal, serialization . Arrangements: adapter, model, evaluation, grant, group, message, route, task, trigger .

## Field value types

` string number boolean instant keyword bytes ref list map `; parameterised `[:ref <class>]`, `[:list <type>]`, `[:map <type>]`.

### adapter

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| principal/driven-by | `[:enum :person :process :none]` |  |
| principal/namespace | `:uuid` |  |
| principal/grants | `[:set :uuid]` |  |
| principal/workspaces | `[:set :uuid]` | yes |
| principal/models | `[:set :uuid]` | yes |
| principal/routes | `[:set :uuid]` | yes |
| principal/budget | `:uuid` | yes |
| principal/public-key | `:string` | yes |
| model/kind | `[:enum :model :compiler :runner :shell :emulator :storage...` |  |
| model/format | `:keyword` |  |
| model/residency | `[:enum :local :tenant :external]` |  |
| model/capacity | `[:map-of :keyword :any]` |  |
| model/pricing | `[:map-of :keyword :double]` |  |
| model/adapter | `:uuid` | yes |
| adapter/binds | `[:enum :api :cli :filesystem :container :task-system :dat...` |  |
| adapter/locator | `:string` |  |
| adapter/objects | `[:set :uuid]` |  |
| adapter/sync | `[:enum :pull :push :both]` |  |
| adapter/last-seen | `[:map-of :uuid #object[clojure.core$inst_QMARK_ 0x58164e9...` |  |

### model

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| principal/driven-by | `[:enum :person :process :none]` |  |
| principal/namespace | `:uuid` |  |
| principal/grants | `[:set :uuid]` |  |
| principal/workspaces | `[:set :uuid]` | yes |
| principal/models | `[:set :uuid]` | yes |
| principal/routes | `[:set :uuid]` | yes |
| principal/budget | `:uuid` | yes |
| principal/public-key | `:string` | yes |
| model/kind | `[:enum :model :compiler :runner :shell :emulator :storage...` |  |
| model/format | `:keyword` |  |
| model/residency | `[:enum :local :tenant :external]` |  |
| model/capacity | `[:map-of :keyword :any]` |  |
| model/pricing | `[:map-of :keyword :double]` |  |
| model/adapter | `:uuid` | yes |

### evaluation

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| evaluation/initiator | `:uuid` |  |
| evaluation/model | `:uuid` |  |
| evaluation/delegation | `:uuid` |  |
| evaluation/serialization | `:uuid` |  |
| evaluation/policy | `[:enum :normal :one-shot]` |  |
| evaluation/outcome | `:map` | yes |
| evaluation/produced | `[:set :uuid]` |  |
| evaluation/cost | `[:map-of :keyword :double]` |  |
| evaluation/state | `[:enum :queued :running :done :failed :gated]` |  |

### field

| Field | Schema | Optional |
| --- | --- | --- |
| field/name | `:keyword` |  |
| field/type | `[:or [:enum :string :number :boolean :instant :keyword :b...` |  |
| field/reader | `[:enum :human :agent :both]` |  |
| field/value | `[:or :string :double :int :boolean #object[clojure.core$i...` |  |
| field/author | `:uuid` |  |
| field/set-by | `:uuid` | yes |
| field/set-at | `#object[clojure.core$inst_QMARK_ 0x58164e9a "clojure.core...` |  |
| field/visibility | `:uuid` | yes |
| field/mode | `[:enum :inline :blob :external]` | yes |

### grant

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| grant/grantor | `:uuid` |  |
| grant/grantee | `:uuid` |  |
| grant/scope | `[:or [:set :uuid] [:map [:prefix [:string {:min 1, :max 5...` |  |
| grant/ops | `[:set [:enum :read :reference :transmit :write :delegate ...` |  |
| grant/delegable | `:boolean` |  |
| grant/until | `[:or #object[clojure.core$inst_QMARK_ 0x58164e9a "clojure...` | yes |

### group

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| principal/driven-by | `[:enum :person :process :none]` |  |
| principal/namespace | `:uuid` |  |
| principal/grants | `[:set :uuid]` |  |
| principal/workspaces | `[:set :uuid]` | yes |
| principal/models | `[:set :uuid]` | yes |
| principal/routes | `[:set :uuid]` | yes |
| principal/budget | `:uuid` | yes |
| principal/public-key | `:string` | yes |
| group/members | `[:set :uuid]` |  |
| group/policy | `[:set :uuid]` |  |
| group/kind | `[:enum :team :channel :project :federation :custom]` |  |

### message

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| message/from | `:uuid` |  |
| message/to | `[:set :uuid]` |  |
| message/route | `:uuid` |  |
| message/text | `:string` |  |
| message/tags | `[:vector :uuid]` |  |
| message/in-reply-to | `:uuid` | yes |
| message/seq | `[:int {:min 0}]` |  |

### object

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |

### principal

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| principal/driven-by | `[:enum :person :process :none]` |  |
| principal/namespace | `:uuid` |  |
| principal/grants | `[:set :uuid]` |  |
| principal/workspaces | `[:set :uuid]` | yes |
| principal/models | `[:set :uuid]` | yes |
| principal/routes | `[:set :uuid]` | yes |
| principal/budget | `:uuid` | yes |
| principal/public-key | `:string` | yes |

### route

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| route/transport | `:uuid` |  |
| route/address | `:string` |  |
| route/input-tags | `[:vector :uuid]` |  |
| route/output-format | `:uuid` | yes |
| route/hard-rules | `[:vector :uuid]` |  |

### serialization

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| serialization/working-set | `[:or [:set :uuid] [:map [:query :any]]]` |  |
| serialization/model | `:uuid` |  |
| serialization/depth | `[:map-of :uuid :int]` |  |
| serialization/format | `:keyword` |  |
| serialization/bytes | `[:or [:map [:blob/sha256 [:string {:min 64, :max 64}]] [:...` |  |
| serialization/size | `[:map [:bytes :int] [:tokens {:optional true} :int]]` |  |
| serialization/hash | `:string` |  |
| serialization/as-of | `[:map-of :uuid [:int {:min 0}]]` |  |
| serialization/layout | `:map` | yes |
| serialization/live | `:boolean` | yes |

### task

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| task/owner | `:uuid` |  |
| task/state | `[:enum :open :active :blocked :done :cancelled]` |  |
| task/blocked-on | `[:set :uuid]` |  |
| task/due | `#object[clojure.core$inst_QMARK_ 0x58164e9a "clojure.core...` | yes |
| task/evidence | `[:set :uuid]` |  |

### trigger

| Field | Schema | Optional |
| --- | --- | --- |
| object/id | `:uuid` |  |
| object/class | `[:enum :object :principal :field :event :serialization :g...` |  |
| object/names | `[:set [:tuple :uuid [:string {:min 1, :max 512}]]]` |  |
| object/stream | `:uuid` |  |
| object/fields | `[:map-of :keyword [:map {:title "Field"} [:field/name :ke...` |  |
| object/created | `:uuid` | yes |
| object/backing | `:uuid` | yes |
| trigger/owner | `:uuid` |  |
| trigger/as | `:uuid` |  |
| trigger/delegation | `:uuid` |  |
| trigger/predicate | `[:multi {:dispatch :kind} [:schedule [:map [:kind [:= :sc...` |  |
| trigger/evaluation | `:map` |  |
| trigger/once | `:boolean` | yes |
| trigger/state | `[:enum :armed :fired :disarmed]` |  |

## Event kinds

| Kind | Body |
| --- | --- |
| created | `[:map [:class [:enum :object :principal :field :event :se...` |
| bound | `[:map [:namespace :uuid] [:name [:string {:min 1, :max 51...` |
| unbound | `[:map [:namespace :uuid] [:name [:string {:min 1, :max 51...` |
| field-set | `[:map [:name :keyword] [:type [:or [:enum :string :number...` |
| field-read | `[:map [:name :keyword] [:by :uuid] [:via {:optional true}...` |
| resolved | `[:map [:name [:string {:min 1, :max 512}]] [:object :uuid...` |
| granted | `[:map [:grant :uuid] [:grantee :uuid] [:ops [:set [:enum ...` |
| revoked | `[:map [:grant :uuid] [:reason {:optional true} :string]]` |
| sent | `[:map [:message :uuid] [:route :uuid]]` |
| delivered | `[:map [:message :uuid] [:route :uuid] [:recipient :uuid]]` |
| task-moved | `[:map [:task :uuid] [:from [:enum :open :active :blocked ...` |
| evaluated | `[:map [:evaluation :uuid] [:state [:enum :queued :running...` |
| fired | `[:map [:trigger :uuid] [:evaluation :uuid]]` |
| attached | `[:map [:session :string] [:client-kind [:enum :listener :...` |
| detached | `[:map [:session :string] [:reason {:optional true} :string]]` |
| retired | `[:map [:reason {:optional true} :string]]` |

## Ops

 attach, detach, resolve, read, history, subscribe, unsubscribe, search, present, bind, unbind, set-field, send, move, evaluate, cancel, release, fail, grant, revoke, arm, disarm, serialize, act
