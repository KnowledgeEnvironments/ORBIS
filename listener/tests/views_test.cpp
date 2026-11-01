// Views (sprint 1 L10) and attachments (L8), the pure side: parsing a view Object's state, the
// table model (shapes, sorting, refs, TSV), the tree and graph payloads, image bytes, the view
// cache with a blob payload, base64 and sha256, and the attachment classification.
#include "chat/Attach.hpp"
#include "chat/Views.hpp"
#include "util/Base64.hpp"
#include "util/Sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace ke;
using namespace ke::chat;

namespace {

constexpr const char* kView = "5d1f0a9e-7c2b-4e1a-9f00-000000000001";
constexpr const char* kBlob = "5d1f0a9e-7c2b-4e1a-9f00-000000000002";
constexpr const char* kRoot = "034968ad-3358-4e57-a31b-d1bde0c15b31";
constexpr const char* kAtlas = "9b60b24f-1111-4000-8000-000000000001";

json fieldState(json fields) {
    json f = json::object();
    for (auto& [k, v] : fields.items()) {
        f[k] = json{{"field/value", v}, {"field/type", "string"}};
    }
    return json{{"object/id", kView}, {"object/class", "serialization"}, {"object/fields", f}};
}

} // namespace

TEST_CASE("a view Object's state parses into a spec", "[views]") {
    const json state = fieldState(json{{"serialization/kind", "view"},
                                       {"view/kind", "table"},
                                       {"view/title", "Routes"},
                                       {"view/data", json{{"columns", {"name", "members"}}, {"rows", {{"root.route", 2}}}}},
                                       {"view/about", json::array({kRoot})},
                                       {"view/truncated?", true}});
    REQUIRE(isViewState(state));
    const ViewSpec v = parseView(kView, state);
    CHECK(v.kind == "table");
    CHECK(v.title == "Routes");
    CHECK(v.dataReady);
    CHECK(v.blob.empty());
    CHECK(v.truncated);
    REQUIRE(v.about.size() == 1);
    CHECK(v.about[0] == kRoot);
    CHECK(v.data["columns"].size() == 2);

    SECTION("a JSON string payload is parsed; a ref payload is a blob to read") {
        const ViewSpec s1 = parseView(kView, fieldState(json{{"view/kind", "tree"}, {"view/data", "{\"label\":\"x\"}"}}));
        CHECK(s1.dataReady);
        CHECK(s1.data["label"] == "x");
        const ViewSpec s2 = parseView(kView, fieldState(json{{"view/kind", "image"}, {"view/data", kBlob}}));
        CHECK_FALSE(s2.dataReady);
        CHECK(s2.blob == kBlob);
    }
    SECTION("not a view") {
        CHECK_FALSE(isViewState(fieldState(json{{"message/text", "hi"}})));
    }
}

TEST_CASE("the table model takes every payload shape and sorts", "[views][table]") {
    SECTION("columns + rows of arrays") {
        // rows as an explicit array of arrays (an initializer list of string+number pairs would read as an object)
        const json rows = json::array({json::array({"b", 20}), json::array({"a", 3}), json::array({"c", 100})});
        const TableModel t = TableModel::from(json{{"columns", {"n", "size"}}, {"rows", rows}});
        REQUIRE(t.columns == std::vector<std::string>{"n", "size"});
        REQUIRE(t.size() == 3);
        CHECK(t.order == std::vector<std::size_t>{0, 1, 2});
        TableModel s = t;
        s.sortBy(1, true);
        CHECK(s.order == std::vector<std::size_t>{1, 0, 2}); // 3, 20, 100: numeric, not lexical
        s.sortBy(1, false);
        CHECK(s.order == std::vector<std::size_t>{2, 0, 1});
        s.sortBy(0, true);
        CHECK(s.order == std::vector<std::size_t>{1, 0, 2}); // a, b, c
        s.sortBy(-1, true);
        CHECK(s.order == std::vector<std::size_t>{0, 1, 2});
        CHECK(s.sortCol == -1);
    }
    SECTION("rows of objects: columns are the union in first-seen order") {
        const TableModel t = TableModel::from(json{{"rows", {json{{"a", 1}, {"b", 2}}, json{{"b", 3}, {"c", 4}}}}});
        CHECK(t.columns == std::vector<std::string>{"a", "b", "c"});
        REQUIRE(t.size() == 2);
        CHECK(t.rows[1][0].is_null());
        CHECK(t.rows[1][2] == 4);
    }
    SECTION("a bare array of arrays names its columns col N") {
        const TableModel t = TableModel::from(json::array({{1, 2, 3}, {4, 5}}));
        CHECK(t.columns == std::vector<std::string>{"col 1", "col 2", "col 3"});
        CHECK(t.rows[1][2].is_null());
    }
    SECTION("refs are detected in strings and {ref name} cells") {
        CHECK(TableModel::cellRef(json(kRoot)) == kRoot);
        CHECK(TableModel::cellRef(json{{"ref", kAtlas}, {"name", "atlas"}}) == kAtlas);
        CHECK(TableModel::cellRef(json("root")).empty());
        CHECK(TableModel::cellRef(json(42)).empty());
        CHECK(TableModel::cellText(json(kRoot)) == "034968ad");
        CHECK(TableModel::cellText(json{{"ref", kAtlas}, {"name", "atlas"}}) == "atlas");
        CHECK(TableModel::cellText(json(2.5)) == "2.5");
        CHECK(TableModel::cellText(json(7)) == "7");
        CHECK(TableModel::cellText(json(nullptr)).empty());
    }
    SECTION("TSV follows the display order") {
        const json rows = json::array({json::array({"x", 2}), json::array({"y\tz", 1})});
        TableModel t = TableModel::from(json{{"columns", {"n", "v"}}, {"rows", rows}});
        t.sortBy(1, true);
        CHECK(t.tsv() == "n\tv\ny z\t1\nx\t2\n");
    }
}

TEST_CASE("the tree payload becomes nodes, explicit or from plain JSON", "[views][tree]") {
    const TreeNode explicitTree = treeFrom(json{{"label", "atlas/"},
                                                {"children",
                                                 {json{{"label", "src/"}, {"children", {json{{"label", "main.cpp"}, {"value", "4 KiB"}}}}},
                                                  json{{"label", "README.md"}, {"ref", kAtlas}}}}});
    CHECK(explicitTree.label == "atlas/");
    REQUIRE(explicitTree.children.size() == 2);
    CHECK(explicitTree.children[0].children[0].value == "4 KiB");
    CHECK(explicitTree.children[1].ref == kAtlas);
    CHECK(explicitTree.count() == 4);
    const TreeNode plain = treeFrom(json{{"a", {1, 2}}, {"b", json{{"c", kRoot}}}});
    REQUIRE(plain.children.size() == 2);
    CHECK(plain.children[0].label == "a");
    CHECK(plain.children[0].children.size() == 2);
    CHECK(plain.children[1].children[0].ref == kRoot);
}

TEST_CASE("the graph payload lays out nodes and keeps edges whole", "[views][graph]") {
    const model::GraphModel g = graphFrom(json{{"nodes", {json{{"id", kRoot}, {"name", "root"}, {"cls", "principal"}, {"driven-by", "process"}},
                                                          json{{"id", kAtlas}, {"name", "atlas"}, {"cls", "principal"}}}},
                                               {"edges", {json{{"from", kRoot}, {"to", kAtlas}, {"label", "root.atlas"}},
                                                          json{{"from", kAtlas}, {"to", "shell.local"}, {"kind", "adapter"}}}}});
    REQUIRE(g.nodes.size() == 3); // shell.local was named by an edge only
    CHECK(g.node(kRoot)->drivenBy == "process");
    CHECK(g.node("shell.local")->name == "shell.local");
    REQUIRE(g.edges.size() == 2);
    CHECK(g.edges[1].kind == model::EdgeKind::Adapter);
    // laid out: no two nodes share a position
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        for (std::size_t j = i + 1; j < g.nodes.size(); ++j) {
            CHECK_FALSE((g.nodes[i].x == g.nodes[j].x && g.nodes[i].y == g.nodes[j].y));
        }
    }
    CHECK(graphFrom(json("nope")).nodes.empty());
}

TEST_CASE("image and text payloads", "[views]") {
    const std::string png = "\x89PNG\r\n\x1a\n";
    const ImageBytes img = imageFrom(json{{"mime", "image/png"}, {"bytes", util::base64Encode(png)}});
    CHECK(img.mime == "image/png");
    CHECK(std::string(img.bytes.begin(), img.bytes.end()) == png);
    CHECK(imageFrom(json{{"blob", kBlob}}).blob == kBlob);
    CHECK(textFrom(json{{"text", "# hi"}}) == "# hi");
    CHECK(textFrom(json("plain")) == "plain");
}

TEST_CASE("the view cache reads once and waits for a blob payload", "[views][cache]") {
    ViewCache c;
    CHECK(c.request(kView));
    CHECK_FALSE(c.request(kView));
    CHECK(c.state(kView) == ViewState::Pending);
    const std::string blob = c.onRead(kView, fieldState(json{{"view/kind", "table"}, {"view/title", "big"}, {"view/data", kBlob}}));
    CHECK(blob == kBlob);
    CHECK(c.state(kView) == ViewState::Pending);
    CHECK(c.get(kView) == nullptr);
    const std::string payload = R"({"columns":["a"],"rows":[[1],[2]]})";
    json blobState = json{{"object/id", kBlob}, {"object/fields", json{{"file/bytes", json{{"field/value", util::base64Encode(payload)}}}}}};
    c.onBlob(kBlob, blobState);
    REQUIRE(c.get(kView) != nullptr);
    CHECK(c.get(kView)->dataReady);
    CHECK(c.get(kView)->data["rows"].size() == 2);
    SECTION("an Object that is not a view is remembered as such") {
        ViewCache d;
        d.request(kRoot);
        CHECK(d.onRead(kRoot, fieldState(json{{"message/text", "x"}})).empty());
        CHECK(d.knownNotView(kRoot));
        CHECK_FALSE(d.request(kRoot));
    }
    SECTION("a dropped connection re-reads pending views and blobs") {
        ViewCache d;
        d.request(kView);
        d.onRead(kView, fieldState(json{{"view/kind", "tree"}, {"view/data", kBlob}}));
        const std::vector<std::string> again = d.resetPending();
        REQUIRE(again.size() == 1);
        CHECK(again[0] == kBlob);
    }
}

TEST_CASE("base64 and sha256 agree with the known vectors", "[util]") {
    CHECK(util::base64Encode(std::string_view("Man")) == "TWFu");
    CHECK(util::base64Encode(std::string_view("Ma")) == "TWE=");
    CHECK(util::base64Encode(std::string_view("M")) == "TQ==");
    std::vector<unsigned char> out;
    REQUIRE(util::base64Decode("TWFu", out));
    CHECK(std::string(out.begin(), out.end()) == "Man");
    REQUIRE(util::base64Decode("TQ==\n", out));
    CHECK(std::string(out.begin(), out.end()) == "M");
    CHECK_FALSE(util::base64Decode("T*==", out));
    CHECK(util::Sha256::hex(std::string_view("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(util::Sha256::hex(std::string_view("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string million(1000000, 'a');
    CHECK(util::Sha256::hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("attachments are classified: text, bytes, caps, folders", "[attach]") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "orbis-attach-test";
    std::filesystem::create_directories(dir / "sub");
    {
        std::ofstream(dir / "notes.md") << "# hello\n\nworld\n";
        std::ofstream(dir / "blob.bin", std::ios::binary) << std::string("\x00\x01\x02\xff", 4);
        std::ofstream(dir / "sub" / "x.txt") << "x";
    }
    const Attachment md = classify((dir / "notes.md").string());
    CHECK(md.refusal.empty());
    CHECK(md.text);
    CHECK(md.mime == "text/markdown");
    CHECK(md.size == 15);
    CHECK(md.sha256 == util::Sha256::hex(std::string_view("# hello\n\nworld\n")));
    CHECK(createFields(md)["file/name"] == "notes.md");
    CHECK(contentField(md).field == "file/text");
    CHECK(contentField(md).type == "string");
    const Attachment bin = classify((dir / "blob.bin").string());
    CHECK_FALSE(bin.text);
    CHECK(bin.mime == "application/octet-stream");
    CHECK(contentField(bin).field == "file/bytes");
    CHECK(contentField(bin).type == "bytes");
    CHECK(contentField(bin).value == util::base64Encode(std::string("\x00\x01\x02\xff", 4)));
    const Attachment folder = classify(dir.string());
    CHECK(folder.directory);
    CHECK(classify((dir / "missing").string()).refusal == "not a regular file");
    CHECK_FALSE(isText(std::string("a\0b", 3)));
    CHECK(isText("héllo"));
    CHECK(mimeOf("a.PNG") == "image/png");
    SECTION("the listing is a tree view payload, dirs first, two levels") {
        bool truncated = false;
        const json tree = folderTree(dir.string(), 40, truncated);
        CHECK_FALSE(truncated);
        CHECK(tree["label"] == "orbis-attach-test/");
        REQUIRE(tree["children"].size() == 3);
        CHECK(tree["children"][0]["label"] == "sub/");
        CHECK(tree["children"][0]["children"][0]["label"] == "x.txt");
        const json fields = folderViewFields(dir.string(), tree, truncated);
        CHECK(fields["view/kind"] == "tree");
        CHECK(fields["view/data"]["label"] == "orbis-attach-test/");
        bool trunc2 = false;
        const json cut = folderTree(dir.string(), 1, trunc2);
        CHECK(trunc2);
        CHECK(cut["children"].size() == 2); // one entry + the "… N more" marker
    }
    SECTION("the cap refuses a big file") {
        const std::filesystem::path big = dir / "big.bin";
        {
            std::ofstream out(big, std::ios::binary);
            std::string chunk(1024 * 1024, 'z');
            for (int i = 0; i < 9; ++i) {
                out << chunk;
            }
        }
        const Attachment a = classify(big.string());
        CHECK(a.refusal.rfind("too big", 0) == 0);
        std::filesystem::remove(big);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("views: table columns may be {key label align} objects (view.table-of, cost.report)", "[views]") {
    const ke::json data = {{"columns", ke::json::array({ke::json{{"key", "principal"}, {"label", "principal"}},
                                                      ke::json{{"key", "tokens-in"}, {"label", "tokens in"}, {"align", "right"}}})},
                           {"rows", ke::json::array({ke::json::array({"root", 11892}),
                                                     ke::json{{"principal", "person"}, {"tokens-in", 12}}})}};
    const ke::chat::TableModel t = ke::chat::TableModel::from(data);
    REQUIRE(t.columns.size() == 2);
    CHECK(t.columns[1] == "tokens in"); // the label, never the JSON of the column
    CHECK(t.keys[1] == "tokens-in");
    CHECK(t.aligns[1] == "right");
    REQUIRE(t.rows.size() == 2);
    CHECK(t.rows[0][1] == 11892);
    CHECK(t.rows[1][1] == 12); // an object row is read by the key
}
