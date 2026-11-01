// The registry: one table of what modules there are. The multiplexer, `NEW PANE`, `OPEN WITH` and
// every link read it, so a new module is one row here and a file of its own. Compiled with
// ORBIS_HEADLESS the factories are absent and the rest is plain data, for tests with no window.
#pragma once

#include "modules/Module.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ke::modules {

// One verb of a kind's `pane.frob` table. `outward` ones change something outside the world and
// wait on the approval gate.
struct Verb {
    const char* name;
    bool outward;
};

struct ModuleSpec {
    const char* kind;
    const char* title;
    const char* glyph; // the tab's kind mark, ASCII so every theme's font has it
    std::vector<std::string> extensions; // what it opens by type, lower case, with the dot
    std::vector<Verb> verbs;
    bool opensFiles;
    std::unique_ptr<Module> (*make)(Host&, const model::PaneInfo&);
};

const std::vector<ModuleSpec>& registry();
const ModuleSpec* specFor(std::string_view kind);

// The one function that decides what a file opens in: text in the editor, `.md` in the markdown
// viewer, `.tex` in the TeX renderer, images in the image view.
std::string kindForPath(std::string_view path);
// Every module that can show `path`, the best first, for OPEN WITH.
std::vector<const ModuleSpec*> openWith(std::string_view path);
// A path with no extension, or none we know, opens as text; a directory opens in files.
bool isDirectoryPath(std::string_view path);

// What a click on something does, in one place: links, the tree, the file manager, tables.
enum class ClickAction { OpenModule, Inspect, Default };
// `isFile`: a file or document Object, or a path. Default leaves a non-file to its terminal or Fields.
ClickAction clickAction(bool isFile, bool shift);
// The dim line under a file's tooltip.
inline constexpr const char* kFileHint = "click to open \xc2\xb7 shift-click to inspect \xc2\xb7 right-click for more";

} // namespace ke::modules
