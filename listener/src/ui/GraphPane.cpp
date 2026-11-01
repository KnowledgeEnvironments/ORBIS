#include "ui/GraphPane.hpp"

#include "ui/ActivityMark.hpp"
#include "model/ObjectTree.hpp"
#include "platform/Platform.hpp"
#include "ui/Chrome.hpp"
#include "ui/SymbolUi.hpp"

#include "drive/Commands.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <imgui_stdlib.h>

namespace ke::ui {

namespace {

constexpr double kPulseSeconds = 1.1;
constexpr double kHoldSeconds = 0.5;
constexpr std::size_t kMaxPulses = 8;
constexpr float kPadLines = 0.3f;

float dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

ImVec2 at(ImVec2 origin, model::P2 p) { return ImVec2(origin.x + p.x, origin.y + p.y); }

bool inside(ImVec2 p, ImVec2 mn, ImVec2 mx) { return p.x >= mn.x && p.x < mx.x && p.y >= mn.y && p.y < mx.y; }

// The text cut to fit `width`, on a character boundary, with an ellipsis when anything went.
std::string fit(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) {
        return s;
    }
    const std::string dots = "…";
    std::string out = s;
    while (!out.empty() && ImGui::CalcTextSize((out + dots).c_str()).x > width) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            out.pop_back();
        }
    }
    return out + dots;
}

const char* glyph(model::ActorKind k) {
    switch (k) {
    case model::ActorKind::Person:
        return "@";
    case model::ActorKind::Agent:
        return ">";
    case model::ActorKind::Group:
        return "#";
    case model::ActorKind::Model:
        return "*";
    }
    return "";
}

// A point `t` of the way along a polyline, 0 to 1.
ImVec2 along(const std::vector<ImVec2>& pts, float t) {
    float total = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        total += dist(pts[i - 1], pts[i]);
    }
    float left = total * t;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float d = dist(pts[i - 1], pts[i]);
        if (left <= d && d > 0.0f) {
            const float k = left / d;
            return ImVec2(pts[i - 1].x + (pts[i].x - pts[i - 1].x) * k, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * k);
        }
        left -= d;
    }
    return pts.empty() ? ImVec2() : pts.back();
}

} // namespace

bool GraphPane::spinning(const model::Principals& principals) const {
    for (const model::PrincipalInfo* p : principals.ordered()) {
        if (p->loopIteration() > 0) {
            return true;
        }
    }
    return false;
}

void GraphPane::setOverlay(model::GraphModel g, std::string title) {
    overlay_ = std::move(g);
    overlayTitle_ = std::move(title);
}

void GraphPane::clearOverlay() {
    overlay_ = model::GraphModel{};
    overlayTitle_.clear();
}

void GraphPane::status(std::string text, bool err) {
    status_.push_back(Status{std::move(text), err});
    if (status_.size() > 3) {
        status_.erase(status_.begin());
    }
}

namespace {

// The twilight field of PROMETHEUS (tools/prometheus/native/explorer.cpp), flat: Howard Look's SGI
// twilight.c colours compressed into the lower quarter of the view, black above, seeded stars that
// never move. Same constants, same generator, same seed.
constexpr float kTwilightBand = 0.25f;     // the share of the view the colour takes
constexpr float kTwilightBluePeak = 0.055f, kTwilightBlackHeight = 0.120f; // heights on the sphere
constexpr float kSkyAlpha = 0.32f, kSmallStarAlpha = 0.52f, kLargeStarAlpha = 0.68f;
constexpr int kLargeStarEvery = 21; // 120 of 2,620 on the sphere
constexpr float kPixelsPerStar = 2200.0f; // what of the 2,620 a camera sees, as a density

ImU32 mixColor(ImU32 a, ImU32 b, float t, float alpha) {
    const auto chan = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(x + (y - x) * t + 0.5f);
    };
    return (chan(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) | (chan(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) |
           (chan(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT) | (static_cast<ImU32>(alpha * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 skyColor(float height, float alpha) {
    if (height < kTwilightBluePeak) {
        return mixColor(Surface::twilightOrange, Surface::twilightBlue, height / kTwilightBluePeak, alpha);
    }
    if (height < kTwilightBlackHeight) {
        return mixColor(Surface::twilightBlue, Surface::twilightBlack,
                        (height - kTwilightBluePeak) / (kTwilightBlackHeight - kTwilightBluePeak), alpha);
    }
    return mixColor(Surface::twilightBlack, Surface::twilightBlack, 0.0f, alpha);
}

void drawTwilight(ImDrawList* dl, ImVec2 at, ImVec2 size) {
    const float bottom = at.y + size.y, band = size.y * kTwilightBand, right = at.x + size.x;
    const float peak = bottom - band * (kTwilightBluePeak / kTwilightBlackHeight);
    const ImU32 orange = skyColor(0.0f, kSkyAlpha), blue = skyColor(kTwilightBluePeak, kSkyAlpha), black = skyColor(1.0f, kSkyAlpha);
    dl->AddRectFilledMultiColor(ImVec2(at.x, peak), ImVec2(right, bottom), blue, blue, orange, orange);
    dl->AddRectFilledMultiColor(ImVec2(at.x, bottom - band), ImVec2(right, peak), black, black, blue, blue);

    std::uint64_t state = 0x5347492d5457494cULL;
    const auto unit = [&state] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>((state >> 40) & 0xffffffu) / static_cast<float>(0x1000000u);
    };
    const int stars = static_cast<int>(size.x * size.y / kPixelsPerStar);
    for (int i = 0; i < stars; ++i) {
        const float x = at.x + unit() * size.x, y = at.y + unit() * size.y;
        const float view = (bottom - y) / std::max(1.0f, band); // 0 at the horizon, 1 at the top of the colour
        const float height = view * kTwilightBlackHeight;
        const bool large = i % kLargeStarEvery == 0;
        const float alpha = large ? kLargeStarAlpha : kSmallStarAlpha;
        const ImU32 ink = height >= kTwilightBlackHeight
                              ? mixColor(Surface::twilightStar, Surface::twilightStar, 0.0f, alpha)
                              : mixColor(skyColor(height, 1.0f), Surface::twilightStar, height / kTwilightBlackHeight, alpha);
        const float side = large ? 1.8f : 1.0f;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + side, y + side), ink);
    }
}

} // namespace

GraphActions GraphPane::draw(const GraphInput& in) {
    GraphActions a;
    if (beginPane("GRAPH", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, !header)) {
        if (header) {
            header();
        }
        a = drawBody(in);
    }
    endPane();
    return a;
}

void GraphPane::rebuild(const GraphInput& in, float width) {
    std::string key = std::to_string(in.principals.applied()) + "/" + std::to_string(in.tasks.applied()) + "/" +
                      std::to_string(in.panes.size()) + "/" + in.session.principal().value_or("");
    for (const chat::UnsafeEntry& e : in.unsafe) {
        key += "/" + e.principal;
    }
    for (const auto& [id, n] : in.unread) {
        key += "/" + id + ":" + std::to_string(n);
    }
    for (const auto& [id, a] : in.activity) {
        key += "/" + id + "=" + std::to_string(static_cast<int>(a));
    }
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const bool fresh = key != builtFor_;
    if (fresh) {
        model::MapState state;
        state.self = in.session.principal().value_or("");
        for (const chat::UnsafeEntry& e : in.unsafe) {
            state.unsafe.insert(e.principal);
        }
        state.unread = in.unread;
        state.activity = in.activity;
        map_ = model::buildMap(in.principals, in.tasks, in.panes, state);
        builtFor_ = key;
        welcome_ = in.session.attached() && !map_.actors.empty() && model::firstVisit(map_, in.principals, state.self);
    }
    if (fresh || width != laidFor_ || line != laidLine_) {
        model::MapMetrics m;
        m.lineH = line;
        m.charW = ImGui::CalcTextSize("M").x;
        welcomeH_ = welcome_ ? line * 8.5f : 0.0f;
        m.top = welcomeH_;
        model::layoutMap(map_, width, m);
        laidFor_ = width;
        laidLine_ = line;
    }
}

void GraphPane::notePulses(const GraphInput& in) {
    const std::vector<model::MessageRef>& messages = in.principals.messages();
    const double now = in.anim.t;
    if (baselined_) {
        for (std::size_t i = messagesSeen_; i < messages.size(); ++i) {
            const model::MessageRef& m = messages[i];
            if (!m.recipient.empty() && m.from != m.recipient) {
                pulses_.push_back(Pulse{m.from, m.recipient, now});
            }
        }
    } else if (!messages.empty()) {
        baselined_ = true; // what was already said is history, not traffic
    }
    messagesSeen_ = messages.size();
    pulses_.erase(std::remove_if(pulses_.begin(), pulses_.end(),
                                 [now](const Pulse& p) { return now - p.start > kPulseSeconds; }),
                  pulses_.end());
    if (pulses_.size() > kMaxPulses) {
        pulses_.erase(pulses_.begin(), pulses_.end() - static_cast<std::ptrdiff_t>(kMaxPulses));
    }
}

const model::MapActor* GraphPane::actorAt(ImVec2 mouse, ImVec2 origin) const {
    for (const model::MapActor& a : map_.actors) {
        if (inside(mouse, at(origin, {a.x, a.y}), at(origin, {a.right(), a.bottom()}))) {
            return &a;
        }
    }
    return nullptr;
}

std::string GraphPane::sharedRoute(const GraphInput& in, const std::string& who) const {
    const std::string self = in.session.principal().value_or("");
    const model::PrincipalInfo* me = in.principals.get(self);
    const model::PrincipalInfo* them = in.principals.get(who);
    if (me == nullptr || them == nullptr) {
        return "";
    }
    const std::vector<std::string> mine = me->routes();
    for (const std::string& r : them->routes()) {
        if (std::find(mine.begin(), mine.end(), r) != mine.end()) {
            return r;
        }
    }
    return "";
}

void GraphPane::connect(const GraphInput& in, const std::string& from, const std::string& to) {
    const std::string name = in.principals.nameOf(from) + ".to." + in.principals.nameOf(to);
    if (actions_ == nullptr || actions_->get("route.create") == nullptr) {
        status("this world offers no route.create", true);
        return;
    }
    status("connecting " + name + " ...", false);
    in.chat.actNamed("route.create", json{{"name", name}, {"members", json::array({from, to})}},
                     [this, name](const Response& r) {
                         if (!r.ok) {
                             status("connect " + name + ": " + (r.err ? r.err->code + " " + r.err->message : "refused"), true);
                             return;
                         }
                         status(name + " connected", false);
                     });
}

void GraphPane::landed(const std::string& from, const std::string& to) {
    dropFrom_ = from;
    dropTo_ = to;
    popupWanted_ = "drop-menu";
}

void GraphPane::drawLinks(ImDrawList* dl, ImVec2 origin, const Anim& anim) {
    if (map_.stacked) {
        return; // one column has nowhere to run them: a node says who it talks to when it pulses
    }
    for (const model::MapLink& l : map_.links) {
        if (l.path.size() < 2) {
            continue;
        }
        std::vector<ImVec2> pts;
        for (const model::P2& p : l.path) {
            pts.push_back(at(origin, p));
        }
        const bool lit = !hovered_.empty() && (l.from == hovered_ || l.to == hovered_);
        // What has carried something stands out; what merely exists stays out of the way.
        const ImU32 color = lit ? Ink::nav : l.traffic ? Ink::primary : Surface::codeBorder;
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), color, lit || l.traffic ? 1.5f : 1.0f, ImDrawFlags_None);
    }
    for (const Pulse& p : pulses_) {
        const model::MapLink* l = map_.link(p.from, p.to);
        if (l == nullptr || l->path.size() < 2) {
            continue;
        }
        std::vector<ImVec2> pts;
        for (const model::P2& q : l->path) {
            pts.push_back(at(origin, q));
        }
        if (l->from != p.from) {
            std::reverse(pts.begin(), pts.end());
        }
        const float t = std::clamp(static_cast<float>((anim.t - p.start) / kPulseSeconds), 0.0f, 1.0f);
        const ImVec2 c = along(pts, t);
        const float r = std::floor(ImGui::GetTextLineHeight() * 0.22f) + 1.0f;
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), Ink::strong);
    }
}

void GraphPane::drawPorts(const model::MapActor& a, ImVec2 origin, float top) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float charW = ImGui::CalcTextSize("M").x;
    const float left = origin.x + a.x + charW * 1.25f;
    const float textX = left + charW * 1.5f;
    const float right = origin.x + a.right() - charW;
    float y = top;
    const int rows = static_cast<int>(a.ports.size()) + (a.morePorts > 0 ? 1 : 0);
    int row = 0;
    for (const model::MapPort& p : a.ports) {
        ++row;
        treeGuide(dl, std::floor(left), std::floor(y), std::floor(y + line), std::floor(textX - charW * 0.3f), row == rows);
        ImGui::SetCursorScreenPos(ImVec2(textX, y));
        ImGui::PushID(p.id.c_str());
        ImGui::SetNextItemAllowOverlap();
        const bool clicked = ImGui::InvisibleButton("port", ImVec2(std::max(1.0f, right - textX), line));
        const bool hot = ImGui::IsItemHovered();
        drive::hit("port:" + p.name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (clicked) {
            LinkRequest r;
            r.object = p.id;
            if (p.pane) {
                r.kind = LinkRequest::OpenPane;
                r.openKind = p.kind;
            } else {
                r.kind = LinkRequest::Inspect;
            }
            symbolUi().ask(std::move(r));
        }
        if (hot) {
            ImGui::SetTooltip("%s\n%s\n%s", p.name.c_str(), p.kind.c_str(), p.pane ? "click opens the pane" : "click inspects it");
        }
        ImGui::PopID();
        const float kindW = ImGui::CalcTextSize(p.kind.c_str()).x;
        const float nameRoom = right - textX - kindW - charW;
        const bool showKind = nameRoom > charW * 8.0f;
        dl->AddText(ImVec2(textX, y), hot ? Ink::nav : Ink::primary, fit(p.name, showKind ? nameRoom : right - textX).c_str());
        if (showKind) {
            dl->AddText(ImVec2(right - kindW, y), Ink::secondary, p.kind.c_str());
        }
        y += line;
    }
    if (a.morePorts > 0) {
        treeGuide(dl, std::floor(left), std::floor(y), std::floor(y + line), std::floor(textX - charW * 0.3f), true);
        dl->AddText(ImVec2(textX, y), Ink::secondary, ("+" + std::to_string(a.morePorts) + " more").c_str());
    }
}

void GraphPane::drawActor(const GraphInput& in, GraphActions& out, const model::MapActor& a, ImVec2 origin) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mn = at(origin, {a.x, a.y});
    const ImVec2 mx = at(origin, {a.right(), a.bottom()});
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float charW = ImGui::CalcTextSize("M").x;
    const float pad = lineH * kPadLines;
    const float textX = mn.x + charW;
    const float right = mx.x - charW;
    const ImGuiIO& io = ImGui::GetIO();
    const bool touch = platform::touchScreen();

    ImGui::PushID(a.id.c_str());
    ImGui::SetCursorScreenPos(mn);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("actor", ImVec2(mx.x - mn.x, mx.y - mn.y),
                                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hot = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    drive::hit("actor:" + a.name, mn, mx);
    if (hot) {
        hovered_ = a.id;
        tooltip(in, a);
    }

    const bool isPerson = a.id == in.session.principal().value_or("");
    const bool pulse = std::any_of(pulses_.begin(), pulses_.end(), [&a](const Pulse& p) { return p.to == a.id || p.from == a.id; });
    const bool principal = a.kind == model::ActorKind::Person || a.kind == model::ActorKind::Agent;

    pressing_ = pressing_ || held;
    // Press, in order: an armed connect, a hold, a drag, a click.
    if (held && io.MouseDownDuration[ImGuiMouseButton_Left] > kHoldSeconds && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !holdFired_) {
        holdFired_ = true;
        menuNode_ = a.id;
        platform::haptic(platform::Haptic::Tap);
        popupWanted_ = "node-menu";
    }
    if (held && !touch && principal && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !holdFired_) {
        dragFrom_ = a.id;
    }
    if (clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        menuNode_ = a.id;
        popupWanted_ = "node-menu";
    } else if (clicked && dragFrom_.empty() && !holdFired_) {
        if (!armedFrom_.empty()) {
            if (armedFrom_ != a.id && principal) {
                landed(armedFrom_, a.id);
            }
            armedFrom_.clear();
        } else if (io.KeyShift) {
            out.focus = a.id;
            out.view = 2;
        } else {
            out.focus = a.id;
            out.view = a.kind == model::ActorKind::Model || a.kind == model::ActorKind::Group ? 2 : 0;
        }
        selected_ = a.id;
    }
    if (!held && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        holdFired_ = false;
    }

    // The face. The frame says the one thing that matters most here.
    chrome(dl, mn, mx, ChromeRole::RaisedPane);
    const ImU32 frame = a.unsafe ? Ink::unsafe : a.waiting > 0 ? Ink::inspected : hot || pulse ? Ink::nav : a.id == selected_ ? Ink::strong : Surface::codeBorder;
    dl->AddRect(mn, mx, frame, 0.0f, a.unsafe || a.waiting > 0 || pulse ? 2.0f : 1.0f);

    // Name, and on the right the badges.
    float y = mn.y + pad;
    float nameRight = right;
    if (a.unread > 0) {
        const std::string n = std::to_string(a.unread);
        const float w = ImGui::CalcTextSize(n.c_str()).x + charW;
        const ImVec2 bmn(right - w, y), bmx(right, y + ImGui::GetTextLineHeight());
        dl->AddRectFilled(bmn, bmx, Ink::nav);
        dl->AddText(ImVec2(bmn.x + charW * 0.5f, y), Surface::background, n.c_str()); // dark on bright reads, white on cyan does not
        nameRight = bmn.x - charW * 0.5f;
    }
    // Working has the tape below, and an idle node says so in its line.
    if (a.activity != model::Activity::None && a.activity != model::Activity::Working && a.activity != model::Activity::Idle) {
        const float s = ImGui::GetTextLineHeight();
        activityMark(dl, ImVec2(nameRight - s, y), s, a.activity, ImGui::GetTime());
        nameRight -= s + charW * 0.5f;
    }
    if (a.unsafe) {
        std::string word = upper("unsafe");
        for (const chat::UnsafeEntry& e : in.unsafe) {
            if (e.principal == a.id) {
                word += " " + std::to_string(chat::minutesLeft(e.until, in.wallClock)) + "m";
            }
        }
        const float w = ImGui::CalcTextSize(word.c_str()).x;
        dl->AddText(ImVec2(nameRight - w, y), Ink::unsafe, word.c_str());
        nameRight -= w + charW * 0.5f;
    }
    dl->AddText(ImVec2(textX, y), Ink::secondary, glyph(a.kind));
    const float nameX = textX + charW * 1.5f;
    dl->AddText(ImVec2(nameX, y), Ink::strong, fit(a.name, nameRight - nameX).c_str());
    y += lineH;

    // What it is doing.
    const float room = right - textX;
    switch (a.kind) {
    case model::ActorKind::Agent:
        if (a.waiting > 0) {
            dl->AddText(ImVec2(textX, y), Ink::inspected, fit("wants: " + a.waitingSummary, room).c_str());
        } else if (a.loop == model::LoopState::Running) {
            const float tape = std::min(room, charW * 8.0f);
            hazardTape(dl, ImVec2(textX, y + 1.0f), ImVec2(textX + tape, y + ImGui::GetTextLineHeight() - 1.0f), in.anim.t);
            dl->AddText(ImVec2(textX + tape + charW, y), Ink::primary, "working");
        } else if (a.loop == model::LoopState::Stalled) {
            dl->AddText(ImVec2(textX, y), Ink::inspected, fit("stalled: " + a.stalled, room).c_str());
        } else {
            dl->AddText(ImVec2(textX, y), Ink::secondary, "idle");
        }
        y += lineH;
        if (a.waiting == 1 && a.canDecide) {
            ImGui::SetCursorScreenPos(ImVec2(textX, y));
            ImGui::SetNextItemAllowOverlap();
            if (bracketButtonNamed("approve", "approve:" + a.name, true, false, Ink::inspected)) {
                const std::string id = a.waitingTask, name = a.name;
                in.chat.actNamed("task.approve", json{{"task", id}}, [this, name](const Response& r) {
                    status(r.ok ? "approved for " + name : "approve: " + (r.err ? r.err->code + " " + r.err->message : "refused"), !r.ok);
                });
            }
            ImGui::SameLine();
            ImGui::SetNextItemAllowOverlap();
            if (bracketButtonNamed("deny", "deny:" + a.name)) {
                const std::string id = a.waitingTask, name = a.name;
                in.chat.actNamed("task.deny", json{{"task", id}, {"reason", "denied from the map"}}, [this, name](const Response& r) {
                    status(r.ok ? "denied for " + name : "deny: " + (r.err ? r.err->code + " " + r.err->message : "refused"), !r.ok);
                });
            }
        } else if (a.waiting > 0) {
            ImGui::SetCursorScreenPos(ImVec2(textX, y));
            ImGui::SetNextItemAllowOverlap();
            const std::string word = "open " + std::to_string(a.waiting);
            if (bracketButtonNamed(word.c_str(), "cards:" + a.name, true, false, Ink::inspected)) {
                out.approvals = true;
            }
        } else if (const std::string totals = model::totalsLine(in.evals.totals(a.id)); !totals.empty()) {
            dl->AddText(ImVec2(textX, y), Ink::secondary, fit(totals, room).c_str());
        }
        y += lineH;
        break;
    case model::ActorKind::Person:
        dl->AddText(ImVec2(textX, y), Ink::secondary, isPerson ? "you" : "person");
        y += lineH;
        break;
    case model::ActorKind::Group:
        dl->AddText(ImVec2(textX, y), Ink::secondary, "group");
        y += lineH;
        break;
    case model::ActorKind::Model: {
        const std::string st = a.modelStatus.empty() ? "unknown" : a.modelStatus;
        const float w = ImGui::CalcTextSize(st.c_str()).x;
        dl->AddText(ImVec2(right - w, y), st == "ok" ? Ink::connected : st == "unknown" ? Ink::secondary : Ink::inspected, st.c_str());
        dl->AddText(ImVec2(textX, y), Ink::secondary, fit(a.detail, room - w - charW).c_str());
        y += lineH;
        break;
    }
    }
    drawPorts(a, origin, y);
    ImGui::PopID();
}

void GraphPane::tooltip(const GraphInput& in, const model::MapActor& a) {
    if (!dragFrom_.empty()) {
        return;
    }
    const model::PrincipalInfo* p = in.principals.get(a.id);
    std::string tip = a.name + "\n" + a.id + "\n" + model::name(a.kind);
    if (const std::string what = p != nullptr ? in.principals.describe(a.id, p->cls) : std::string(); !what.empty()) {
        tip += "\n" + what;
    }
    if (p != nullptr && !p->loopStalled().empty()) {
        tip += "\nstalled: " + p->loopStalled();
    }
    if (const model::FieldState* q = p != nullptr ? p->field("loop/queue") : nullptr; q != nullptr && q->value.is_array() && !q->value.empty()) {
        tip += "\nqueued: " + std::to_string(q->value.size());
    }
    if (const model::FieldState* h = in.principals.fieldOf(a.id, "object/hint");
        h != nullptr && h->value.is_string() && !h->value.get<std::string>().empty()) {
        tip += "\nhint: " + h->value.get<std::string>() + " (by " + in.principals.nameOf(h->by) + ")";
    }
    for (const model::MapLink& l : map_.links) {
        if (l.from != a.id && l.to != a.id) {
            continue;
        }
        const std::string other = in.principals.nameOf(l.from == a.id ? l.to : l.from);
        tip += "\n" + std::string(l.kind == model::LinkKind::Model ? "model " : "route to ") + other;
        if (l.messages > 0) {
            tip += " (" + std::to_string(l.messages) + " messages)";
        }
    }
    if (const std::string totals = model::totalsLine(in.evals.totals(a.id)); !totals.empty()) {
        tip += "\n" + totals;
    }
    tip += platform::touchScreen() ? "\ntap opens, hold for more" : "\nclick opens, shift-click inspects, drag onto another to connect";
    ImGui::SetTooltip("%s", tip.c_str());
}

void GraphPane::drawWelcome(const GraphInput& in, GraphActions& out, ImVec2 origin) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float charW = ImGui::CalcTextSize("M").x;
    const float width = std::min(std::max(map_.width, laidFor_) - 2.0f * charW, charW * 76.0f);
    const ImVec2 mn(origin.x + charW, origin.y + line * 0.5f);
    const ImVec2 mx(mn.x + width, mn.y + line * 7.0f);
    chrome(dl, mn, mx, ChromeRole::InsetWell);
    const float x = mn.x + charW;
    float y = mn.y + line * 0.5f;
    const std::string world = in.session.world().name.empty() ? std::string("this world") : in.session.world().name;
    dl->AddText(ImVec2(x, y), Ink::strong, fit("Welcome to " + world, width - 2.0f * charW).c_str());
    y += line * 1.25f;
    const model::MapActor* agent = nullptr;
    for (const model::MapActor& a : map_.actors) {
        if (a.kind == model::ActorKind::Agent) {
            agent = &a;
            break;
        }
    }
    const std::string who = agent != nullptr ? agent->name : std::string("Nobody");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - 2.0f * charW);
    ImGui::PushStyleColor(ImGuiCol_Text, Ink::primary);
    ImGui::TextUnformatted((who + " lives here and knows this world. Say hello, or give it a project to work on. "
                                  "Everything it does stays on the record.")
                               .c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(x, mx.y - line * 1.5f));
    if (agent != nullptr) {
        if (bracketButtonNamed("say hello", "welcome-hello", true, false, Ink::nav)) {
            out.focus = agent->id;
            out.view = 0;
        }
        ImGui::SameLine();
    }
    if (bracketButtonNamed("add a project", "welcome-project")) {
        out.action = "workspace.mount";
    }
    ImGui::SameLine();
    if (bracketButtonNamed("new principal", "welcome-principal")) {
        out.action = "principal.create";
    }
}

void GraphPane::drawMenus(const GraphInput& in, GraphActions& out) {
    if (popupWanted_ != nullptr) {
        ImGui::OpenPopup(popupWanted_);
        popupWanted_ = nullptr;
    }
    if (ImGui::BeginPopup("node-menu")) {
        const model::MapActor* a = map_.actor(menuNode_);
        const model::PrincipalInfo* p = in.principals.get(menuNode_);
        const std::string cls = p != nullptr ? p->cls : std::string();
        text(Ink::strong, a != nullptr ? a->name : menuNode_);
        ImGui::Separator();
        if (menuItem("OPEN", nullptr, false, a != nullptr)) {
            out.focus = menuNode_;
            out.view = 0;
        }
        if (menuItem("INSPECT", nullptr, false, a != nullptr)) {
            out.focus = menuNode_;
            out.view = 2;
        }
        const bool connectable = a != nullptr && (a->kind == model::ActorKind::Person || a->kind == model::ActorKind::Agent);
        if (menuItem("CONNECT TO...", nullptr, false, connectable)) {
            armedFrom_ = menuNode_;
        }
        drive::hit("menu:connect", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        // The world's actions, as it offers them: a new action shows up on the next attach.
        if (actions_ != nullptr && actions_->size() > 0 && beginMenu("ACTIONS")) {
            for (const model::ActionInfo* act : actions_->ordered()) {
                bool enabled = a != nullptr;
                json args = json::object();
                if (a != nullptr) {
                    args = actionPrefill(act->name, menuNode_, cls, enabled);
                }
                if (!enabled) {
                    continue; // only what makes sense on this actor
                }
                const std::string label = upper(act->name) + (act->gated ? "  (gated)" : "");
                if (menuItem(label.c_str())) {
                    out.action = act->name;
                    out.actionArgs = args;
                }
                drive::hit("menu:" + act->name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (ImGui::IsItemHovered() && !act->help.empty()) {
                    wrappedTooltip(act->help);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (menuItem("COPY ID")) {
            platform::setClipboard(menuNode_);
        }
        if (menuItem("COPY NAME")) {
            platform::setClipboard(in.principals.nameOf(menuNode_));
        }
        const std::string blocked = model::deleteBlocked(in.principals, menuNode_);
        if (menuItem("DELETE", nullptr, false, blocked.empty())) {
            LinkRequest r;
            r.kind = LinkRequest::Delete;
            r.object = menuNode_;
            symbolUi().ask(std::move(r));
        }
        if (!blocked.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", blocked.c_str());
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("drop-menu")) {
        const std::string fromName = in.principals.nameOf(dropFrom_);
        const std::string toName = in.principals.nameOf(dropTo_);
        const model::MapActor* from = map_.actor(dropFrom_);
        const model::MapActor* to = map_.actor(dropTo_);
        text(Ink::strong, fromName + " onto " + toName);
        ImGui::Separator();
        const model::MapLink* have = map_.link(dropFrom_, dropTo_);
        const bool joined = have != nullptr && have->kind == model::LinkKind::Route;
        const std::string connectLabel = joined ? "ALREADY CONNECTED" : "CONNECT";
        if (menuItem(connectLabel.c_str(), nullptr, false, !joined)) {
            connect(in, dropFrom_, dropTo_);
        }
        drive::hit("menu:drop-connect", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const bool agents = from != nullptr && to != nullptr && from->kind == model::ActorKind::Agent && to->kind == model::ActorKind::Agent;
        const std::string route = agents ? sharedRoute(in, dropTo_) : std::string();
        if (agents) {
            if (menuItem("HAND OVER...", nullptr, false, !route.empty())) {
                handFrom_ = dropFrom_;
                handTo_ = dropTo_;
                handText_ = "Take over from " + fromName + ": ";
            }
            drive::hit("menu:drop-hand", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (route.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("you share no Route with %s", toName.c_str());
            }
        }
        ImGui::EndPopup();
    }
    if (!handTo_.empty() && !ImGui::IsPopupOpen("HAND OVER")) {
        ImGui::OpenPopup("HAND OVER");
    }
}

void GraphPane::drawHandOver(const GraphInput& in) {
    if (!ImGui::BeginPopupModal("HAND OVER", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const std::string toName = in.principals.nameOf(handTo_);
    dimText("a Message to %s, from you, on your Route", toName.c_str());
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 60.0f);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool go = ImGui::InputText("##handover", &handText_, ImGuiInputTextFlags_EnterReturnsTrue);
    drive::hit("input:handover", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    bool close = false;
    dialogGap();
    if (bracketButtonNamed("send", "handover-send") || go) {
        const std::string route = sharedRoute(in, handTo_);
        if (!route.empty()) {
            in.chat.submitTo(handTo_, route, handText_, [this, toName](const std::string& code, const std::string& message) {
                status(code.empty() ? "handed over to " + toName : "hand over: " + code + " " + message, !code.empty());
            });
        }
        close = true;
    }
    ImGui::SameLine();
    if (bracketButtonNamed("cancel", "handover-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        close = true;
    }
    if (close) {
        handTo_.clear();
        handFrom_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GraphPane::drawOverlay(ImFont* mono) {
    // A view's own graph, laid out in layers by the view: a read-only picture.
    constexpr float kNodeW = 200.0f, kNodeH = 58.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float maxX = 0.0f, maxY = 0.0f;
    for (const model::GraphNode& n : overlay_.nodes) {
        maxX = std::max(maxX, n.x + kNodeW);
        maxY = std::max(maxY, n.y + kNodeH);
    }
    for (const model::GraphEdge& e : overlay_.edges) {
        const model::GraphNode* a = overlay_.node(e.from);
        const model::GraphNode* b = overlay_.node(e.to);
        if (a == nullptr || b == nullptr) {
            continue;
        }
        std::vector<ImVec2> pts;
        for (const model::P2& p : model::angularPath(a->x + kNodeW, a->y + kNodeH / 2, b->x, b->y + kNodeH / 2, kNodeH)) {
            pts.push_back(at(origin, p));
        }
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), Ink::nav, 1.0f, ImDrawFlags_None);
        if (!e.label.empty() && pts.size() >= 2) {
            const std::size_t mid = pts.size() / 2;
            dl->AddText(mono, ImGui::GetFontSize() * 0.85f, ImVec2((pts[mid - 1].x + pts[mid].x) / 2, (pts[mid - 1].y + pts[mid].y) / 2 - 14.0f),
                        Ink::nav, e.label.c_str());
        }
    }
    for (const model::GraphNode& n : overlay_.nodes) {
        const ImVec2 mn = at(origin, {n.x, n.y});
        const ImVec2 mx(mn.x + kNodeW, mn.y + kNodeH);
        chrome(dl, mn, mx, ChromeRole::RaisedPane);
        outlineRect(dl, mn, mx, Ink::nav);
        dl->AddText(ImVec2(mn.x + 6.0f, mn.y + 6.0f), Ink::strong, fit(n.name, kNodeW - 12.0f).c_str());
        if (!n.cls.empty()) {
            dl->AddText(mono, ImGui::GetFontSize() * 0.85f, ImVec2(mn.x + 6.0f, mx.y - 6.0f - ImGui::GetFontSize()), Ink::nav,
                        (n.cls + (n.drivenBy.empty() ? "" : " / " + n.drivenBy)).c_str());
        }
    }
    ImGui::Dummy(ImVec2(maxX + 20.0f, maxY + 20.0f));
}

GraphActions GraphPane::drawBody(const GraphInput& in) {
    GraphActions out;
    ImGui::PushFont(in.mono, 0.0f);

    if (!overlayTitle_.empty()) {
        text(Ink::nav, overlayTitle_);
        ImGui::SameLine();
        if (bracketButtonNamed("dismiss", "dismiss-overlay")) {
            clearOverlay();
        }
        ImGui::BeginChild("graph-view", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        touchDragScroll();
        if (!overlayTitle_.empty()) {
            drawOverlay(in.mono);
        }
        ImGui::EndChild();
        ImGui::PopFont();
        return out;
    }

    notePulses(in);
    pressing_ = false;
    if (!armedFrom_.empty()) {
        text(Ink::inspected, "connect " + in.principals.nameOf(armedFrom_) + " to: click the other one (Esc cancels)");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            armedFrom_.clear();
        }
    }

    const float statusH = static_cast<float>(status_.size()) * ImGui::GetTextLineHeightWithSpacing();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(60.0f, avail.y - statusH);
    ImGui::BeginChild("map", avail, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    if (twilight) {
        drawTwilight(ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowWidth(), ImGui::GetWindowHeight()));
    }
    if (dragFrom_.empty()) {
        touchDragScroll();
    }
    rebuild(in, ImGui::GetContentRegionAvail().x);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    hovered_.clear();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (const model::MapActor* under = ImGui::IsWindowHovered() ? actorAt(mouse, origin) : nullptr) {
        hovered_ = under->id; // links light before the node's own button runs
    }

    if (map_.actors.empty()) {
        dimText("%s", in.session.attached() ? "reading the world..." : "not attached");
    }
    for (const model::MapHeading& h : map_.headings) {
        const ImVec2 p = at(origin, {h.x, h.y});
        dl->AddText(p, Ink::secondary, upper(h.text).c_str());
    }
    drawLinks(dl, origin, in.anim);
    for (const model::MapActor& a : map_.actors) {
        if (const model::MapActor* parent = a.parent.empty() ? nullptr : map_.actor(a.parent); parent != nullptr && !map_.stacked) {
            const float x = origin.x + std::floor(parent->x + (a.x - parent->x) / 2.0f);
            const float top = origin.y + std::floor(parent->bottom());
            const float mid = origin.y + std::floor(a.y + ImGui::GetTextLineHeightWithSpacing());
            dl->AddLine(ImVec2(x, top), ImVec2(x, mid), Ink::secondary);
            dl->AddLine(ImVec2(x, mid), ImVec2(origin.x + a.x, mid), Ink::secondary);
        }
    }
    for (const model::MapActor& a : map_.actors) {
        drawActor(in, out, a, origin);
    }
    if (welcome_) {
        drawWelcome(in, out, origin);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(map_.width, map_.height));

    // The drag: a rubber band to the mouse, and a drop on whoever is under it.
    if (!dragFrom_.empty()) {
        if (const model::MapActor* from = map_.actor(dragFrom_)) {
            const ImVec2 start(origin.x + from->right(), origin.y + from->midY());
            dl->AddLine(start, ImVec2(mouse.x, start.y), Ink::inspected);
            dl->AddLine(ImVec2(mouse.x, start.y), mouse, Ink::inspected);
        }
        const model::MapActor* target = actorAt(mouse, origin);
        if (target != nullptr && target->id != dragFrom_) {
            dl->AddRect(at(origin, {target->x, target->y}), at(origin, {target->right(), target->bottom()}), Ink::inspected, 0.0f, 2.0f);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (target != nullptr && target->id != dragFrom_) {
                if (target->kind == model::ActorKind::Person || target->kind == model::ActorKind::Agent) {
                    landed(dragFrom_, target->id);
                } else {
                    status("connect people and agents; a Model is bound in FIELDS", true);
                }
            }
            dragFrom_.clear();
        }
    }
    drawMenus(in, out);
    drawHandOver(in);
    ImGui::EndChild();
    for (const Status& s : status_) {
        text(s.err ? Ink::inspected : Ink::secondary, s.text);
    }
    ImGui::PopFont();
    return out;
}

} // namespace ke::ui
