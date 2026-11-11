// A form built from an action's argument schema (sprint 1 L6): the world stores each named
// action's args as a Malli vector form in EDN text (`action/schema`), e.g.
//
//   [:map [:name :string] [:charter :string] [:driven-by {:optional true} [:enum "process" "person"]]
//         [:model {:optional true} [:re "^[0-9a-f]{8}-..."]]
//         [:grant [:map [:scope [:or [:enum "everything"] [:map [:prefix :string]] [:vector [:re ...]]]]
//                       [:ops [:vector [:enum "read" "reference" ...]]]]]]
//
// The listener parses just enough EDN to know each arg's name, whether it is optional, and its
// kind (text, int, bool, enum, id, list of enum, list of ids, list of strings, nested map, a
// choice of variants), draws widgets for them, and produces the args JSON. A schema it cannot
// read yields an empty form and the caller falls back to a hand-made one. No UI in the parser,
// so it is tested without ImGui.
#pragma once

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ke::ui {

using json = nlohmann::json;

// ---- EDN, the little we need ----------------------------------------------------------------
struct Edn {
    enum Kind { Nil, Bool, Number, String, Keyword, Symbol, Vector, List, Map, Set } kind = Nil;
    std::string text; // String, Keyword (without the colon), Symbol, Number (as written)
    bool boolean = false;
    std::vector<Edn> items;                     // Vector, List, Set
    std::vector<std::pair<Edn, Edn>> entries;   // Map
    const Edn* get(const std::string& keyword) const; // Map lookup by keyword key
    bool isKeyword(const char* k) const { return kind == Keyword && text == k; }
};
// Parses one EDN value; false (and `err`) on a syntax error.
bool parseEdn(const std::string& src, Edn& out, std::string& err);

// ---- the form model -------------------------------------------------------------------------
struct ArgSpec {
    enum Kind { Text, Multiline, Int, Bool, Enum, Id, EnumList, IdList, StringList, Nested, Choice, Unknown } kind = Unknown;
    std::string name;
    bool optional = false;
    std::vector<std::string> values;            // Enum, EnumList
    std::vector<ArgSpec> children;              // Nested: its args; Choice: its variants (names "everything"/"prefix"/"ids" when recognisable)
    std::string hint;                           // shown dim beside the widget
};

// `[:map ...]` -> its args; an empty vector when the form is not a map schema.
std::vector<ArgSpec> specsFromSchema(const std::string& edn, std::string& err);
// One arg spec from a Malli type form (exposed for tests).
ArgSpec specFromType(const std::string& name, const Edn& type, bool optional);

// ---- values -----------------------------------------------------------------------------------
// The form's state: a value per arg (nested by name), plus which Choice variant is active.
struct FormValues {
    json values = json::object();             // name -> value (nested maps for Nested/Choice)
    std::map<std::string, int> choice;          // Choice arg path -> variant index
    std::map<std::string, std::string> listText; // IdList/StringList typed as comma text, by path
    std::map<std::string, std::string> intText;  // Int typed as text, by path
};
// The args JSON for `act`: optional args left empty are omitted; EnumList/IdList/StringList as
// arrays; Int parsed; a Choice contributes its active variant's value.
json argsFromValues(const std::vector<ArgSpec>& specs, const FormValues& v, const std::string& path = "");
// Fill defaults: `defaults` are {name: value} applied where the form has nothing yet.
void applyDefaults(const std::vector<ArgSpec>& specs, FormValues& v, const json& defaults);
// The stored value at a dotted path (created as an object chain when absent), for the widgets.
json* valueSlot(FormValues& v, const std::string& path);

} // namespace ke::ui
