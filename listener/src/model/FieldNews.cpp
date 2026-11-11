#include "model/FieldNews.hpp"

#include <array>

namespace ke::model {

namespace {

constexpr const char* kSeen = "loop/seen-seq";

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// A write by someone other than this Principal or the world itself.
bool someoneElse(const Principals& fold, const PrincipalInfo& info, const FieldState& f) {
    if (f.by.empty() || f.by == info.id) {
        return false;
    }
    const PrincipalInfo* who = fold.get(f.by);
    return who == nullptr || who->drivenBy != "none";
}

} // namespace

bool aboutThePrincipal(const std::string& field) {
    // What it reads as instructions or knowledge. A setting a person flips, reasoning or a Model, is not
    // an edit waiting for it, and neither are its running figures.
    static constexpr std::array<const char*, 10> notNews{"cost/", "view/", "evaluation/", "notification/", "harness/",
                                                         "loop/", "principal/", "policy/", "serialize/", "model/"};
    for (const char* n : notNews) {
        if (startsWith(field, n)) {
            return false;
        }
    }
    return !field.ends_with("/hint") && !field.ends_with("/comment");
}

FieldNews fieldNews(const Principals& fold, const PrincipalInfo& info, const FieldState& f) {
    if (info.drivenBy != "process" || !aboutThePrincipal(f.name) || !someoneElse(fold, info, f)) {
        return FieldNews::None;
    }
    const auto it = info.fields.find(kSeen);
    if (it == info.fields.end() || !it->second.value.is_number()) {
        return FieldNews::None; // it has not woken yet: everything is in its first prompt, nothing is news
    }
    return f.seq > it->second.value.get<Seq>() ? FieldNews::Waiting : FieldNews::Seen;
}

int waitingEdits(const Principals& fold, const PrincipalInfo& info) {
    if (info.fields.find(kSeen) == info.fields.end()) {
        return 0; // a Principal that never woke has nothing to catch up on
    }
    int n = 0;
    for (const auto& [name, f] : info.fields) {
        n += fieldNews(fold, info, f) == FieldNews::Waiting ? 1 : 0;
    }
    return n;
}

} // namespace ke::model
