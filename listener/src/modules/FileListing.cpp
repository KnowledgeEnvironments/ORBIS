#include "modules/FileListing.hpp"

#include "modules/Registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace ke::modules {

namespace {

using nlohmann::json;

std::string str(const json& o, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (o.contains(k) && o[k].is_string()) {
            return o[k].get<std::string>();
        }
    }
    return "";
}

FileEntry entryOf(const json& o) {
    FileEntry e;
    if (o.is_string()) {
        std::string s = o.get<std::string>();
        if (!s.empty() && s.back() == '/') {
            s.pop_back();
            e.kind = "dir";
        } else {
            e.kind = "file";
        }
        e.name = std::move(s);
        return e;
    }
    e.name = str(o, {"name", "file", "path"});
    if (const std::size_t slash = e.name.rfind('/'); slash != std::string::npos && slash + 1 < e.name.size()) {
        e.name = e.name.substr(slash + 1);
    }
    e.modified = str(o, {"modified", "mtime", "date", "at"});
    e.kind = str(o, {"kind", "type"});
    if (o.contains("dir") && o["dir"].is_boolean() && o["dir"].get<bool>()) {
        e.kind = "dir";
    }
    if (e.kind.empty()) {
        e.kind = "file";
    }
    if (o.contains("size") && o["size"].is_number()) {
        e.size = o["size"].get<std::int64_t>();
    }
    return e;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

std::vector<FileEntry> parseListing(const json& result) {
    std::vector<FileEntry> out;
    const json* list = nullptr;
    if (result.is_array()) {
        list = &result;
    } else if (result.is_object()) {
        for (const char* k : {"entries", "listing", "files"}) {
            if (result.contains(k) && result[k].is_array()) {
                list = &result[k];
                break;
            }
        }
        if (list == nullptr) {
            const std::string text = str(result, {"text", "listing", "screen"});
            std::istringstream lines(text);
            for (std::string l; std::getline(lines, l);) {
                if (!l.empty() && l != "./" && l != "../") {
                    out.push_back(entryOf(json(l)));
                }
            }
            return out;
        }
    }
    if (list != nullptr) {
        for (const json& o : *list) {
            FileEntry e = entryOf(o);
            if (!e.name.empty() && e.name != "." && e.name != "..") {
                out.push_back(std::move(e));
            }
        }
    }
    return out;
}

void sortListing(std::vector<FileEntry>& entries, FileSort sort, bool descending) {
    std::stable_sort(entries.begin(), entries.end(), [&](const FileEntry& a, const FileEntry& b) {
        if (a.directory() != b.directory()) {
            return a.directory();
        }
        int c = 0;
        switch (sort) {
        case FileSort::Size:
            c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
            break;
        case FileSort::Date:
            c = a.modified.compare(b.modified);
            break;
        case FileSort::Kind:
            c = kindLabel(a).compare(kindLabel(b));
            break;
        case FileSort::Name:
            break;
        }
        if (c == 0) {
            return lower(a.name) < lower(b.name);
        }
        return descending ? c > 0 : c < 0;
    });
}

std::string formatSize(std::int64_t bytes) {
    if (bytes < 0) {
        return "-";
    }
    if (bytes < 1024) {
        return std::to_string(bytes);
    }
    static const char kUnits[] = "KMGT";
    double v = static_cast<double>(bytes);
    int u = -1;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, v < 10.0 ? "%.1f%c" : "%.0f%c", v, kUnits[u]);
    return buf;
}

std::string shortDate(std::string_view iso) {
    if (iso.size() < 16) {
        return std::string(iso);
    }
    return std::string(iso.substr(5, 5)) + " " + std::string(iso.substr(11, 5));
}

std::string kindLabel(const FileEntry& e) {
    if (e.directory()) {
        return "dir";
    }
    if (e.kind == "link") {
        return "link";
    }
    const std::string kind = kindForPath(e.name);
    return kind == "editor" ? "text" : kind;
}

std::string parentPath(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return "";
    }
    return slash == 0 ? "/" : std::string(path.substr(0, slash));
}

std::string joinPath(std::string_view dir, std::string_view name) {
    if (dir.empty()) {
        return std::string(name);
    }
    std::string out(dir);
    if (out.back() != '/') {
        out += '/';
    }
    return out + std::string(name);
}

void FilePanel::set(std::vector<FileEntry> list) {
    const std::string keep = current() ? current()->name : "";
    entries = std::move(list);
    sortListing(entries, sort);
    error.clear();
    loading = false;
    cursor = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == keep) {
            cursor = static_cast<int>(i);
        }
    }
}

void FilePanel::move(int delta) {
    cursor = entries.empty() ? 0 : std::clamp(cursor + delta, 0, static_cast<int>(entries.size()) - 1);
}

const FileEntry* FilePanel::current() const {
    return cursor >= 0 && cursor < static_cast<int>(entries.size()) ? &entries[static_cast<std::size_t>(cursor)] : nullptr;
}

} // namespace ke::modules
