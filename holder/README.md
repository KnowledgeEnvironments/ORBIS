# The terminal holder

A small process per world that owns its PTYs and outlives it, so a terminal goes on through a restart. Why and how are in `docs/panes.md`.

```
make holder                          from the repo root; needs the Zig in PIN and, once, the network
holder/build/holder <socket>         the supervisor runs this
python3 holder/tests/holder_test.py holder/build/holder
```

`GHOSTTY_SRC=<checkout>` uses an existing checkout rather than cloning into `third_party/`.

libghostty-vt (MIT, ghostty-org/ghostty) is built from the commit in `PIN`, with `zig build -Demit-lib-vt`, and linked statically. Its API is pre-1.0, so `src/vt.c` is the only file that includes it: an upgrade changes that file and `PIN`.

The holder is C11 and POSIX, one thread, one poll loop, no locks. The frames are in `src/main.c`, and the world's end is `world/src/knowledge/world/pane/holder.clj`. The protocol has a version, and both ends must agree.
