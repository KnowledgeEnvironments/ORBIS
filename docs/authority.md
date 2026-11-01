# Authority: who can configure whom

Every privileged property of a Principal is a Field or a Grant on the log. Authority comes from one
check, `resolves?`, plus a few rules that sit above it for what the check must not decide.

## The chain

```
world            issues the root Grant and the owner's Grant at boot
 ├ root          scope everything, every op, delegable
 │  └ Principal  strictly narrower than its creator, never delegable
 └ person        every op, delegable, everywhere; the first person's Grant is the world's, not root's
```

Every Grant has a grantor, and `grant!` issues one only under a delegable Grant the grantor holds that
already covers the scope and every op asked for. Authority can only shrink going down. A person's authority sits beside root's: it does not descend from it.

## Why root can configure another Principal

A system prompt is `instructions/*`, memory is `memory/*`, a working directory is a bound Adapter, a
Model is a Grant and a binding. All are writes. Root's Grant covers everything, so `set-field`, `bind`
and `grant` on any Principal resolve. Nothing names root specially: a Principal with `write` on another
Principal can do the same, which is how a narrower developer Principal can manage what it created.

## Why the reverse does not hold

- A new Principal gets `read` and `write` on itself, its own Route, and a Grant strictly narrower than
  its creator's (`err narrower-required`), with `delegate` refused outright.
- Root's Object is outside that scope, so a write to it does not resolve. A miss and a refusal are the
  same `unresolved`, so the attempt learns nothing.
- Without `delegate` it cannot pass on or widen anything it holds, including to itself.
- It can revoke only what it granted.

A temporary Principal holds the same kind of Grant, with an end. Its Grants are never delegable and it cannot outlive its parent, so it cannot pass anything on or make anything that lasts.

## What root cannot do either

`resolves?` answers what a Principal can reach, so anything an agent must not decide for itself sits
above it. These hold for every Principal, root included, and all of them live in one namespace,
`world/src/knowledge/world/authority.clj`. `authority_test.clj` holds the table of who may write what.

| Property | Rule | Enforced at |
|---|---|---|
| `policy/*`: gates, standing rules | Only a person's own session | `set-field`, `field.clear`, `approval.*` |
| Approving a card, `task.decide` | Only a person's own session | `task.decide`, `task.decide-many` |
| Unsafe mode | Armed only by a person, per Principal, time-limited | `approval.unsafe` |
| `principal/driven-by`, `principal/namespace`, `principal/token-hash`, `world/root`, `world/release`, `world/restart`, `world/notifications` | The kernel's: never by `set-field`, from any session | `protocol/ops.clj` `kernel-fields` |
| A Grant a person holds or issued | Revoked only by a person: their session or their own card, no rule, no unsafe mode | `authority/must-card?`, `handle :revoke` |
| A new kernel | A person's own card, never a rule, unsafe mode or a batch | `authority/must-card?`, `world.restart` |
| Ops listed in `policy/approval` | A card for a person, unless a rule admits them | `act/gates.clj` |
| Which Model an agent runs on | Only a person's own session | `model.choose`, `authority/write-refusal` |

Without `kernel-fields`, write on an Object would reach these. Every Principal holds write on itself and
person-only rests on `driven-by`, so a Principal could turn itself into a person and move its own gate.

## A person's terminal

A terminal a person opens from their own session runs as them, outside the box, because the box is for agents. So it is closed to every agent, root included: `pane/person` makes `resolves?` itself answer nil, so `read`, `history`, `pane.list` and `search` all behave as if it were not there.

A person can open one to one agent with `pane.share {pane, principal, mode, minutes}`: per pane, per agent, at most 480 minutes, from their own session only. There is no share with everyone. `read` lets the agent see it. `type` also lets it ask to type: `pane.send` and `pane.frob` on such a pane are gated whatever the agent's policy says, a person's standing rule can admit them, and unsafe mode never does. Holding a share is what permits it, not a Grant, so root's `everything` does not help.

`pane/person` and `pane/shared` are kernel Fields, because root holds write on everything. A share out of time, too long, or not made by a person counts as absent. Every live share is in `world/unsafe` with `kind shared-terminal`, so the red indicator arms and its OFF ends it.

## What root can still do

Root is trusted, and this is the honest edge of it. Root can rewrite another Principal's instructions
and memory, read Fields under its Grant, and propose Principals and Grants for a person to review.

`data/root.token` is the operator's key: the credential the world wrote for its owner at boot. A session on
it passes the gate, as a person's does, because it is how a world is set up and how the supervisor checks
health. It still cannot do anything person-only. Treat it as sudo. A token root issued itself is a process
like any other.

## The box

Gates govern what the protocol does. The box governs what a process can do at all. Every shell command and
terminal runs inside the host's sandbox, `sandbox-exec` on macOS or `bwrap` on Linux, through one interface,
`adapters/confine.clj`.

| Reach | Default |
|---|---|
| Read | The toolchain, the Adapter's root, and `.m2`, `.gitlibs`, `.clojure`, `opt` under home |
| Write | The root, and a scratch directory that is also `TMPDIR` |
| Never | The data dir, `*.token`, `world.env`, `.envrc`, the rest of home, the pasteboard, the keychain, launchd, AppleEvents |
| Network | `adapter/network`: `model` (loopback and the Models' ports), `none`, or `all` |
| Environment | An allow-list, so no key is inherited |

A mount refuses the same secret names, so `read` hands over nothing a shell could not.

- No sandbox on the host, no shell: `err unconfined`. A root that holds the home directory is refused too.
- `adapter.unconfine {adapter, minutes}` lifts it for at most 480 minutes. A person only, and a loop's request is
  always a person's own card. It lists in `world/unsafe` with `kind unconfined`, so the red indicator arms.
- `adapter.confine {adapter, network?, read?}` is a person's. `adapter/locator` and these Fields are not writable.
- The world's own git runs in the box. `git status` can run a repository's `core.fsmonitor` or a filter's `clean`, a
  shell can write both, and the world reads `harness/head` and project status, so a world's git gets the same box.
- `release.make` is how a release is made, because a confined shell cannot write `releases/`, or it could change
  the running kernel with no card. The working copy's git runs inside the box, since an agent wrote its config.

Limits. Seatbelt filters by port and bwrap only on or off, so `model` means any host on a Model's port on
macOS, and the host's network on Linux; `all` is how a LAN is opened. `file-read-metadata` is allowed, so a
command can learn that a path exists and its size, never its contents. Package caches under home are read-only,
because a cache a command writes is one an unconfined JVM loads: a new dependency is fetched outside the box.
A setuid program cannot start in any Seatbelt sandbox, so `top`, `ps` and `sudo` fail with `operation not
permitted`; `uptime` and `htop` work, and `htop` sees only the box's own processes. A file named like a secret
inside a project is unreadable there too. `sandbox-exec` is deprecated by Apple and
works today.

`person.token` stays on disk, 0600, outside every box. `scripts/go.sh` and the supervisor's tests read it again,
so deleting it after first delivery would break them, and the box already keeps it from agents.

## Open

- [ ] `pane/render.clj` runs tectonic outside the box: it needs its cache and first-run network

## TeX is untrusted input

TeX source can come from a model. A formula passes a deny-list of what opens files, runs commands or redefines the language, then compiles in the box: no network, tectonic `--untrusted --only-cached`, writing only to a job directory, with a limit of 4096 characters, 20 seconds and 2 MiB of output, and two at once. `sips` rasterizes in the same box. Packages come from a machine cache, read-only in the box, filled once outside it from a fixed document no model wrote. Only a person's own session can ask, and a test proves a source reading `root.token` or running a command fails with the deny-list bypassed.

The first box was too strict to work in: no network, and no way for a Principal to reach a remote or another machine. The defaults now are these. The network is open. `git` and `ssh` work in every shell through an `ssh-agent` the world keeps outside the box, so a command can use this machine's keys and cannot read them. A command still writes only its mounts, and still cannot read the home directory or a secret by name. `adapter.confine {network, ssh, read}` tightens or opens one shell.

Root is the operator, on a wide leash (`principal/wide-leash`, on by default, `KE_ROOT_LEASH=0` to start without). Its own shell runs outside the box, with no card for ordinary work, because it runs, tests and configures everyone else. What a Principal needs, it asks root for, and root decides: it grants, mounts and uses `adapter.confine` without a person, on the log. What stays a person's: a kernel swap, `adapter.unconfine`, revoking a person's Grant, and root's own leash, which `adapter.confine {adapter, leash: false}` ends. The trade: an agent reading hostile text can ask root for something, and root's judgment is then all that stands between them.

What is not confined: the priming run, which has network and runs outside the box, on that fixed document; and the rendered PNG, which a client decodes with stb_image.

## What a wake may do for another agent

In a wake another process Principal started, a Principal not on root's leash widens nothing: `project.add`, `workspace.mount`, `adapter.unconfine`, `grant`, `principal.create` and `delegate` are refused with `not-from-a-person`, before any card. An agent stopped by its box does not get through by asking root. A card raised in such a wake says which agent asked.

A denial covers its end. For an hour, any op of that Principal reaching a file the denied card named is gated as `after-denial`, whatever its policy says. No rule and no unsafe mode admits it, and the card says what was denied.

## Audit

Root's charter asks it to answer "who can do what" from the record: Grants and grantors, Routes,
`policy/*`, standing rules, tokens, and `history`.
