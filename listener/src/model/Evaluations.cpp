#include "model/Evaluations.hpp"

#include "model/Snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ke::model {

namespace {

std::string str(const json& j, const char* key) {
    if (!j.is_object()) {
        return "";
    }
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

void noteImages(EvaluationInfo& info, const json& value) {
    info.imagesSent = info.imagesHeld = 0;
    if (value.is_array()) {
        for (const json& img : value) {
            if (img.is_object() && img.value("sent", false)) {
                ++info.imagesSent;
            } else {
                ++info.imagesHeld;
            }
        }
    }
}

long long num(const json& j, const char* key, long long fallback = -1) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? static_cast<long long>(it->get<double>()) : fallback;
}

} // namespace

bool Evaluations::apply(const Event& e) {
    if (e.seq <= applied_) {
        return false;
    }
    applied_ = e.seq;
    const json& b = e.body;
    if (e.kind == "created") {
        const std::string cls = str(b, "class");
        const json fields = b.is_object() && b.contains("fields") && b["fields"].is_object() ? b["fields"] : json::object();
        if (cls == "evaluation") {
            EvaluationInfo& v = evaluations_[e.stream];
            v.id = e.stream;
            v.principal = fields.contains("evaluation/initiator") && fields["evaluation/initiator"].is_string()
                              ? fields["evaluation/initiator"].get<std::string>()
                              : e.actor;
            v.model = str(fields, "evaluation/model");
            v.state = str(fields, "evaluation/state");
            v.created = e.seq;
            v.last = e.seq;
            v.at = e.at;
            order_.push_back(e.stream);
            indexMessage(e.stream, str(fields, "evaluation/message"));
            v.wake = str(fields, "evaluation/wake");
            return true;
        }
        if (cls == "serialization") {
            SerializationInfo& s = serializations_[e.stream];
            s.id = e.stream;
            s.created = e.seq;
            s.hash = str(fields, "serialization/hash");
            if (fields.contains("serialization/size")) {
                const json& size = fields["serialization/size"];
                s.bytes = size.is_object() ? num(size, "bytes") : (size.is_number() ? static_cast<long long>(size.get<double>()) : -1);
            }
            return true;
        }
        return false;
    }
    if (e.kind == "evaluated") {
        const std::string id = str(b, "evaluation");
        if (id.empty()) {
            return false;
        }
        EvaluationInfo& v = evaluations_[id];
        if (v.id.empty()) { // seen before its `created` (a resumed history): still useful
            v.id = id;
            v.principal = e.actor;
            v.created = e.seq;
            v.at = e.at;
            order_.push_back(id);
        }
        v.last = e.seq;
        const std::string st = str(b, "state");
        if (st == "streaming") {
            if (v.done()) {
                return false; // a late delta after the final event: ignored
            }
            if (v.runningAt.empty()) {
                v.runningAt = e.at;
            }
            v.state = st;
            const long long n = num(b, "seq-in-stream");
            const bool hasPartial = b.is_object() && b.contains("partial") && b["partial"].is_string();
            const bool hasDelta = b.is_object() && b.contains("delta") && b["delta"].is_string();
            const bool hasThinking = b.is_object() && b.contains("thinking-delta") && b["thinking-delta"].is_string();
            const long long thinkingChars = num(b, "thinking-chars");
            if (!hasPartial && n >= 0 && n <= v.streamSeq) {
                return false; // duplicate or out of order: its deltas are already in, its counts cannot be newer
            }
            if (hasPartial) {
                v.partial = b["partial"].get<std::string>();
                v.streamSeq = std::max(v.streamSeq, n);
            } else {
                if (hasDelta) {
                    v.partial += b["delta"].get<std::string>();
                }
                if (hasThinking) {
                    v.thinkingPartial += b["thinking-delta"].get<std::string>();
                }
                v.streamSeq = n >= 0 ? n : v.streamSeq + 1;
            }
            if (thinkingChars >= 0) {
                v.thinkingChars = std::max(v.thinkingChars, thinkingChars);
            } else if (hasThinking) {
                v.thinkingChars = std::max(v.thinkingChars, static_cast<long long>(v.thinkingPartial.size()));
            }
            return true;
        }
        if (!st.empty()) {
            v.state = st;
            if (b.contains("reason") && b["reason"].is_string()) {
                v.reason = b["reason"].get<std::string>();
            }
            if (st == "running" && v.runningAt.empty()) {
                v.runningAt = e.at;
            }
            if (b.is_object() && b.contains("partial") && b["partial"].is_string() && !v.done()) {
                v.partial = b["partial"].get<std::string>();
            }
        }
        if (const std::string eng = str(b, "model"); !eng.empty()) {
            v.model = eng;
        }
        if (const std::string ser = str(b, "serialization"); !ser.empty()) {
            v.serialization = ser;
        }
        if (b.is_object() && b.contains("cost") && b["cost"].is_object()) {
            const long long in = num(b["cost"], "tokens-in");
            const long long out = num(b["cost"], "tokens-out");
            if (in >= 0) {
                v.tokensIn = in;
            }
            if (out >= 0) {
                v.tokensOut = out;
            }
        }
        if (b.is_object() && b.contains("outcome") && b["outcome"].is_object() && b["outcome"].contains("usage") &&
            b["outcome"]["usage"].is_object()) {
            const json& u = b["outcome"]["usage"];
            if (v.tokensIn < 0) {
                v.tokensIn = num(u, "prompt-tokens");
            }
            if (v.tokensOut < 0) {
                v.tokensOut = num(u, "completion-tokens");
            }
        }
        return true;
    }
    if (e.kind == "field-set") {
        auto it = evaluations_.find(e.stream);
        if (it == evaluations_.end()) {
            return false;
        }
        const std::string name = str(b, "name");
        const json value = b.is_object() && b.contains("value") ? b["value"] : json();
        it->second.last = e.seq;
        if (name == "evaluation/ops") {
            it->second.ops = value.is_array() ? value : json::array();
        } else if (name == "evaluation/results") {
            it->second.results = value.is_array() ? value : json::array();
        } else if (name == "evaluation/iteration") {
            it->second.iteration = value.is_number() ? value.get<int>() : 0;
        } else if (name == "evaluation/thinking") {
            it->second.thinking = value;
        } else if (name == "evaluation/message") {
            indexMessage(it->first, value.is_string() ? value.get<std::string>() : std::string());
        } else if (name == "evaluation/wake") {
            it->second.wake = value.is_string() ? value.get<std::string>() : std::string();
        } else if (name == "evaluation/reasoning") {
            if (value.is_string()) {
                it->second.reasoning = value.get<std::string>();
            }
        } else if (name == "evaluation/images") {
            noteImages(it->second, value);
        } else if (name == "evaluation/context") {
            if (value.is_object()) {
                it->second.ctxBytes = num(value, "serialization-bytes");
                it->second.ctxIn = num(value, "prompt-tokens");
                it->second.ctxOut = num(value, "completion-tokens");
                it->second.ctxObjects = num(value, "objects");
                if (value.contains("reasoning") && value["reasoning"].is_string()) {
                    it->second.reasoning = value["reasoning"].get<std::string>();
                }
                if (it->second.tokensIn < 0) {
                    it->second.tokensIn = it->second.ctxIn;
                }
                if (it->second.tokensOut < 0) {
                    it->second.tokensOut = it->second.ctxOut;
                }
            }
        } else {
            return false;
        }
        return true;
    }
    return false;
}

void Evaluations::seed(const SnapshotObject& o) {
    if (o.cls == "serialization") {
        SerializationInfo& s = serializations_[o.id];
        s.id = o.id;
        s.created = o.created;
        if (const FieldState* f = o.field("serialization/hash"); f && f->value.is_string()) {
            s.hash = f->value.get<std::string>();
        }
        if (const FieldState* f = o.field("serialization/size")) {
            s.bytes = f->value.is_object() ? num(f->value, "bytes")
                      : f->value.is_number() ? static_cast<long long>(f->value.get<double>())
                                             : -1;
        }
        return;
    }
    if (o.cls != "evaluation") {
        return;
    }
    totalsCache_.clear();
    EvaluationInfo& v = evaluations_[o.id];
    const bool fresh = v.id.empty();
    v.id = o.id;
    v.created = o.created;
    v.last = o.created;
    v.at = o.createdAt;
    if (fresh) {
        order_.push_back(o.id);
    }
    auto strField = [&](const char* name) -> std::string {
        const FieldState* f = o.field(name);
        return f && f->value.is_string() ? f->value.get<std::string>() : std::string();
    };
    v.principal = strField("evaluation/initiator");
    if (v.principal.empty()) {
        v.principal = o.actor;
    }
    v.model = strField("evaluation/model");
    v.serialization = strField("evaluation/serialization");
    indexMessage(o.id, strField("evaluation/message"));
    v.wake = strField("evaluation/wake");
    v.state = strField("evaluation/state");
    if (v.state.empty()) {
        v.state = "done";
    }
    if (const FieldState* f = o.field("evaluation/ops")) {
        v.ops = f->value.is_array() ? f->value : json::array();
        v.last = std::max(v.last, f->seq);
    }
    if (const FieldState* f = o.field("evaluation/results")) {
        v.results = f->value.is_array() ? f->value : json::array();
        v.last = std::max(v.last, f->seq);
    }
    if (const FieldState* f = o.field("evaluation/iteration"); f && f->value.is_number()) {
        v.iteration = f->value.get<int>();
    }
    if (const FieldState* f = o.field("evaluation/reasoning"); f && f->value.is_string()) {
        v.reasoning = f->value.get<std::string>();
    }
    if (const FieldState* f = o.field("evaluation/thinking")) {
        v.thinking = f->value;
    }
    if (const FieldState* f = o.field("evaluation/images")) {
        noteImages(v, f->value);
    }
    if (const FieldState* f = o.field("evaluation/context"); f && f->value.is_object()) {
        v.ctxBytes = num(f->value, "serialization-bytes");
        v.ctxIn = num(f->value, "prompt-tokens");
        v.ctxOut = num(f->value, "completion-tokens");
        v.ctxObjects = num(f->value, "objects");
        if (f->value.contains("reasoning") && f->value["reasoning"].is_string()) {
            v.reasoning = f->value["reasoning"].get<std::string>();
        }
        v.tokensIn = v.ctxIn;
        v.tokensOut = v.ctxOut;
        v.last = std::max(v.last, f->seq);
    }
    if (const FieldState* f = o.field("evaluation/cost"); f && f->value.is_object()) {
        if (const long long in = num(f->value, "tokens-in"); in >= 0) {
            v.tokensIn = in;
        }
        if (const long long out = num(f->value, "tokens-out"); out >= 0) {
            v.tokensOut = out;
        }
    }
}

const EvaluationInfo* Evaluations::get(std::string_view id) const {
    auto it = evaluations_.find(std::string(id));
    return it == evaluations_.end() ? nullptr : &it->second;
}

const SerializationInfo* Evaluations::serialization(std::string_view id) const {
    auto it = serializations_.find(std::string(id));
    return it == serializations_.end() ? nullptr : &it->second;
}

void Evaluations::indexMessage(const std::string& evaluation, const std::string& message) {
    EvaluationInfo& v = evaluations_[evaluation];
    if (message.empty() || v.message == message) {
        return;
    }
    if (!v.message.empty()) {
        auto& old = byMessage_[v.message];
        old.erase(std::remove(old.begin(), old.end(), evaluation), old.end());
    }
    v.message = message;
    auto& ids = byMessage_[message];
    if (std::find(ids.begin(), ids.end(), evaluation) == ids.end()) {
        ids.push_back(evaluation);
    }
}

std::vector<const EvaluationInfo*> Evaluations::forMessage(std::string_view message, std::string_view principal) const {
    std::vector<const EvaluationInfo*> out;
    auto it = byMessage_.find(std::string(message));
    if (it == byMessage_.end()) {
        return out;
    }
    for (const std::string& id : it->second) {
        const EvaluationInfo& v = evaluations_.at(id);
        if (principal.empty() || v.principal == principal) {
            out.push_back(&v);
        }
    }
    std::sort(out.begin(), out.end(), [](const EvaluationInfo* a, const EvaluationInfo* b) { return a->created < b->created; });
    return out;
}

std::vector<const EvaluationInfo*> Evaluations::between(std::string_view principal, Seq after, Seq upto) const {
    std::vector<const EvaluationInfo*> out;
    for (const std::string& id : order_) {
        const EvaluationInfo& v = evaluations_.at(id);
        if (v.principal == principal && v.created > after && v.created <= upto) {
            out.push_back(&v);
        }
    }
    return out;
}

Totals Evaluations::totals(std::string_view principal) const {
    if (auto it = totalsCache_.find(std::string(principal)); it != totalsCache_.end() && it->second.first == applied_) {
        return it->second.second;
    }
    Totals t;
    for (const auto& [id, v] : evaluations_) {
        if (v.principal != principal) {
            continue;
        }
        ++t.evaluations;
        if (v.tokensIn >= 0 || v.tokensOut >= 0) {
            ++t.withUsage;
            t.tokensIn += std::max(0LL, v.tokensIn);
            t.tokensOut += std::max(0LL, v.tokensOut);
        }
        if (v.ctxBytes >= 0 || v.ctxObjects >= 0) {
            ++t.withContext;
            t.ctxBytes += std::max(0LL, v.ctxBytes);
            t.ctxObjects += std::max(0LL, v.ctxObjects);
        }
    }
    totalsCache_[std::string(principal)] = {applied_, t};
    return t;
}

const EvaluationInfo* Evaluations::latest(std::string_view principal) const {
    const EvaluationInfo* best = nullptr;
    for (const auto& [id, v] : evaluations_) {
        if (v.principal == principal && (!best || v.created > best->created)) {
            best = &v;
        }
    }
    return best;
}

const EvaluationInfo* Evaluations::running(std::string_view principal) const {
    const EvaluationInfo* v = latest(principal);
    return v && !v->done() ? v : nullptr;
}
std::vector<const EvaluationInfo*> Evaluations::cancelledAfter(std::string_view principal, Seq after) const {
    std::vector<const EvaluationInfo*> out;
    for (const std::string& id : order_) {
        const EvaluationInfo& v = evaluations_.at(id);
        if (v.principal == principal && v.cancelled() && v.created > after) {
            out.push_back(&v);
        }
    }
    return out;
}
const EvaluationInfo* Evaluations::streaming(std::string_view principal) const {
    const EvaluationInfo* v = latest(principal);
    return v && !v->done() && (!v->partial.empty() || !v->thinkingPartial.empty()) ? v : nullptr;
}

const EvaluationInfo* Evaluations::latestWithUsage(std::string_view principal) const {
    const EvaluationInfo* best = nullptr;
    for (const auto& [id, v] : evaluations_) {
        if (v.principal == principal && (v.tokensIn >= 0 || v.ctxBytes >= 0) && (!best || v.created > best->created)) {
            best = &v;
        }
    }
    return best;
}

double epochSeconds(const std::string& iso) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0;
    double s = 0.0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &m, &s) != 6) {
        return -1.0;
    }
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = 0;
    const std::time_t t = timegm(&tm);
    return t < 0 ? -1.0 : static_cast<double>(t) + s;
}

std::string progressLine(const Evaluations& all, std::string_view principal, std::string_view name, int iteration,
                         double nowEpoch, int turnLimit) {
    // The phase is read from the latest Evaluation: running with nothing received is thinking
    // (GLM reasons 25-50 s before its first content delta), content arriving is replying.
    const EvaluationInfo* cur = all.latest(principal);
    const bool live = cur && !cur->done() && !cur->runningAt.empty();
    const bool replying = live && cur->replying();
    const bool thinking = live && !replying;
    std::string s = std::string(name) + (replying ? " replying" : thinking ? " thinking" : " working");
    if (iteration > 0) {
        s += " · turn " + std::to_string(iteration) + "/" + std::to_string(turnLimit > 0 ? turnLimit : kLoopIterationCap);
    }
    bool charsShown = false;
    if (replying) {
        s += " · " + kTokens(static_cast<long long>(cur->partial.size())) + " chars";
        charsShown = true;
    } else if (thinking && cur->thinkingChars > 0) {
        s += " · " + kTokens(cur->thinkingChars) + " chars";
        charsShown = true;
    }
    if (live) {
        const double started = epochSeconds(cur->runningAt);
        if (started >= 0 && nowEpoch >= started) {
            s += " · " + std::to_string(static_cast<long long>(std::floor(nowEpoch - started))) + " s";
        }
    }
    if (!charsShown) {
        if (const EvaluationInfo* last = all.latestWithUsage(principal)) {
            s += " · last call " + kTokens(std::max(0LL, last->tokensIn)) + " in";
        }
    }
    return s;
}

std::string kTokens(long long n) {
    char buf[32];
    if (n < 0) {
        return "";
    }
    if (n < 1000) {
        std::snprintf(buf, sizeof buf, "%lld", n);
    } else if (n < 1000000) {
        std::snprintf(buf, sizeof buf, "%.1fK", static_cast<double>(n) / 1000.0);
    } else {
        std::snprintf(buf, sizeof buf, "%.1fM", static_cast<double>(n) / 1000000.0);
    }
    return buf;
}

std::string withSeparators(long long n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int k = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (k > 0 && k % 3 == 0) {
            out.insert(out.begin(), ',');
        }
        out.insert(out.begin(), *it);
        ++k;
    }
    return (n < 0 ? "-" : "") + out;
}

std::string totalsShort(const Totals& t) {
    if (t.evaluations == 0) {
        return "";
    }
    std::string s = std::to_string(t.evaluations) + " ev";
    if (t.withUsage > 0) {
        s += " \u00b7 " + withSeparators(t.tokensIn) + "/" + withSeparators(t.tokensOut);
    }
    return s;
}

std::string statusLine(const Evaluations& all, std::string_view principal) {
    const Totals t = all.totals(principal);
    if (t.evaluations == 0) {
        return "";
    }
    std::string s;
    if (const EvaluationInfo* last = all.latestWithUsage(principal)) {
        s = "CTX last ";
        if (last->ctxBytes >= 0) {
            s += humanBytes(last->ctxBytes) + " · ";
        } else if (const SerializationInfo* ser = all.serialization(last->serialization); ser && ser->bytes >= 0) {
            s += humanBytes(ser->bytes) + " · ";
        }
        s += std::to_string(std::max(0LL, last->tokensIn)) + " in / " + std::to_string(std::max(0LL, last->tokensOut)) + " out";
        if (!last->reasoning.empty()) {
            s += " · reasoning " + last->reasoning;
        }
    }
    s += (s.empty() ? "" : "  ·  ") + std::string("TOTAL ") + std::to_string(t.evaluations) + (t.evaluations == 1 ? " eval" : " evals");
    if (t.withUsage > 0) {
        s += " · " + kTokens(t.tokensIn) + " in / " + kTokens(t.tokensOut) + " out";
    }
    return s;
}

void Evaluations::clear() {
    totalsCache_.clear();
    evaluations_.clear();
    serializations_.clear();
    order_.clear();
    byMessage_.clear();
    applied_ = -1;
}

std::string humanBytes(long long bytes) {
    char buf[32];
    if (bytes < 0) {
        return "";
    }
    if (bytes < 1024) {
        std::snprintf(buf, sizeof buf, "%lld B", bytes);
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof buf, "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

std::string totalsLine(const Totals& t) {
    if (t.evaluations == 0) {
        return "";
    }
    std::string s = std::to_string(t.evaluations) + (t.evaluations == 1 ? " evaluation" : " evaluations");
    if (t.withUsage > 0) {
        s += " · " + std::to_string(t.tokensIn) + " in / " + std::to_string(t.tokensOut) + " out";
    }
    if (t.withContext > 0) {
        s += " · ctx " + humanBytes(t.ctxBytes) + " · " + std::to_string(t.ctxObjects) + " objects";
    }
    return s;
}

std::string costLine(const std::vector<const EvaluationInfo*>& evals, const Evaluations& all) {
    if (evals.empty()) {
        return "";
    }
    const EvaluationInfo* last = evals.back();
    if (last->ctxBytes >= 0 || last->ctxObjects >= 0) {
        long long in = 0, out = 0;
        for (const EvaluationInfo* v : evals) {
            in += std::max(0LL, v->tokensIn);
            out += std::max(0LL, v->tokensOut);
        }
        std::string s = "ctx " + humanBytes(std::max(0LL, last->ctxBytes)) + " · " + std::to_string(in) + " in / " +
                        std::to_string(out) + " out";
        if (last->ctxObjects >= 0) {
            s += " · " + std::to_string(last->ctxObjects) + " objects";
        }
        if (last->imagesSent > 0) {
            s += " · " + std::to_string(last->imagesSent) + " image" + (last->imagesSent > 1 ? "s" : "") + " sent";
        }
        if (last->imagesHeld > 0) {
            s += " · " + std::to_string(last->imagesHeld) + " image" + (last->imagesHeld > 1 ? "s" : "") + " not sent: this model takes none";
        }
        if (!last->reasoning.empty()) {
            s += " · reasoning " + last->reasoning;
        }
        if (evals.size() > 1) {
            s += " · " + std::to_string(evals.size()) + " evaluations";
        }
        return s;
    }
    long long in = 0, out = 0;
    bool usage = false;
    for (const EvaluationInfo* v : evals) {
        if (v->tokensIn >= 0 || v->tokensOut >= 0) {
            usage = true;
            in += std::max(0LL, v->tokensIn);
            out += std::max(0LL, v->tokensOut);
        }
    }
    std::string s;
    if (usage) {
        s = std::to_string(in) + " in / " + std::to_string(out) + " out";
    }
    if (const SerializationInfo* ser = all.serialization(evals.back()->serialization); ser && ser->bytes >= 0) {
        s += (s.empty() ? "" : " · ") + humanBytes(ser->bytes) + " serialized";
    }
    s += (s.empty() ? "" : " · ") + std::to_string(evals.size()) + (evals.size() == 1 ? " evaluation" : " evaluations");
    return s;
}

} // namespace ke::model
