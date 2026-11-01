// Attaching a local file to a conversation (sprint 1 L8): a file dropped on a terminal becomes an
// Object in the world and a Message on the Route that tags it, so the recipient's working set
// includes it. The pure side lives here: reading and classifying the file (text or bytes, mime,
// size, sha256), the Fields to create it with, the caps, and a folder's listing as a tree view.
// The chat issues the ops: `act {class object action create args {fields}}`, then `set-field`
// of file/text (string) or file/bytes (bytes, base64 on the wire; the world blobs > 4 KiB), then
// `send {to route text "attached: <name>" tags [object]}`.
#pragma once

#include "model/Event.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace ke::chat {

constexpr std::size_t kAttachMaxBytes = 8u * 1024u * 1024u; // one file; above it the drop is refused
constexpr std::size_t kAttachTextMax = 64u * 1024u;         // text up to this goes in file/text

struct Attachment {
    std::string path;   // as dropped
    std::string name;   // the base name
    std::string mime;   // from the extension, else text/plain or application/octet-stream
    std::size_t size = 0;
    std::string sha256; // hex
    bool text = false;  // file/text (a string Field) rather than file/bytes
    std::string content; // the bytes (or the text)
    std::string refusal; // non-empty when the file cannot be attached (too big, unreadable)
    bool directory = false;
};

// Mime type from a file name's extension ("" when unknown).
std::string mimeOf(const std::string& name);
// Valid UTF-8 with no NUL bytes (so it may travel as a string Field).
bool isText(const std::string& bytes);
// Read and classify a path; `refusal` set when it cannot be attached. Directories are flagged,
// not read.
Attachment classify(const std::string& path);
// The create args for the Object (the small Fields; the content goes in its own set-field).
json createFields(const Attachment& a);
// The set-field for the content: {field type value}.
struct ContentField {
    std::string field; // file/text or file/bytes
    json type;         // "string" or "bytes"
    json value;        // the text, or base64
};
ContentField contentField(const Attachment& a);
// A folder's top two levels as a tree view payload ({label, children [...]}, dirs first), capped
// at `maxEntries` per level; `truncated` set when a level was cut.
json folderTree(const std::string& path, std::size_t maxEntries, bool& truncated);
// The Fields of a tagged view Object carrying a tree listing (view/kind tree, view/title, view/data).
json folderViewFields(const std::string& path, const json& tree, bool truncated);

} // namespace ke::chat
