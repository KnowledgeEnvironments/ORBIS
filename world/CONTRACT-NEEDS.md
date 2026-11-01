# Contract needs and findings from the world track (sprint 0)

Ground rule 10: a gap is a finding. None of these changed `schema/` or `docs/`; the world took
the least-deviating reading in each case.

1. **`granted` body lacks `delegable` and `until`.** `knowledge.schema.events/bodies :granted` is
   `{grant grantee ops scope}`; the fold needs delegability and expiry to apply spec 5.9 and 6.1.
   The world adds `:delegable` and `:until` as extra keys (the map is open) and emits a `created`
   event for the Grant Object (class `:grant`) before `granted`, so a Grant is an Object with a
   stream as 5.9 requires. Proposal: add `[:delegable :boolean] [:until {:optional true} ...]` to
   the `granted` body.

2. **Schema Object names collide.** `knowledge.schema.objects/schema-objects` names both
   `:ke.args/attach` and `:ke.result/attach` as `schema.attach`. The world binds
   `schema.<name>` for `ke/*`, `schema.args.<name>` and `schema.result.<name>` otherwise, and
   stores that bound name in `schema/name` plus the registry key in `schema/key`.

3. **`schema/form` is not a `Value`.** `m/form` contains symbols (`inst?`, `bytes?`) and, for
   `:ke/Frame`, a function (`:dispatch frame-kind`), neither of which `knowledge.schema.core/Value`
   admits or Transit encodes. The world stores `schema/form` as EDN text (type `:string`) with
   functions printed as their class name.

4. **Pushes and `resolved` events.** Spec 6.1 step 5 makes every resolution an event. Applying
   that to the subscription filter would append one `resolved` event per pushed event on the
   principal's own stream, which the same subscription then pushes, without end. The world appends
   `resolved` for every op-level resolution (read, history, set-field, bind, unbind, revoke, act)
   and filters pushes with the pure predicate. Needs a decisions-log row.

5. **Response `result` is `:any` in the JSON form.** `:ke/Response` types `result` as `:any`, so a
   JSON client gets ids inside results back as strings although `:ke.result/<op>` schemas exist.
   The world's test client decodes `result` by the op it sent (`codec/decode-result`); the
   listener should do the same, or `Response` could become a `:multi` on the request's op.

6. **Credential visibility.** Sprint 0 tokens are stored as the Field `principal/token-hash`
   (sha256) with `visibility` = the Grant under which the creator acted (the root Grant when root
   creates). "root-only" is therefore "whoever holds the creating delegation", which is root in
   sprint 0 and the natural generalisation later.

7. **Op names in the kernel-zero brief.** The brief's "resolves? ... nil for :grant" uses an op
   that is not in `knowledge.schema.core/Op`; the test checks `:delegate`.

8. **`bind` and the object.** The brief says bind is op `:write` on the namespace holder; the
   world also requires the object to exist but does not require `:reference` on it. If binding
   should count as a reference, that is one more resolution per bind.

9. **Kaocha var order.** `:kaocha.plugin.randomize/randomize? false` on the kernel-zero suite
   stops shuffling, but kaocha still loads vars in its own order, not definition order; the six
   step tests share one world through a `:once` fixture and do not depend on order. The
   unnamespaced `:randomize? false` that was in tests.edn is ignored by kaocha and was replaced.

## Sprint 1 (serializer, send, evaluate, the loop)

10. **Resolved by schema:** `granted` now carries optional `delegable` and `until`; `evaluated`
    carries optional `serialization` and `outcome` (additive, exported to `listener/generated/schema`).

11. **A recipient does not resolve the Message delivered to it.** Delivery is an event on the
    recipient's stream (9.1) but resolution is Grants only (6.1), and a sender without a delegable
    Grant (any non-root Principal) cannot grant the recipient `:read` on the Message. The
    kernel-zero test therefore reads root's reply with root's client. Proposal: a Message's
    visibility Grant is the Route's, or `delivered` carries an implied `:read :reference` for the
    recipient on that Message. Same for a creator and the Objects it creates (`act create` gives
    the creator no Grant on the new Object).

12. **`send` is checked on the Route only.** `:transmit` on the Route (5.9) creates the Message
    without `:create` on `schema.Message`. Recipients must exist; Group fan-out at delivery (9.1)
    is not done yet.

13. **Model fields beyond the class schema.** A Model carries `model/base-url`,
    `model/api-mode`, `model/id` and `model/key-env` (spec 11.5 names `base_url` and
    `api_mode`; the key lives in the environment, never in the log). `knowledge.schema.classes/Model`
    could add them as optional. `route/output-format` is stored as a keyword (`:text`) naming the
    parser, where the schema says a schema ref; `route/template` is a reader `:agent` Field set
    right after `created`, since `created` fields fold as reader `:both`.

14. **Residency rule.** 8.3 says a Grant rule on `model.residency` is checked before bytes
    exist. There is no such rule yet: a Model with residency `:external` answers `err residency`;
    `:local` and `:tenant` need only `:evaluate` on the Model.

15. **Kernel-zero step 1 names.** The names `root.workspace`, `root.model`, `root.route` are
    bound in root's own namespace, on other Objects; the test now reads them from the `bound`
    events on root's stream (the earlier test expected them among root's own `object/names`).
    `root.route` and `transport.listener` are also bound in the world namespace so that another
    Principal can address root's inbox by name (the first person's `{:prefix "root"}` Grant
    depends on it).

16. **Step 3's Evaluation uses a Model the witness must resolve.** The test now resolves
    `root.model` and the witness holds `:evaluate` on it (the earlier test passed root's id as
    the model).

17. **Credential Fields in a context.** `principal/token-hash` has reader `:agent` with a
    visibility Grant; for root that Grant is the root Grant, so root's own context renders its
    token hash (a sha256, not the token). Proposal: a reader value no Model serializes, or
    visibility by a Grant held by nobody's loop.

18. **A loop needs `:write` on its own Principal** to keep `loop/*` Fields (ground rule 4). Root
    has it through the root Grant; a process Principal created by `act create` has no Grant on
    itself until given one. Subagent creation (9.8) should grant it.

19. **Self-delivery.** A delivery an Evaluation produced (`event/via` set) does not wake a loop in
    v1, so a Principal writing to itself, or two loops on one Route, cannot ping-pong. This also
    means a subagent's reply does not wake its parent yet.

20. **`message/seq`** (5.2, position on the Route's stream) is not stored as a Field: the `sent`
    event's seq is that position and lives on the Route's stream.

21. **The world stores no fact about the model's reply beyond the outcome.** `usage` from the
    endpoint lands in the `evaluated` done event's outcome; the model name the server reports
    (`your-model` for the requested `your-model`) is kept in
    `outcome.usage.model`.
