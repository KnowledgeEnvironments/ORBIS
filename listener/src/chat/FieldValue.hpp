// What the NEW FIELD row knows, pure so it can be tested: the Field types a person may declare,
// a typed value checked before it is sent, and what a Field's name means to the world.
#pragma once

#include "chat/Symbols.hpp"
#include "model/Event.hpp"

#include <string>
#include <utility>
#include <string_view>

namespace ke::chat {

inline constexpr const char* kFieldTypes[] = {"string", "number", "boolean", "keyword", "instant", "ref", "list", "map"};
inline constexpr int kFieldTypeCount = 8;

struct ParsedField {
    bool ok = false;
    json value;
    json type;         // the declared type: "number", ["ref", "route"], ["list", "string"]
    std::string error; // why not, for a dim note under the box
};

// `text` as a value of `kind` (one of kFieldTypes). A ref takes an id, a unique prefix or a name,
// with or without a leading `@`; list and map take JSON.
ParsedField parseFieldValue(std::string_view kind, const std::string& text, const SymbolTable* table);

// What the world does with a Field of this name: `instructions/` joins the system prompt and so
// on. Empty when the name means nothing special.
std::string fieldNameHint(std::string_view name);

// A Field's declared type as text, and its head (`[:list :number]` is "list").
std::string typeName(const json& type);
std::string scalarName(const json& type);
// Text typed into a value box, as the Field's type wants it, and back.
json valueFor(const json& type, const std::string& text);
std::string editText(const json& value);
// `<field>/hint` and `<field>/comment` are facets of `<field>`: the base name and the facet, or "".
std::pair<std::string, std::string> facetOf(const std::string& name);

// What the model will do when nobody asks: an explicit Field, else the Model's, else off.
std::string reasoningShown(const std::string& loopMode, const std::string& modelMode);

} // namespace ke::chat
