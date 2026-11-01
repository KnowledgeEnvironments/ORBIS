// The kernel record block a loop's reply ends with (world pass 13), parsed back out of the text.
#include "chat/KernelRecord.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke::chat;

TEST_CASE("kernel record: heading, runs, exits, durations and tails come apart from the prose", "[kernel-record]") {
    const std::string text =
        "All green: 133 tests.\n\nRuns this wake (kernel record):\n"
        "- `make test-unit` → exit 0 (129 s)\n"
        "    133 tests, 1695 assertions, 0 failures.\n"
        "    Ran 1 test suite.\n"
        "- `ctest --preset macos-debug` → exit 2\n"
        "    Errors while running CTest\n";
    const KernelRecord r = splitKernelRecord(text);
    REQUIRE(r.present);
    CHECK(r.body == "All green: 133 tests.");
    REQUIRE(r.runs.size() == 2);
    CHECK(r.runs[0].cmd == "make test-unit");
    CHECK(r.runs[0].exit == 0);
    CHECK(r.runs[0].durationS == 129);
    REQUIRE(r.runs[0].tail.size() == 2);
    CHECK(r.runs[0].tail[1] == "Ran 1 test suite.");
    CHECK(r.runs[1].cmd == "ctest --preset macos-debug");
    CHECK(r.runs[1].exit == 2);
    CHECK_FALSE(r.runs[1].durationS.has_value());
    REQUIRE(r.runs[1].tail.size() == 1);
}

TEST_CASE("kernel record: an ASCII arrow and a record with no prose parse too; no heading means no record", "[kernel-record]") {
    const KernelRecord a = splitKernelRecord("Runs this wake (kernel record):\n- `ls -la` -> exit 0 (1 s)");
    REQUIRE(a.present);
    CHECK(a.body.empty());
    REQUIRE(a.runs.size() == 1);
    CHECK(a.runs[0].cmd == "ls -la");
    CHECK(a.runs[0].exit == 0);
    CHECK(a.runs[0].durationS == 1);
    const KernelRecord c = splitKernelRecord("Runs this wake (kernel record):\n- `make test-schema` \u2192 exit 0 (1 s)\n    \x1b[32m8 tests, 235 assertions, 0 failures.\x1b[m\n");
    REQUIRE(c.present);
    REQUIRE(c.runs.size() == 1);
    REQUIRE(c.runs[0].tail.size() == 1);
    CHECK(c.runs[0].tail[0] == "8 tests, 235 assertions, 0 failures."); // the colour codes are gone
    const KernelRecord b = splitKernelRecord("Just prose, with `code` and an arrow → nowhere.");
    CHECK_FALSE(b.present);
    CHECK(b.body == "Just prose, with `code` and an arrow → nowhere.");
    CHECK(b.runs.empty());
}
