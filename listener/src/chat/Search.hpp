// Search (sprint 1, MVP item): the world's `search` op answers hits the caller can read; until a
// world has it, the client searches what it already holds (the Principals fold's Fields and names,
// the Routes, the Messages read into the cache) and says `local`. Both paths land in the same
// SearchHit, with the match offsets in the snippet so the pane can draw it in inverse video.
#pragma once

#include "chat/MessageCache.hpp"
#include "model/Principals.hpp"

#include <string>
#include <vector>

namespace ke::chat {

struct SearchHit {
    std::string object;  // the Object the hit is on (a Principal, Route, Message, ...)
    std::string name;    // a name for it (the world's `name`, or nameOf locally)
    std::string field;   // the Field the text was found in ("" for a name hit)
    std::string kind;    // field | message | object (the world's `kind` when it sends one)
    std::string snippet; // the matched text with some context
    std::size_t matchStart = std::string::npos; // offsets of the query inside `snippet`
    std::size_t matchLen = 0;
    Seq seq = 0;
    double score = 0.0;
    bool operator==(const SearchHit&) const = default;
};

// Case-insensitive (ASCII) substring search over the folds: Field values of every Principal and
// Route, their bound names, and the texts of the Messages the cache has read. Whole-word matches
// score 2, substrings 1; hits come newest first (seq), then by score; at most `limit`.
std::vector<SearchHit> localSearch(const model::Principals& fold, const MessageCache& messages, const std::string& text,
                                   std::size_t limit = 50);

// The world's `search` result {hits [{object field? name? seq snippet score kind}] total} as hits,
// the match offsets found by looking for `text` inside each snippet.
std::vector<SearchHit> parseWorldHits(const json& result, const std::string& text);

// A snippet of `value` around its first match of `text` (about `context` characters either side,
// ellipses when clipped) with the match offsets; matchStart is npos when `text` is not in it.
SearchHit snippetOf(const std::string& value, const std::string& text, std::size_t context = 40);

} // namespace ke::chat
