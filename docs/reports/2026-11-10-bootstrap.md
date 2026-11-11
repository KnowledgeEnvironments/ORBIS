# Bootstrap: a fresh world improves itself on a real Model

`scripts/test-bootstrap.py`, 2026-11-10. A clean data directory, a release of the checkout's HEAD under
its supervisor on a private port, root on `gpt-5.6-sol` through a signed-in ChatGPT record, and a real
open bug from the punch list as the task. The person's side is the script: it decides root's cards by a
policy it prints, approves the kernel swap (always the person's own card), and checks the result from
outside the world. Nothing in the running worlds was touched.

The task: a person's `project.add` failed with `no Model: give model` even when `model` was given
(punch list, 2026-11-05). Acceptance in the prompt: fix, a focused test, run it, commit, `release.make`,
`world.restart`, then after the restart read `world/release` and report with evidence.

## Run 1: the work, then silence

Root read `instructions/harness`, outlined `src.fs/world/src/knowledge/world/act`, searched for
`adopt-project!` and `no Model: give`, read the handler, the schema and the existing projects test, and
edited both files and the test. It ran the focused namespace (21 tests, 81 assertions), `git diff
--check`, committed, called `release.make` (commit 8a028aab), wrote `memory/notes`, and asked
`world.restart`. 76 ops, 5 minutes to the card.

The card was approved and the supervisor brought the world up on 8a028aab: healthy after 5 s, good
after 10. Then nothing for 26 minutes.

The log said why. The wake that asked ended at `approval-pending` (`loop/iteration` back to 0). The
approval-result Message started a new wake, evaluation ffb379cc, running when the process exited. At
boot `loop/resume.clj` marked it failed with reason restart and re-queued nothing: `approval-result` was
in its `not-a-wake` set. The scripted self-hosting test never saw this because it restarts in unsafe
mode, where the asking wake itself is the orphan and resumes.

Fix: the kind rule applies only to Messages the lane never saw. A Message the loop had already started
on is a wake whatever its kind, except a Trigger's, which fires again by itself. Test
`a-wake-the-loop-had-started-resumes-whatever-its-kind` fails without the change and passes with it
(decisions 2026-11-10).

## Run 2: through

| | |
|---|---|
| Wall | 520 s; 271 s of model time, the rest commands and the restart |
| Evaluations | 44; 1.40M tokens in, 7.4k out |
| Ops | 118, 12 failed (below) |
| To the card | 448 s |
| Cards | 1: the kernel swap. Reads and tests ran on the person's `read-only` and `test` rules; every build, git and test command fit the policy |
| Release | 7941d449, healthy after 4 s, good after 10 |

After the restart the wake resumed, root read `world/release`, and replied with the commit, the three
files with line ranges, the test command and its count, the move from 9332db8 to 7941d44, and the kernel's
record of its runs. `memory/notes` carries the commit, the test seqs and the restart. Then, from outside:
`release.good` is root's commit, `src` HEAD is that commit, the commit touches the files the task named,
three test runs preceded `release.make`, and a person's `project.add` with `model` succeeds on the new
world where it was refused on the old one.

Root's change is better than the one from run 1: `model` may be a name or an id, and the fallback to
root's first Model sits in `principal.create` itself, so any caller without a Model gets one. It is
merged to master as 052c7fb, author root, with one indentation fix.

### The 12 failed ops

- 7 `file.edit` no-match: the `old` text was off by whitespace each time; the error's "nearest line" got
  root there on the next try, at a turn each.
- 3 `read` unresolved: `src/<path>`, where the mount is bound `src.fs/<path>`. The harness page said
  `src/<path>`; it now says `src.fs`.
- 1 `test` refused: root works across projects, so the bare `test` tool has no one project; the refusal
  told it to `run` the command, which it did.
- 1 `world.restart` answered `approval-pending`, which is the card.

## Run 3: a second task, the same day

Task `view-refs-test`: `title-refs` and `data-refs` in views had no test. Root read the view code and the
loop's reference parsing, wrote `world/test/knowledge/world/view/refs_test.clj` (one test, eight
assertions: display text, and the exact spans recorded on `view/title/refs` and `view/data/refs`), ran it
three times, committed, released and asked the swap. 200 s wall, 150 s of model time, 26 evaluations,
500k tokens in, 56 ops, 3 failed: two reads of `src/<path>` (after the harness page fix, down from
three), and the card. Every check passed; the test is on master as 2a292e4.

The run before this one came up unauthenticated: run 2 had refreshed the ChatGPT token into its own
copy, and the record it was seeded from was stale. The test now owns a record store and writes the
refreshed record back after each run.

## What this shows and what it does not

The self-hosting loop holds on a real Model: read, change, test, release, ask, swap, resume, report, and
the person can check each step without trusting the report. One real defect in that loop was found by
the first run and is fixed with a test.

It is two tasks, one Model, one run each. The fixed task is now closed on master, so the next run needs
an open one (`--task`); the script carries a registry and refuses to run a task whose bug is gone. The
"self-improving" half that does not exist yet is the lesson: nothing turns a wake's seven whitespace
misses into a rule, or its three wrong mount names into a correction root makes to its own harness
page. That is the next thing to build, and this test is where to measure it.

## How to run it

```
scripts/test-bootstrap.py <scratch> --port 7811 --record <credentials/chatgpt dir of a stopped world> [--task <name>] [--keep]
```

The record is a ChatGPT sign-in a stopped world made; the script copies it under the new Model's id
and marks the Model as plan-backed. Two worlds must not share one record, since the refresh token
rotates. `results.json` in the scratch directory holds every op, card decision and reply.
