// The kernel record a loop's reply ends with (world pass 13): the commands its wake ran, from the
// Task Fields rather than the model's prose. The world appends it as text with a fixed heading:
//
//   Runs this wake (kernel record):
//   - `make test-unit` → exit 0 (129 s)
//       133 tests, 1695 assertions, 0 failures.
//
// The terminal parses it back out of the reply so it can draw a framed box (command, exit in
// green or red, duration, stdout tail) instead of plain text, and so a `reply/contradiction`
// Field can point at the run the prose contradicted. Pure functions, tested without ImGui.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ke::chat {

struct RunRecord {
    std::string cmd;
    std::optional<long long> exit;      // absent when the world could not say
    std::optional<long long> durationS; // the `(N s)` part
    std::vector<std::string> tail;      // the stdout tail lines under the run
    std::string task;                   // filled by the terminal when the fold names the Task
    bool operator==(const RunRecord&) const = default;
};

struct KernelRecord {
    bool present = false;         // the heading was found
    std::string body;             // the reply without the record (trailing blank lines removed)
    std::vector<RunRecord> runs;
};

// The fixed heading the world writes, without the surrounding newlines.
inline constexpr const char* kKernelRecordHeading = "Runs this wake (kernel record):";

KernelRecord splitKernelRecord(const std::string& text);

} // namespace ke::chat
