# ADR 0003: Global seq over the world log

Status: accepted, 2026-10-29

## Context
Spec 4.4 gave each stream its own dense seq and ordered across streams by wall clock. Resume needs a gap check.
## Decision
seq is assigned by the single writer as log position: one total order per world. stream is a partition key; per-stream order follows from global order.
## Consequences
No gap is a contiguous range check. Cross-world order (federation) still needs a hybrid logical clock; deferred.
