// Views (sprint 1 L10): a Message of kind "view" names a Serialization Object whose
// `serialization/kind` is "view" and whose Fields say what to draw:
//   view/kind       text | table | tree | graph | image
//   view/title      the block's title
//   view/data       the payload: a map inline, or a ref to a blob Object when it is large
//   view/about      refs the view is about (provenance chips)
//   view/truncated? the producer cut the data
// This file is the pure side: parsing a `read` state into a ViewSpec, the table / tree / graph
// models the drawer needs (sorting, ref detection, TSV), the image bytes, and a cache of views
// keyed by their id (the chat issues the reads, like the MessageCache). No UI, no network.
// A Message that merely tags an Object carrying `view/kind` is treated the same way, so a client
// (a file drop's folder listing) can attach a view without the world's `present`.
//
// Payload shapes (anything else falls back to a text view of the JSON):
//   table  {columns [..], rows [[..]..]} | {rows [{k v}..]} | [[..]..] | [{k v}..]
//   tree   {label, ref?, value?, children [..]} | any JSON (objects and arrays become nodes)
//   graph  {nodes [{id, name?, cls?, driven-by?}..], edges [{from, to, label?, kind?}..]}
//   image  {mime, bytes <base64>} | {mime, blob <ref>}
//   text   {text "..."} | "..." (markdown)
#pragma once

#include "model/Event.hpp"
#include "model/Graph.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ke::chat {

struct ViewSpec {
    std::string id;
    std::string kind;  // text table tree graph image, "" when unknown
    std::string title;
    json data;         // inline payload once known (null while a blob ref is pending)
    std::string blob;  // the blob Object the payload lives in, "" when inline
    std::vector<std::string> about;
    bool truncated = false;
    bool dataReady = false; // data holds the payload (inline, or the blob arrived)
    bool operator==(const ViewSpec&) const = default;
};

// True when a `read` state carries a view (view/kind present).
bool isViewState(const json& state);
// Parse a view Object's state as `read` returns it.
ViewSpec parseView(const std::string& id, const json& state);
// The payload bytes of a blob Object's state: the first bytes-like Field (file/bytes, bytes,
// blob/bytes, serialization/bytes), base64 decoded; empty when none.
std::vector<unsigned char> blobBytes(const json& state);
// A uuid-shaped string (the id grammar every ref uses on the wire).
bool looksLikeRef(std::string_view s);

// ---- table ----------------------------------------------------------------------------------
struct TableModel {
    std::vector<std::string> columns; // labels, as shown
    std::vector<std::string> keys;    // the key an object row is read by (== the label for a plain column)
    std::vector<std::string> aligns;  // "left" | "right" | "center" per column ("" when the view said nothing)
    std::vector<std::vector<json>> rows;
    std::vector<std::size_t> order; // row indices in display order
    int sortCol = -1;
    bool sortAsc = true;

    static TableModel from(const json& data);
    // Sort by a column (numbers numerically, else by text); col -1 restores the source order.
    void sortBy(int col, bool asc);
    // A cell as text: strings as they are, numbers without trailing zeros, refs by their 8-char prefix.
    static std::string cellText(const json& cell);
    // The ref a cell carries ("" when none): a uuid string, or {ref "..."}.
    static std::string cellRef(const json& cell);
    // Tab-separated rows in display order, header first.
    std::string tsv() const;
    std::size_t size() const { return rows.size(); }
};

// ---- tree -----------------------------------------------------------------------------------
struct TreeNode {
    std::string label;
    std::string ref;   // a ref the node names ("" when none)
    std::string value; // a scalar shown after the label ("" when none)
    std::vector<TreeNode> children;
    std::size_t count() const; // nodes in the subtree, itself included
};
TreeNode treeFrom(const json& data);

// ---- graph ----------------------------------------------------------------------------------
// Nodes and edges from the payload, laid out with the layered layout; nodes keep `cls` and
// `drivenBy` so the drawer can style them like the patcher's.
model::GraphModel graphFrom(const json& data);

// ---- image ----------------------------------------------------------------------------------
struct ImageBytes {
    std::string mime;
    std::vector<unsigned char> bytes;
    std::string blob; // when the bytes live in a blob Object
};
ImageBytes imageFrom(const json& data);

// ---- text -----------------------------------------------------------------------------------
std::string textFrom(const json& data);

// ---- cache ----------------------------------------------------------------------------------
enum class ViewState { Unknown, Pending, Ready, Failed };

class ViewCache {
  public:
    // Mark a view id as being read (Unknown -> Pending); false when already known.
    bool request(const std::string& id);
    // The view Object's state arrived. Returns the blob id to read next when the payload is a
    // ref, "" otherwise.
    std::string onRead(const std::string& id, const json& state);
    void onReadFailed(const std::string& id, const std::string& code);
    // A blob Object's state arrived for the views waiting on it.
    void onBlob(const std::string& blob, const json& state);
    void onBlobFailed(const std::string& blob, const std::string& code);
    // The connection dropped: pending reads are lost. Returns the ids to read again (views and blobs).
    std::vector<std::string> resetPending();

    ViewState state(const std::string& id) const;
    const ViewSpec* get(const std::string& id) const; // Ready only
    const std::string* failure(const std::string& id) const;
    // Whether `id` was seen as a non-view Object (so a tagged Object is not re-read every build).
    bool knownNotView(const std::string& id) const;
    unsigned long long version() const { return version_; }
    std::size_t size() const { return entries_.size(); }

  private:
    struct Entry {
        ViewState state = ViewState::Unknown;
        ViewSpec spec;
        std::string error;
        bool notView = false;
    };
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, std::vector<std::string>> blobWaiters_; // blob id -> view ids
    unsigned long long version_ = 0;
};

} // namespace ke::chat
