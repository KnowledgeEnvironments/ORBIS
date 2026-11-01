# Ground rules for development agents

Binding for every agent and person writing the kernel, the listener, or an adapter. `checklist.md` lists what a change must pass before it counts as done.

These bind every agent and person writing the kernel, the listener, or an adapter. They exist because a demo built from special cases proves nothing, and the spec's claim is that the primitives compose.

1. **No special cases for scenarios.** A requested demo, example, or test scenario is built from the primitives in sections 4 and 5. If a scenario cannot be built from them, the primitives are wrong. Write the gap in the decisions log and stop. Never add a branch that recognizes the scenario.
2. **Compose, never bypass.** Every feature is an arrangement of Object, Principal, Field, Event, and Serialization. A new class is a decisions-log entry with the invariant it needs that the five cannot express.
3. **The log is the only state.** No cache, index, or client store holds a fact that is not in the log. Anything derived can be deleted and rebuilt. A test that depends on derived state is wrong.
4. **One access path.** Every read, write, send, and evaluate goes through resolution. No admin path, no debug path, no test path around it. Tests create Principals and Grants like everyone else.
5. **Names from the ontology.** Code, schemas, protocol ops, and UI labels use the class and field names in this document. A better name is a decisions-log entry, then a rename everywhere.
6. **No invented facts in the record.** Fixtures, seeds, and mocks are Objects whose creation is an event by a named test Principal. Nothing appears in the log without an actor and a delegation.
7. **Evaluations are honest.** A Model stub records a Serialization and an outcome like a real Model. A test that skips serialization tests nothing.
8. **Protocol changes after M0 go through the decisions log** before code. The listener and the world never ship a private op.
9. **Dogfood from M3.** Once kernel-zero passes, development agents work as Principals in the world: their Tasks are Tasks, their runs are Evaluations, their notes are authored Fields. Their instruction files become Fields on the Objects they describe.
10. **A gap is a finding.** When the spec and the code disagree, the agent writes the disagreement to the decisions log as an open question with both readings, and builds the reading that keeps the invariants. Silent divergence is the one forbidden move.
