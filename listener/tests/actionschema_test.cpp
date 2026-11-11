// The create-Principal dialog and the action menus are built from the world's `action/schema`
// Fields (Malli forms as EDN text). The parser has to read the shipped schemas as they are, keep
// optional args optional, and turn the form's values into the args JSON `act` expects.
#include "ui/ActionSchema.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke::ui;

namespace {

const char* kCreate =
    "[:map [:name :string] [:charter :string] [:driven-by {:optional true} [:enum \"process\" \"person\"]] "
    "[:model {:optional true} [:re \"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$\"]] "
    "[:routes-from {:optional true} [:re \"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$\"]] "
    "[:grant [:map [:scope [:or [:enum \"everything\"] [:map [:prefix :string]] "
    "[:vector [:re \"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$\"]]]] "
    "[:ops [:vector [:enum \"read\" \"reference\" \"transmit\" \"write\" \"delegate\" \"evaluate\" \"create\"]]]]] "
    "[:actions {:optional true} [:vector :string]]]";

const char* kRun = "[:map [:adapter :string] [:cmd :string] [:cwd {:optional true} :string] [:timeout-s {:optional true} :int]]";

const ArgSpec* find(const std::vector<ArgSpec>& specs, const char* name) {
    for (const ArgSpec& a : specs) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("edn: vectors, maps, keywords, strings with escapes, numbers, booleans", "[actionschema][edn]") {
    Edn e;
    std::string err;
    REQUIRE(parseEdn("[:map {:closed true} [:n :int] [:s \"a \\\"q\\\" b\"] [:k :foo/bar] [:v -12] [:b false] nil]", e, err));
    REQUIRE(e.kind == Edn::Vector);
    REQUIRE(e.items.size() == 8);
    CHECK(e.items[0].isKeyword("map"));
    CHECK(e.items[1].kind == Edn::Map);
    CHECK(e.items[1].get("closed") != nullptr);
    CHECK(e.items[1].get("closed")->boolean);
    CHECK(e.items[3].items[1].text == "a \"q\" b");
    CHECK(e.items[4].items[1].text == "foo/bar");
    CHECK(e.items[5].items[1].kind == Edn::Number);
    CHECK(e.items[5].items[1].text == "-12");
    CHECK(e.items[6].items[1].kind == Edn::Bool);
    CHECK(e.items[7].kind == Edn::Nil);
    CHECK_FALSE(parseEdn("[:map [:n :int]", e, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("specs: the principal.create schema as shipped", "[actionschema]") {
    std::string err;
    const std::vector<ArgSpec> specs = specsFromSchema(kCreate, err);
    REQUIRE(err.empty());
    REQUIRE(specs.size() == 7);
    CHECK(find(specs, "name")->kind == ArgSpec::Text);
    CHECK_FALSE(find(specs, "name")->optional);
    CHECK(find(specs, "charter")->kind == ArgSpec::Multiline);
    const ArgSpec* driven = find(specs, "driven-by");
    REQUIRE(driven != nullptr);
    CHECK(driven->kind == ArgSpec::Enum);
    CHECK(driven->optional);
    CHECK(driven->values == std::vector<std::string>{"process", "person"});
    CHECK(find(specs, "model")->kind == ArgSpec::Id);
    CHECK(find(specs, "model")->optional);
    const ArgSpec* grant = find(specs, "grant");
    REQUIRE(grant != nullptr);
    CHECK(grant->kind == ArgSpec::Nested);
    REQUIRE(grant->children.size() == 2);
    const ArgSpec& scope = grant->children[0];
    CHECK(scope.kind == ArgSpec::Choice);
    REQUIRE(scope.children.size() == 3);
    CHECK(scope.children[0].name == "everything");
    CHECK(scope.children[1].name == "prefix");
    CHECK(scope.children[1].kind == ArgSpec::Nested);
    CHECK(scope.children[2].name == "ids");
    CHECK(scope.children[2].kind == ArgSpec::IdList);
    const ArgSpec& ops = grant->children[1];
    CHECK(ops.kind == ArgSpec::EnumList);
    CHECK(ops.values.size() == 7);
    CHECK(find(specs, "actions")->kind == ArgSpec::StringList);
}

TEST_CASE("values: defaults in, args out, optionals omitted when empty", "[actionschema]") {
    std::string err;
    const std::vector<ArgSpec> specs = specsFromSchema(kCreate, err);
    REQUIRE(err.empty());
    FormValues v;
    applyDefaults(specs, v,
                  json{{"name", "atlas"},
                       {"charter", "owns the atlas tree"},
                       {"routes-from", "034968ad-3358-4e57-a31b-d1bde0c15b31"},
                       {"grant", {{"scope", {{"prefix", "atlas"}}}, {"ops", json::array({"read", "write", "evaluate"})}}}});
    const json args = argsFromValues(specs, v);
    CHECK(args["name"] == "atlas");
    CHECK(args["charter"] == "owns the atlas tree");
    CHECK(args["routes-from"] == "034968ad-3358-4e57-a31b-d1bde0c15b31");
    CHECK_FALSE(args.contains("driven-by")); // optional, untouched
    CHECK_FALSE(args.contains("model"));
    CHECK_FALSE(args.contains("actions"));
    REQUIRE(args.contains("grant"));
    CHECK(args["grant"]["scope"] == json{{"prefix", "atlas"}});
    CHECK(args["grant"]["ops"] == json::array({"read", "write", "evaluate"}));
    // switch the scope to everything
    v.choice["grant.scope"] = 0;
    CHECK(argsFromValues(specs, v)["grant"]["scope"] == "everything");
    // a list of ids typed as text
    v.choice["grant.scope"] = 2;
    v.listText["grant.scope#2"] = "aaaaaaaa-0000-4000-8000-000000000001, bbbbbbbb-0000-4000-8000-000000000002";
    const json ids = argsFromValues(specs, v)["grant"]["scope"];
    REQUIRE(ids.is_array());
    CHECK(ids.size() == 2);
}

TEST_CASE("values: task.run with an int typed as text; a bad int is left out", "[actionschema]") {
    std::string err;
    const std::vector<ArgSpec> specs = specsFromSchema(kRun, err);
    REQUIRE(err.empty());
    REQUIRE(specs.size() == 4);
    CHECK(find(specs, "timeout-s")->kind == ArgSpec::Int);
    FormValues v;
    applyDefaults(specs, v, json{{"adapter", "shell.local"}, {"cmd", "make test-unit"}});
    v.intText["timeout-s"] = "300";
    json args = argsFromValues(specs, v);
    CHECK(args["timeout-s"] == 300);
    CHECK(args["adapter"] == "shell.local");
    CHECK_FALSE(args.contains("cwd"));
    v.intText["timeout-s"] = "soon";
    CHECK_FALSE(argsFromValues(specs, v).contains("timeout-s"));
}

TEST_CASE("specs: not a map schema, or unparsable, yields nothing and says why", "[actionschema]") {
    std::string err;
    CHECK(specsFromSchema("[:vector :string]", err).empty());
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK(specsFromSchema("[:map [:a", err).empty());
    CHECK_FALSE(err.empty());
}
