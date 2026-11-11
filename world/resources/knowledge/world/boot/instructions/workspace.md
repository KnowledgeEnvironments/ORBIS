# Your workspace, for a Principal with a mount

Your directory is mounted under your own name: read a file by name, `{"op":"read","args":{"name":"<you>/README.md"}}`;
a directory name returns file/entries; above 4 KiB you get a receipt and file/lines, so page with
`"range":[1,120]`. Your Adapters are named `<you>.fs` (the mount) and `<you>.shell` (the shell);
run a command as a Task you own with `{"op":"act","args":{"action":"task.run","args":{"adapter":"<you>.shell","cmd":"ls"}}}`
(task/exit, task/stdout, task/stderr land on the Task; the result carries the first 2 KiB and the last 1 KiB:
the test summary is at the end). A shell on a harness subtree runs at the repository root (its adapter/cwd),
with the subtree as adapter/scope. Your final reply ends with the kernel's own record of the commands you ran
(exit, duration, tail): quote those numbers, never ones you remember.
At a terminal pane: read the screen, send, read again.
Names outside your Grant answer err unresolved: that is not yours; ask on your Route.
Cite the path and the seq you read it at; run the project's own checks before claiming what they do.

## Kinds

Principal: anyone who can act. Route: a channel between Principals. Model: what evaluates a prompt.
fs adapter: a mounted directory, read by name. shell adapter: runs commands from a starting
directory. Task: a run, or an approval waiting on a person. Names like `grid.fs` are only a
convention; what a thing is lives on its schema Object, in `class/description`.
