#!/bin/zsh
# Close Terminal.app windows whose program has ended, the ones showing [Process completed].
# A busy tab, or an idle shell at a prompt, is left alone. Worlds started through Terminal leave these.
exec osascript <<'APPLESCRIPT'
tell application "Terminal"
  set closed to 0
  repeat with i from (count of windows) to 1 by -1
    set w to window i
    set t to selected tab of w
    if (busy of t) is false then
      set body to history of t
      set finished to false
      repeat with p from (count of paragraphs of body) to 1 by -1
        set line_ to paragraph p of body
        if line_ is not "" then
          if line_ contains "[Process completed]" then set finished to true
          exit repeat
        end if
      end repeat
      if finished and (count of tabs of w) is 1 then
        close w saving no
        set closed to closed + 1
      end if
    end if
  end repeat
  return "closed " & closed & " of " & (closed + (count of windows)) & " windows"
end tell
APPLESCRIPT
