# Lessons

A design, 2026-11-10, for the half of self-improvement that does not exist: a wake that failed or
recovered leaves a lesson, root turns repeats into a change, and the bootstrap test says whether it
helped (the project's own notes, `the project's own notes`). Nothing here is built.

## What the bootstrap runs showed

On a passing run, 12 of root's 118 ops failed: seven `file.edit` no-match on whitespace, three reads of
`src/<path>` where the mount is `src.fs/<path>`, one `test` with no project, one `approval-pending`. The
run before had the same seven-and-three shape. Every one of them is in `evaluation/results` on the log,
and every one vanished when the wake ended. The only trace a wake leaves of itself today is
`memory/notes`, written by the model when its policy says so, and `loop/stalled`, one line when a wake
ends badly.

Three fixes of this kind already exist, each written by hand after a benchmark: `code/unknown-names`
(an edit naming a member the tree lacks), `offers-next-re` (a reply that ends on "I'll read next" is
challenged), `claims-success-re` (a reply claiming a pass with no run behind it). Each took a person
reading a log. The lesson is the kernel doing that reading.

## The rule

A lesson is a Field, `memory/lessons`, on the Principal whose wake earned it: a list of maps the
kernel writes and the model reads. The model does not write it; `memory/notes` stays its own. Root's
heartbeat reads lessons across the Principals it made, and acts on a repeat. The bootstrap test counts
failed ops per run and expects the count to fall once the same lesson has been acted on.

### When a wake ends, the kernel folds its failures

At `finish-turn!`, `stall!` and `end-undone!` in `loop/core.clj`, the wake's Evaluations are on the
log. The kernel groups the failed results by `(op, action, err code)`, and any group of two or more in
one wake, or one that ended the wake, becomes a lesson:

```
{:at <seq>            the wake's last Evaluation
 :message <id>        the wake
 :kind :edit/no-match | :read/unresolved | :op/refused | :wake/stalled | :wake/resumed
 :op "act" :action "file.edit" :code :no-match
 :count 7
 :example {:args <clipped> :err <message clipped>}
 :cost 7                 turns the group took
 :fix nil}               filled in when a lesson is acted on: {:by :at :what}
```

`count` and `example` are facts from the record; nothing is interpreted. A single failure the model
recovered from on the next turn is not a lesson: the threshold is a repeat, since one miss is how work
goes and two of the same is a habit. The kinds are a closed set the kernel knows how to fold; a new
kind is a code change, as a guard is today.

The list is capped at `memory/lessons-max` (20) and ordered newest first; a new lesson of the same
`(kind, action, code)` as an unfixed one bumps that one's count and `at` rather than adding, so a habit
reads as one line growing, not twenty.

### Every wake sees its own lessons

The serializer renders `memory/*` whole and first, so `memory/lessons` goes in the prompt as the notes
do: one line a lesson, `edit/no-match ×7 (last seq 3035): old text off by whitespace; the error names
the nearest line`. The last clause is the kernel's wording for that kind, fixed text per kind, since the
model reading its own record is what the note is for. A Principal with three lessons reads three
lines; the prompt cost is bounded by the cap.

This is the first effect and the cheapest: the model that missed seven times is told so, in its next
wake, before it edits again. The bootstrap test can measure it alone, with root doing nothing else.

### Root acts on a repeat

Root's heartbeat text gains a clause: `lessons of the Principals you created, and your own; a lesson
with count 3 or more that no fix names is yours to act on`. What acting is, in root's hands:

1. A rule for the Principal: a line in its `instructions/own` (its own working notes, which it
   reads every wake). For `read/unresolved` on `src/…`: "the mount is `src.fs`". This is the cheap fix
   and the common one.
2. A change to the harness page or the kernel: when the lesson is about every Principal, root edits
   `src.fs/world/resources/.../harness.md`, or the kernel itself, and goes through release.make and the
   person's card. That is the bootstrap loop, with the task coming from the record instead of a person.
3. Nothing, with a reason: `fix {:by root :what "left: the model recovers on the next turn"}`, so the
   lesson stops being offered.

Each acts by writing `fix` on the lesson through a kernel action, `lesson.fix {principal, at, what}`,
which root holds and no other Principal does. The lesson stays in the list, fixed, until the cap pushes
it out, so the next run can see what was done about it.

### The test says whether it helped

`scripts/test-bootstrap.py` gains one line per run: failed ops by `(action, code)`. A run on a world
whose root has acted on a lesson is expected to show fewer of that kind; the report keeps both
numbers. Two runs of the same task on the same checkout, one with lessons off (`memory/lessons-max` 0)
and one with them on, is the measurement; the task registry already repeats a task on a scratch world.

## What this is not

- Not learning in the model. Lessons are text in the prompt and edits to instruction Fields, on the log,
  readable and revertible by a person. A lesson a person disagrees with is a Field they can clear.
- Not a skill. The punch list's skills item (a how-to as a named Object, shared by Grant) is where a
  fixed lesson may be promoted once it has held across Principals; a lesson is one Principal's record.
- Not retroactive. Lessons fold at the end of a wake from that wake; nothing scans history.

## Order of work

1. `memory/lessons` and `memory/lessons-max` in `memory.clj` defaults and types; the fold at the three
   ends of a wake in `loop/core.clj`, with the kinds `edit/no-match`, `read/unresolved`, `op/refused`,
   `wake/stalled`, `wake/resumed`. A unit test per kind through the stub Model.
2. The serializer line per lesson, with the fixed wording per kind. A serializer test.
3. The bootstrap test's failed-op line and a `--lessons off` switch. One run each way on `view-refs-test`.
4. `lesson.fix` and the heartbeat clause, and root's `instructions/own` as the place a rule lands. A
   test where a Principal's lesson reaches root's heartbeat wake and a rule lands on the Principal.
5. The report, and the decisions row.

Steps 1 to 3 are a day and stand on their own. Step 4 is where root starts improving others, and it
rides on the kernel-swap loop that already holds.
