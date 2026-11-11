# Ground rules for development agents

Binding for every agent and person writing the kernel, the listener or an adapter (full text:
`harness/docs/ground-rules.md`). A demo built from special cases proves nothing; the claim is that the primitives compose.

1. No special cases for scenarios: build from the primitives; a scenario they cannot express is a gap for the decisions log.
2. Compose, never bypass: every feature is an arrangement of Object, Principal, Field, Event, Serialization.
3. The log is the only state: anything derived can be deleted and rebuilt.
4. One access path: every read, write, send and evaluate goes through resolution; no admin, debug or test path.
5. Names from the ontology, in code, schemas, ops and labels; a better name is a decisions entry, then a rename everywhere.
6. No invented facts in the record: fixtures and mocks are Objects created by a named Principal, with an actor and a delegation.
7. Evaluations are honest: a stub Model records a Serialization and an outcome like a real one.
8. Protocol changes go through the decisions log before code; no private ops.
9. Dogfood: development agents work as Principals; their Tasks, runs and notes are Tasks, Evaluations and authored Fields.
10. A gap is a finding: when spec and code disagree, write both readings to the decisions log and build the one that keeps the invariants.
