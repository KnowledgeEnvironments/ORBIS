#include <catch2/catch_test_macros.hpp>

#include "ui/Themes.hpp"

#include <filesystem>
#include <fstream>

using namespace ke::ui;
using nlohmann::json;

TEST_CASE("colours read with and without alpha") {
    CHECK(parseColor("#FF0000") == 0xFF0000FFu);
    CHECK(parseColor("#00FF0080") == 0x8000FF00u);
    CHECK_FALSE(parseColor("red"));
    CHECK_FALSE(parseColor("#GG0000"));
    CHECK_FALSE(parseColor("#12345"));
}

TEST_CASE("the shipped theme is data, and a missing name falls back to it") {
    const ThemeLibrary lib;
    REQUIRE(lib.all().size() == 1);
    CHECK(lib.all().front().name == "prometheus");
    const ThemeSpec& p = lib.named("prometheus");
    CHECK(p.role(ChromeRole::RaisedPane).bevel == 3.0f);
    CHECK(p.role(ChromeRole::InsetWell).bevel == 2.0f);
    CHECK(p.role(ChromeRole::ScrollControl).bevel == 1.0f);
    CHECK_FALSE(p.bracketedControls);
    CHECK(p.brand[0] != 0);
    CHECK(p.ink.unsafe != 0);
    CHECK(lib.named("phosphor").name == "prometheus");
    CHECK(lib.named("no such theme").name == "prometheus");
}

TEST_CASE("a third theme needs no code, and what it leaves out comes from the default") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "orbis-theme-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "ember.json") << R"({"name": "ember", "ink": {"primary": "#FF8800"}})";
    std::ofstream(dir / "broken.json") << R"({"name": "broken", "ink": {"primary": "orange"}})";
    std::ofstream(dir / "junk.json") << "not json";
    ThemeLibrary lib;
    const std::size_t shipped = lib.all().size();
    lib.loadDirectory(dir);
    CHECK(lib.all().size() == shipped + 1);
    const ThemeSpec& e = lib.named("ember");
    CHECK(e.ink.primary == *parseColor("#FF8800"));
    CHECK(e.ink.inspected == lib.named("prometheus").ink.inspected);
    fs::remove_all(dir);
}
