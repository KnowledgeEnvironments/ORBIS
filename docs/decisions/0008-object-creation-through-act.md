# ADR 0008: Object creation through act

Status: accepted, 2026-10-29

## Context
The op list has no create op. The design pass proposed overloading present, which contradicts spec 14 (present renders a view).
## Decision
Creation is an action named create on a class schema, invoked as {op act args {class <k> action create args {...}}}. It emits a created event. In sprint 0 it exists for Object, Principal, Group.
## Consequences
The command table shows create for classes the principal may create; no second creation path.
