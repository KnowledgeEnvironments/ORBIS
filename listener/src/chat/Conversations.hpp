// What a Principal has been talking about: the rows of `history {conversations true}`, the
// directives a person can type, and how close a conversation is to being compressed. Pure.
#pragma once

#include "model/Event.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ke::chat {

struct ConversationRow {
    std::string thread;
    std::string route;
    std::string title;
    std::string firstAt;
    std::string lastAt;
    long long lastSeq = 0;
    long messages = 0;
    long tokensIn = 0;
    long tokensOut = 0;
    bool compressed = false;
    std::vector<std::string> participants;
};

std::vector<ConversationRow> parseConversations(const json& result);
// Title and participant names, ignoring case; an empty query matches everything.
bool conversationMatches(const ConversationRow& row, std::string_view query);

struct Directive {
    const char* word;
    const char* help;
};
inline constexpr Directive kDirectives[] = {
    {"/new", "close this conversation; the next line starts fresh"},
    {"/compress", "compress this conversation now"},
    {"/resume", "reopen a thread: /resume <id>"},
};
// The directives a whole line so far can still become; empty once the line has a space.
std::vector<Directive> directiveMatches(std::string_view line);

// The world's default when a Principal names no `serialize/compress-at`: 70 percent of the budget.
long compressAt(long configured, long budgetTokens);
// Close enough to compression to say so.
bool nearCompression(long contextTokens, long threshold);

// How full the Model's window is, for the gauge by the input line. No window, no gauge: a Model that
// has not said gets its tokens and no percentage, rather than one against an invented size.
struct ContextGauge {
    long used = 0, window = 0, compressAt = 0;
    float fill = 0.0f; // of the window, held to 1
    float mark = 0.0f; // where compression starts
    bool known = false, near = false;
};
ContextGauge contextGauge(long contextTokens, long compressAtTokens, long windowTokens);

} // namespace ke::chat
