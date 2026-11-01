// A Serialization as a person reads it: the exact text a model was sent, cut at the markers the
// serializer writes, with the bytes of each section. Pure.
#pragma once

#include "model/Event.hpp"

#include <string>
#include <vector>

namespace ke::chat {

struct PromptSection {
    std::string name;  // identity, acting, instructions, memory, thread, objects, message
    std::string role;  // system, user, assistant
    std::string text;
    std::size_t bytes() const { return text.size(); }
};

// `serialized` is the Serialization's bytes: an openai-chat array, whole or cut off by a read page,
// or any other text as one section.
std::vector<PromptSection> promptSections(const std::string& serialized);

} // namespace ke::chat
