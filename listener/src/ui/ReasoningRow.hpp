// The row beside the input line: whether this model reasons, and whether reasoning is shown.
// One component, on a Principal's terminal and in the off-the-record window, so the two read and
// behave the same. It draws and reports; the caller owns where the choice goes.
#pragma once

#include <optional>
#include <string>

namespace ke::ui {

struct ReasoningRow {
    bool settable = true;   // false on a person's own terminal: a person has no model
    std::string mode;       // "on" or "off"
    std::string source;     // one line for the hover: where that mode comes from
    std::string scope;      // drive target prefix, so two rows never share a name
};

// Draws the row. `show` is the display switch. Returns "on" or "off" when the mode was clicked.
std::optional<std::string> drawReasoningRow(const ReasoningRow& row, bool& show);

// The height to reserve for it at this width: it wraps on a narrow pane.
float reasoningRowHeight(float width, bool settable);

} // namespace ke::ui
