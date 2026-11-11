// Text and its references, as the world sends them: `message/text` is plain display text and
// `message/refs` says which spans of it are what, `[{start end kind object field? seq? name}]` in
// code points. `<field>/refs` works the same beside any text Field. Links come from these spans and
// never from reading the text; chat/Symbols is only for text that has no refs Field at all.
#pragma once

#include "chat/Symbols.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ke::chat {

struct RefSpan {
    std::size_t start = 0, end = 0; // bytes in the text, [start, end)
    SymbolKind kind = SymbolKind::Object;
    std::string object, field;
    Seq seq = 0;
    std::string name; // the name when it was written, for an Object since retired
    std::string path; // a file on the mount `object`, which the world found when the text was written
    int line = 0;     // the line it cites, 0 for none
    bool operator==(const RefSpan&) const = default;
};

// In order, never overlapping; a span that does not fit the text is dropped.
std::vector<RefSpan> parseRefs(const nlohmann::json& refs, std::string_view text);
// The spans inside `part`, a piece of `text`, with offsets into `part`. `part` empty or not found: none.
std::vector<RefSpan> spansIn(const std::vector<RefSpan>& spans, std::string_view text, std::string_view part);
// The symbol a span stands for, for hover and click. `current` is the fold's name, "" when it has none.
Symbol symbolOf(const RefSpan& span, std::string_view text, const std::string& current);
// `refs` for a line the person typed: each `@`-picked {id, name} at its first whole-word occurrence.
nlohmann::json mentionRefs(std::string_view text, const std::vector<std::pair<std::string, std::string>>& picked);

} // namespace ke::chat
