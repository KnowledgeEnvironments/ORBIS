// Evaluations and Serializations as a pure fold over Events, for the cost line under a reply and
// the ops a loop ran (spec 9.8; decisions 2026-10-30 "the model acts through protocol ops"):
//
//   created {class evaluation fields}        -> an Evaluation (initiator = actor, model)
//   created {class serialization fields}     -> its size (serialization/size.bytes) and hash
//   evaluated {evaluation state serialization cost outcome} -> state, tokens (cost.tokens-in/out
//                                               or outcome.usage.prompt-tokens/completion-tokens)
//   field-set on an Evaluation: evaluation/ops, evaluation/results, evaluation/iteration
//
// Nothing is computed from memory the log does not hold; a reply's `via` names the last
// Evaluation of a wake, the earlier ones are found by principal and seq range.
#pragma once

#include "model/Event.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ke::model {

class Evaluations;

struct SerializationInfo {
    std::string id;
    long long bytes = -1; // -1 unknown
    std::string hash;
    Seq created = 0;
};

struct EvaluationInfo {
    std::string id;
    std::string principal; // the initiator (actor of `created`)
    std::string model;
    std::string serialization;
    std::string message; // evaluation/message: the wake's Message (the question this answers), "" when unknown
    std::string wake;    // evaluation/wake: the delivered event id that woke the loop
    std::string state; // queued, running, streaming, done, failed, cancelled
    std::string reason; // why it ended, on a cancelled Evaluation: "steer" is not a failure
    long long tokensIn = -1;
    long long tokensOut = -1;
    // evaluation/context {serialization-bytes prompt-tokens completion-tokens objects}, -1 unknown
    long long ctxBytes = -1;
    long long ctxIn = -1;
    long long ctxOut = -1;
    long long ctxObjects = -1;
    int imagesSent = 0;    // evaluation/images: what went to the Model
    int imagesHeld = 0;    // and what it could not take, sent as a line of text
    std::string reasoning; // evaluation/reasoning: "on" or "off", what this call actually did
    int iteration = 0;
    json thinking;                // evaluation/thinking: a string, or a blob ref map, when the world records it
    json ops = json::array();     // what the model asked: [{op args err?}]
    json results = json::array(); // what each op answered: [{op args ok result? seq? err?}]
    Seq created = 0;
    Seq last = 0; // seq of the latest event about it
    std::string at;
    std::string runningAt; // event/at of `evaluated running` ("" until then)
    // Streaming: `evaluated` with state streaming carries {delta seq-in-stream} at <= 4 Hz, or a
    // full `partial`; the text so far accumulates here until the final `done`. The same events
    // may carry {thinking-delta thinking-chars} (the model's reasoning, which comes before any
    // content); unknown keys are ignored, counts only grow.
    std::string partial;
    std::string thinkingPartial;  // reasoning text streamed so far ("" when the world sends none)
    long long thinkingChars = -1; // thinking-chars as reported, or thinkingPartial.size(); -1 unknown
    long long streamSeq = -1;     // last seq-in-stream applied (duplicates and reorders are dropped)
    bool done() const { return state == "done" || state == "failed" || state == "cancelled"; }
    bool cancelled() const { return state == "cancelled"; }
    // Live only: nothing has been received yet, or content is arriving.
    bool replying() const { return !partial.empty(); }
};

// Parse an ISO-8601 UTC instant ("2026-10-30T15:18:23.676Z") to seconds since the epoch; -1 when malformed.
double epochSeconds(const std::string& iso);
// The live line for a Principal whose loop runs, by phase of its latest Evaluation:
//   no Evaluation running yet   `root evaluating · iteration 2/8 · last call 5.6K in`
//   running, nothing received   `root thinking · 31 s · last call 5.8K in`
//   thinking-chars reported     `root thinking · 1.2K chars · 31 s`
//   content deltas arriving     `root replying · 420 chars · 34 s`
// `nowEpoch` is the wall clock in seconds. The loop cap (8) is the world's (loop/ops.clj).
// What a turn did, in a few words: `read Prefs.cpp`, `edit App.cpp`, `run cmake --build`. "" for nothing.
std::string opSummary(const json& ops);

std::string progressLine(const Evaluations& all, std::string_view principal, std::string_view name, int iteration,
                         double nowEpoch, int turnLimit = 0, int queued = 0, std::string_view modelName = {});
constexpr int kLoopIterationCap = 8;

struct Totals {
    long long tokensIn = 0;
    long long tokensOut = 0;
    long long ctxBytes = 0;   // serialization bytes sent, summed
    long long ctxObjects = 0; // objects serialized, summed
    int evaluations = 0;
    int withUsage = 0;
    int withContext = 0;
};

// `2 evaluations · 6074 in / 2612 out · ctx 24.2 KiB · 28 objects`, parts omitted when unknown.
std::string totalsLine(const Totals& t);
// 61234 -> "61.2K", 987 -> "987", 1200000 -> "1.2M".
std::string kTokens(long long n);
// 1234567 -> "1,234,567" (the PRINCIPALS TOTAL column and tooltips).
std::string withSeparators(long long n);
// The terminal's status bar for one Principal:
// `CTX last 12.4 KiB · 2287 in / 1016 out  ·  TOTAL 48 evals · 61.2K in / 19.4K out`; "" when none.
std::string statusLine(const Evaluations& all, std::string_view principal);
// The PRINCIPALS column: `48 ev · 61.2K/19.4K`; "" when none.
std::string totalsShort(const Totals& t);

struct SnapshotObject;

class Evaluations {
  public:
    bool apply(const Event& e);
    // Seed from one snapshot object (model/Snapshot): an Evaluation with its bookkeeping Fields
    // (ops, results, iteration, thinking, context) or a Serialization. A snapshot carries no
    // `evaluated` events, so the state comes from an `evaluation/state` Field when the world
    // records one, else `done`: anything still live at the snapshot reports itself in the tail.
    void seed(const SnapshotObject& o);
    void setApplied(Seq seq) { applied_ = std::max(applied_, seq); }
    const EvaluationInfo* get(std::string_view id) const;
    const SerializationInfo* serialization(std::string_view id) const;
    // The Evaluations `principal` initiated with created seq in (after, upto], in seq order: the
    // wake that produced a reply at `upto` when `after` is the previous line of its transcript.
    std::vector<const EvaluationInfo*> between(std::string_view principal, Seq after, Seq upto) const;
    // The Evaluations whose evaluation/message is `message` (the wake that answered it), in
    // creation order, limited to `principal` when given. Exact where the world records the Field
    // (every loop Evaluation since world pass 2); a snapshot seeds it too, so a booted session
    // finds a reply's wake without its ref's via.
    std::vector<const EvaluationInfo*> forMessage(std::string_view message, std::string_view principal = {}) const;
    Totals totals(std::string_view principal) const;
    template <class F> void forEachId(F&& f) const {
        for (const auto& [id, e] : evaluations_) {
            f(id);
        }
    }
    // The Principal's most recently created Evaluation, or nullptr.
    const EvaluationInfo* latest(std::string_view principal) const;
    // The newest finished turn of this Principal that did something, nullptr for none.
    const EvaluationInfo* latestWithOps(std::string_view principal) const;
    // Its Evaluation that is live right now (created, not done), or nullptr: what a cancel targets.
    const EvaluationInfo* running(std::string_view principal) const;
    // Cancelled Evaluations of the Principal created after `after` (the orphans a transcript shows
    // as dim `cancelled` blocks when no reply followed), in creation order.
    std::vector<const EvaluationInfo*> cancelledAfter(std::string_view principal, Seq after) const;
    // The Principal's Evaluation that is streaming right now (its latest, not done, with content
    // or thinking received), or nullptr: the transcript shows it once as a provisional block.
    const EvaluationInfo* streaming(std::string_view principal) const;
    // Its most recent Evaluation that recorded usage or context (the last real call), or nullptr.
    const EvaluationInfo* latestWithUsage(std::string_view principal) const;
    std::size_t size() const { return evaluations_.size(); }
    Seq applied() const { return applied_; }
    void clear();

  private:
    std::map<std::string, EvaluationInfo> evaluations_;
    std::map<std::string, SerializationInfo> serializations_;
    std::vector<std::string> order_; // evaluation ids in creation order
    std::map<std::string, std::vector<std::string>> byMessage_; // evaluation/message -> evaluation ids, creation order
    void indexMessage(const std::string& evaluation, const std::string& message);
    Seq applied_ = -1;
    // totals() per principal, valid for one applied seq (panes ask every frame).
    mutable std::map<std::string, std::pair<Seq, Totals>> totalsCache_;
};

// With evaluation/context on the last Evaluation: `ctx 12.4 KiB · 2287 in / 1016 out · 14 objects`
// (plus ` · N evaluations` for a multi-step wake); otherwise
// `2287 in / 1016 out · 8.1 KiB serialized · 2 evaluations` (parts omitted when unknown); "" when nothing.
std::string costLine(const std::vector<const EvaluationInfo*>& evals, const Evaluations& all);
std::string humanBytes(long long bytes);

} // namespace ke::model
