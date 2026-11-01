#include "model/ScratchFold.hpp"

namespace ke::model {

void ScratchFold::said(const std::string& text) {
    lines_.push_back(ScratchLine{true, false, text, ""});
    answering_ = true;
}

void ScratchFold::apply(const Event& e) {
    evals_.apply(e);
    if (e.kind != "evaluated" || !e.body.is_object()) {
        return;
    }
    const std::string state = e.body.value("state", "");
    const std::string id = e.body.value("evaluation", e.stream);
    if (state == "done") {
        const EvaluationInfo* ev = evals_.get(id);
        const json outcome = e.body.contains("outcome") ? e.body["outcome"] : json::object();
        std::string reply = outcome.is_object() && outcome.contains("text") && outcome["text"].is_string() ? outcome["text"].get<std::string>() : std::string();
        if (reply.empty() && ev) {
            reply = ev->partial;
        }
        if (!reply.empty()) {
            lines_.push_back(ScratchLine{false, false, std::move(reply), id});
        }
        answering_ = false;
    } else if (state == "failed" || state == "cancelled") {
        if (const EvaluationInfo* ev = evals_.get(id); ev && !ev->partial.empty()) {
            lines_.push_back(ScratchLine{false, false, ev->partial, id});
        }
        const std::string why = e.body.contains("error") && e.body["error"].is_string() ? e.body["error"].get<std::string>() : std::string();
        lines_.push_back(ScratchLine{false, true, state + (why.empty() ? std::string() : ": " + why), ""});
        answering_ = false;
    }
}

void ScratchFold::clear() {
    lines_.clear();
    evals_.clear();
    principal_.clear();
    answering_ = false;
}

std::string ScratchFold::partial() const {
    const EvaluationInfo* ev = answering_ ? evals_.streaming(principal_) : nullptr;
    return ev ? ev->partial : std::string();
}

std::string ScratchFold::thinking() const {
    const EvaluationInfo* ev = answering_ ? evals_.streaming(principal_) : nullptr;
    return ev ? ev->thinkingPartial : std::string();
}

std::string ScratchFold::costLine(const ScratchLine& line) const {
    const EvaluationInfo* ev = line.evaluation.empty() ? nullptr : evals_.get(line.evaluation);
    return ev ? model::costLine({ev}, evals_) : std::string();
}

std::string ScratchFold::progress(std::string_view name, double nowEpoch) const {
    return answering_ ? progressLine(evals_, principal_, name, 1, nowEpoch) : std::string();
}

std::string ScratchFold::status() const {
    return statusLine(evals_, principal_);
}

long long ScratchFold::tokensIn() const {
    return evals_.totals(principal_).tokensIn;
}

long long ScratchFold::tokensOut() const {
    return evals_.totals(principal_).tokensOut;
}

} // namespace ke::model
