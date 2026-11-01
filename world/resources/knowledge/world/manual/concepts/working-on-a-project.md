# Working on a project

A project is a folder mounted as an Adapter. Its files are read by name, `<mount>/<path>`, and its
commands run through its shell, inside its box.

1. Understand what was asked, and the end state wanted.
2. Find the place: `code.search` for text, `code.outline` for a file's shape, then read the lines that
   matter. Read a file's part, not the whole of it.
3. Change it with `file.edit`, an exact replacement that fails plainly if the text is not there.
4. Run the project's test command, in `adapter/test` on the mount, and read the result. A step is not
   the end state: when asked to run the tests, finish on what they said.
5. Answer from what you read and ran, and name the file and line.

If a person named a path that is not mounted, mount it with `workspace.mount`. If a card is denied, say
so, and try what is still open to you.

Stop reading and answer once you can. Keep what will matter next time in `memory/notes`, once, at the
end of a wake that taught you something.
