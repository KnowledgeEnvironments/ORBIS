# First steps

A known-good first session, each line a tool call that works as written. A test runs this exact list, so it cannot go stale. Wherever a call takes a reference, give a short handle from your prompt, a bound name or a full id. Leave out arguments you do not use.

See what you can reach:

    outline {}

Read a file, by `<mount>/<path>`. `range` is lines, so a `path:line` citation is one read away:

    read {"name": "harness/README.md", "range": [1, 20]}

Find something. `pattern` is literal unless `regex` is true:

    grep {"pattern": "first line"}

Read an Object, here yourself:

    read {"object": "root"}

Keep what you learned, on a Field. It is a write, so it shows in the record:

    set-field {"object": "root", "field": "memory/notes", "type": "string", "value": "first steps done"}

Run a command. This raises a card: the wake ends, and the person's decision wakes you with the result:

    run {"cmd": "ls"}

Find it all in the record:

    history {"object": "root", "limit": 20}

Then reply: your final text reaches the person by itself. Do not `send` it.

To show TeX, put it in a `tex` fence, a fragment or a whole document, and it is typeset. Use `$...$` in a line and `$$...$$` for display, for math only: never wrap text or `\section` in dollars.

A small diagram goes in a `tex` fence too, and is usually better than ASCII art: a `tikzpicture` with no preamble, a `tikzcd`, or a pgfplots `axis` inside a `tikzpicture`. Keep it about a chat wide and in black lines, since the client inks it to match the screen. These libraries are there without asking: arrows.meta, positioning, shapes.geometric, shapes.misc, calc, fit, matrix, automata, decorations.pathmorphing. A failure shows TeX's reason under the source; nothing else can be fetched.

```tex
\begin{tikzpicture}[>=Stealth]
  \node[draw] (a) {world};
  \node[draw, right=of a] (b) {client};
  \draw[->] (a) -- (b);
\end{tikzpicture}
```
