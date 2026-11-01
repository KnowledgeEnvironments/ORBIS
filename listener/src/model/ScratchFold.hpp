// An off-the-record session as a pure fold over its pushed events. They reach this process alone:
// nothing here is read back from the world, and nothing here is ever written anywhere. Progress,
// the cost line and the CTX totals come from the same Evaluations fold a terminal uses; only the
// conversation lines are this fold's own, because a scratch cannot be read.
#pragma once

#include "model/Evaluations.hpp"
#include "model/Event.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ke::model {

struct ScratchLine {
    bool self = false;
    bool note = false; // said by the session rather than a party: failed, cancelled
    std::string text;
    std::string evaluation; // the Evaluation behind a reply, for its cost line
};

class ScratchFold {
  public:
    void said(const std::string& text);
    void apply(const Event& e);
    void clear();
    void setPrincipal(std::string id) { principal_ = std::move(id); }
    const std::vector<ScratchLine>& lines() const { return lines_; }
    // The reply so far, shown as a provisional block; "" when nothing is streaming.
    std::string partial() const;
    std::string thinking() const;
    bool answering() const { return answering_; }
    const Evaluations& evaluations() const { return evals_; }
    // `ctx 12.4 KiB · 2287 in / 1016 out · 14 objects`, or "" when the world sent no figures.
    std::string costLine(const ScratchLine& line) const;
    // `root thinking · 1.2K chars · 31 s`: the terminal's own line.
    std::string progress(std::string_view name, double nowEpoch) const;
    std::string status() const;
    long long tokensIn() const;
    long long tokensOut() const;

  private:
    std::vector<ScratchLine> lines_;
    Evaluations evals_;
    std::string principal_;
    bool answering_ = false;
};

} // namespace ke::model
