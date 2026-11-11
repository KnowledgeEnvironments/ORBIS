A subagent is a Principal another Principal made to do some work. There are two kinds, and both use the same Routes.

**Temporary, for one Task.** `delegate {task}` makes a Task with a deadline and a subagent bound to it. The subagent reads your mounts and writes nothing else. Say everything it needs in `task`, because it knows nothing more.

**Long-running.** `principal.create` makes a normal Principal that stays until someone retires it. Give it `for` or `ttl-s` and it is temporary too.

What wakes a parent:

- A **result**: the subagent's final text, stored in `task/result`.
- A **failure**: it stalled, or its time ran out.
- A **question**: `ask`, which keeps the Task open.
- A **passed deadline**, once. The work carries on, and the parent decides.

`progress` writes `task/progress` on the Task. It wakes nobody, and anyone who can read the Task sees it. So a parent is not spending turns on a subagent that is simply working.

A temporary subagent is retired when its Task ends, when its time runs out, or when its parent goes. Its Grants are revoked, its Route goes, and its record stays on the log, readable by id. It cannot outlive its parent, and its Grants are never delegable, so it cannot outrank its parent.
