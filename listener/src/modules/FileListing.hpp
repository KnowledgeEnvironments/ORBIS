// What the files module knows about a directory: the world's listing, sorted, with a cursor. The
// client has no filesystem; this only parses and walks what the world answered.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ke::modules {

struct FileEntry {
    std::string name;
    std::int64_t size = -1; // -1 when the world did not say
    std::string modified;   // as the world gave it
    std::string kind;       // file, dir, link
    bool directory() const { return kind == "dir" || kind == "directory"; }
};

// The result of `pane.read {what listing}` or the `list` frob. Tolerant: `{entries [...]}`,
// `{listing [...]}`, a bare array, or `{text "..."}` with one name per line, a `/` marking a
// directory, since the world's first answer is text.
std::vector<FileEntry> parseListing(const nlohmann::json& result);

enum class FileSort { Name, Size, Date, Kind };

// Directories first, then by `sort`.
void sortListing(std::vector<FileEntry>& entries, FileSort sort, bool descending = false);

std::string formatSize(std::int64_t bytes); // `812`, `4.1K`, `1.2M`, `-` when unknown
std::string shortDate(std::string_view iso); // `10-30 21:00`
// What a person calls a file: `markdown`, `text`, `image`, `dir`.
std::string kindLabel(const FileEntry& e);

std::string parentPath(std::string_view path); // `/a/b` -> `/a`; `/` stays; `a` -> ``
std::string joinPath(std::string_view dir, std::string_view name);

// One side of the manager: a directory and a cursor over its listing.
struct FilePanel {
    std::string path;
    std::vector<FileEntry> entries;
    int cursor = 0;
    FileSort sort = FileSort::Name;
    std::string error; // the world's answer when a listing failed
    bool loading = false;

    void set(std::vector<FileEntry> list); // keeps the cursor on the same name where it can
    void move(int delta);
    const FileEntry* current() const;
};

} // namespace ke::modules
