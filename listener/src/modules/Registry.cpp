#include "modules/Registry.hpp"

#include <algorithm>
#include <cctype>

#if defined(ORBIS_HEADLESS)
#define MODULE(fn) nullptr
#else
#define MODULE(fn) &fn
namespace ke::modules {
std::unique_ptr<Module> makeTerminal(Host&, const model::PaneInfo&);
std::unique_ptr<Module> makeFiles(Host&, const model::PaneInfo&);
std::unique_ptr<Module> makeEditor(Host&, const model::PaneInfo&);
std::unique_ptr<Module> makeMarkdown(Host&, const model::PaneInfo&);
std::unique_ptr<Module> makeLatex(Host&, const model::PaneInfo&);
std::unique_ptr<Module> makeImage(Host&, const model::PaneInfo&);
} // namespace ke::modules
#endif

namespace ke::modules {

const std::vector<ModuleSpec>& registry() {
    static const std::vector<ModuleSpec> table = {
        {"terminal", "TERMINAL", ">_", {}, {}, false, MODULE(makeTerminal)},
        {"files", "FILES", "[]", {}, {{"cd", false}, {"list", false}, {"select", false}, {"open", false},
                                      {"mkdir", true}, {"rename", true}, {"copy", true}, {"move", true}, {"delete", true}},
         false, MODULE(makeFiles)},
        {"markdown", "MARKDOWN", "md", {".md", ".markdown", ".mdown"}, {{"scroll", false}, {"goto", false}, {"render", false}},
         true, MODULE(makeMarkdown)},
        {"latex", "TEX", "tx", {".tex", ".ltx", ".latex"}, {{"scroll", false}, {"goto", false}, {"render", false}},
         true, MODULE(makeLatex)},
        {"image", "IMAGE", "im", {".png", ".jpg", ".jpeg", ".gif", ".bmp"}, {{"scroll", false}, {"goto", false}},
         true, MODULE(makeImage)},
        // Last: it opens anything the others do not claim.
        {"editor", "EDITOR", "ed", {}, {{"goto", false}, {"find", false}, {"insert", false}, {"replace", false},
                                        {"undo", false}, {"redo", false}, {"save", true}},
         true, MODULE(makeEditor)},
    };
    return table;
}

const ModuleSpec* specFor(std::string_view kind) {
    for (const ModuleSpec& s : registry()) {
        if (kind == s.kind) {
            return &s;
        }
    }
    return nullptr;
}

namespace {

std::string extensionOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return "";
    }
    std::string ext(base.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

std::string kindForPath(std::string_view path) {
    if (isDirectoryPath(path)) {
        return "files";
    }
    const std::string ext = extensionOf(path);
    for (const ModuleSpec& s : registry()) {
        if (std::find(s.extensions.begin(), s.extensions.end(), ext) != s.extensions.end()) {
            return s.kind;
        }
    }
    return "editor";
}

std::vector<const ModuleSpec*> openWith(std::string_view path) {
    std::vector<const ModuleSpec*> out;
    if (const ModuleSpec* best = specFor(kindForPath(path)); best != nullptr) {
        out.push_back(best);
    }
    for (const ModuleSpec& s : registry()) {
        if (!s.opensFiles || (!out.empty() && out.front() == &s)) {
            continue;
        }
        // An image is no use as text, and text no use as an image.
        const bool image = std::string_view(s.kind) == "image";
        if (image && kindForPath(path) != "image") {
            continue;
        }
        out.push_back(&s);
    }
    return out;
}

bool isDirectoryPath(std::string_view path) { return !path.empty() && path.back() == '/'; }

ClickAction clickAction(bool isFile, bool shift) {
    if (shift) {
        return ClickAction::Inspect;
    }
    return isFile ? ClickAction::OpenModule : ClickAction::Default;
}

} // namespace ke::modules
