// The editor: a buffer held by the world, shown and edited here. A keystroke changes the local
// TextBuffer at once and goes to the world as `insert` or `replace`; a model's edits arrive the
// same way in reverse and are marked with who made them. A refused edit is shown as it came.
// The same buffer can sit on a Field: it wraps by default, since a Field is usually prose, and a
// Markdown one can be read rendered. Save is then a set-field, and the world says so when the Field
// moved underneath.
#include "modules/Module.hpp"
#include "modules/ModuleUi.hpp"
#include "modules/RenderedDoc.hpp"
#include "modules/TextBuffer.hpp"

#include "drive/Commands.hpp"
#include "ui/Markdown.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace ke::modules {

namespace {

class EditorModule final : public Module {
  public:
    EditorModule(Host& host, const model::PaneInfo& p) : host_(host), id_(p.id), field_(p.field), wrap_(!p.field.empty()) {}

    void goToLine(int line) override { wantLine_ = line; }

    void restore(const model::PaneInfo&) override {
        if (!loaded_) {
            loaded_ = true;
            reload(false);
            host_.act("pane.subscribe", {{"pane", id_}}, [](const Response&) {});
        }
    }

    // The world's stream: the whole text when full, then one edit at a time.
    void data(const PaneData& d) override {
        if (d.state.is_object() && d.state.contains("version") && d.state["version"].is_number_integer()) {
            version_ = d.state["version"].get<long long>();
        }
        if (d.state.is_object() && d.state.contains("stale")) {
            stale(d.state["stale"].is_boolean() && d.state["stale"].get<bool>());
        }
        if (d.full) {
            if (d.data != buf_.text()) {
                buf_.setText(d.data, true);
                rendered_doc_.invalidate();
            }
            // Dirty is the world's word: it holds the buffer, and a reconnect must not forget that.
            if (d.state.is_object() && d.state.contains("holder")) {
                if (d.state["holder"].is_null()) {
                    buf_.markSaved();
                } else {
                    buf_.markDirty();
                }
            }
            return;
        }
        if (!d.edit.is_object()) {
            return;
        }
        version_ = d.edit.value("version", version_);
        const std::string who = d.edit.value("who", std::string());
        if (who.empty() || who == host_.self()) {
            return; // this client's own edit, already in the buffer
        }
        buf_.applyRemote(d.edit.value("at", std::size_t{0}), d.edit.value("del", std::size_t{0}), d.edit.value("text", std::string()));
        attribution_ = host_.nameOf(who);
        attributionAt_ = ImGui::GetTime();
    }

    void driven(const model::PaneInput&, const std::string& who) override {
        attribution_ = who;
        attributionAt_ = ImGui::GetTime();
    }

    std::string closeWarning() const override {
        return buf_.dirty() ? "unsaved changes in this buffer" : "";
    }

    void keyboard() override {
        const ImGuiIO& io = ImGui::GetIO();
        if (shortcut(ImGuiKey_S)) {
            save();
            return;
        }
        if (rendered_) {
            return; // reading: the text is not under the keys
        }
        if (shortcut(ImGuiKey_F)) {
            finding_ = true;
            focusFind_ = true;
            return;
        }
        if (io.WantTextInput) {
            return; // the find field has the keyboard
        }
        const TextPos was = buf_.cursor();
        const bool shift = io.KeyShift;
        const bool word = io.KeyAlt;
        const bool doc = io.KeyCtrl;
        const auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
        if (doc && pressed(ImGuiKey_Z)) {
            if (shift) {
                buf_.redo();
            } else {
                buf_.undo();
            }
        } else if (doc && pressed(ImGuiKey_A)) {
            buf_.selectAll();
        } else if (doc && pressed(ImGuiKey_C)) {
            if (buf_.hasSelection()) {
                host_.copy(buf_.selectedText());
            }
        } else if (doc && pressed(ImGuiKey_X)) {
            if (buf_.hasSelection()) {
                host_.copy(buf_.selectedText());
                buf_.insert("");
            }
        } else if (doc && pressed(ImGuiKey_V)) {
            pasteText(host_.paste());
        } else if (pressed(ImGuiKey_LeftArrow)) {
            buf_.moveLeft(shift, word);
        } else if (pressed(ImGuiKey_RightArrow)) {
            buf_.moveRight(shift, word);
        } else if (pressed(ImGuiKey_UpArrow)) {
            if (doc) {
                buf_.moveDocStart(shift);
            } else {
                moveRows(-1, shift);
            }
        } else if (pressed(ImGuiKey_DownArrow)) {
            if (doc) {
                buf_.moveDocEnd(shift);
            } else {
                moveRows(1, shift);
            }
        } else if (pressed(ImGuiKey_PageUp)) {
            moveRows(-pageLines_, shift);
        } else if (pressed(ImGuiKey_PageDown)) {
            moveRows(pageLines_, shift);
        } else if (pressed(ImGuiKey_Home)) {
            buf_.moveHome(shift);
        } else if (pressed(ImGuiKey_End)) {
            buf_.moveEnd(shift);
        } else if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
            newline();
        } else if (pressed(ImGuiKey_Tab)) {
            buf_.insert("    ");
        } else if (pressed(ImGuiKey_Backspace)) {
            buf_.backspace(word);
        } else if (pressed(ImGuiKey_Delete)) {
            buf_.del(word);
        } else if (!doc) {
            std::string typed;
            for (const ImWchar c : io.InputQueueCharacters) {
                char utf8[5] = {};
                typed.append(utf8, static_cast<std::size_t>(ImTextCharToUtf8(utf8, c)));
            }
            if (!typed.empty()) {
                buf_.insert(typed);
            }
        }
        ImGui::GetIO().InputQueueCharacters.resize(0);
        // Only when the cursor moved: this runs every focused frame, and following then undid every scroll.
        follow_ = follow_ || !(buf_.cursor() == was);
        sendEdits();
    }

    void draw(Rect r, bool focused) override {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float lh = std::floor(ImGui::GetTextLineHeight() + 1.0f);
        const float cw = ImGui::CalcTextSize("0").x;
        dl->AddRectFilled(topLeft(r), bottomRight(r), ui::Surface::background);
        float top = r.y;
        const float statusH = lh + 2.0f;
        const float findH = finding_ ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
        Rect body{r.x, top, r.w, r.h - statusH - findH};
        pageLines_ = std::max(1, static_cast<int>(body.h / lh) - 1);
        const int digits = std::max(3, static_cast<int>(std::log10(std::max(1, buf_.lineCount()))) + 1);
        const float gutter = cw * static_cast<float>(digits + 1) + 6.0f;
        const float bodyX = r.x + gutter;
        layoutRows(body.w - gutter - cw);
        canRender_ = !field_.empty() && ui::looksLikeMarkdown(buf_.text());
        if (rendered_ && !canRender_) {
            rendered_ = false;
        }

        const ImGuiIO& io = ImGui::GetIO();
        const bool over = hoveredRect(body);
        if (rendered_) {
            drawRendered(body);
        } else {
            drawText(r, body, focused, digits, bodyX, lh, cw, over);
        }

        if (finding_) {
            ImGui::SetCursorScreenPos(ImVec2(r.x + 4.0f, r.y + r.h - statusH - findH + 2.0f));
            drawFind(r.w);
        }
        // The status line: where, whether saved, who just edited, what the world said.
        const float sy = r.y + r.h - statusH;
        dl->AddLine(ImVec2(r.x, sy), ImVec2(r.x + r.w, sy), ui::Surface::codeBorder);
        std::string left = buf_.lineCol() + (buf_.dirty() ? "  modified" : "  saved");
        ImU32 ink = ui::Ink::secondary;
        if (ImGui::GetTime() - attributionAt_ < 4.0 && !attribution_.empty()) {
            left += "  edited by " + attribution_;
            ink = ui::Ink::inspected;
        }
        // Controls first, so a long status can never push one off the pane.
        ImGui::SetCursorScreenPos(ImVec2(r.x + 4.0f, sy + 1.0f));
        ImGui::PushID(id_.c_str());
        if (ui::bracketButtonNamed("SAVE", "editor-save", buf_.dirty())) {
            save();
        }
        ImGui::SameLine();
        // Reverting has no undo, so the first press only asks.
        const bool armed = ImGui::GetTime() - revertArmedAt_ < 3.0;
        if (ui::bracketButtonNamed(armed ? "REVERT: SURE?" : "REVERT", "editor-revert", buf_.dirty() || conflict_, armed)) {
            if (armed) {
                revertArmedAt_ = -100.0;
                revert();
            } else {
                revertArmedAt_ = ImGui::GetTime();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("drop these edits and go back to what %s holds now", field_.empty() ? "the file" : field_.c_str());
        }
        // On a narrow pane a conflict takes the row: what decides it matters more than the view.
        const bool room = !conflict_ || r.w > cw * 72.0f;
        ImGui::SameLine();
        if (ui::bracketButtonNamed("FIND", "editor-find", true, finding_)) {
            finding_ = !finding_;
            focusFind_ = finding_;
        }
        if (room) {
            ImGui::SameLine();
            if (ui::bracketButtonNamed("WRAP", "editor-wrap", true, wrap_)) {
                wrap_ = !wrap_;
                scroll_ = 0;
                follow_ = true;
                rowsFor_ = 0;
            }
        }
        if (room && canRender_) {
            ImGui::SameLine();
            if (ui::bracketButtonNamed(rendered_ ? "SOURCE" : "RENDERED", "editor-rendered", true, rendered_)) {
                rendered_ = !rendered_;
                rendered_doc_.invalidate();
            }
        }
        if (conflict_) {
            ImGui::SameLine();
            if (ui::bracketButtonNamed("SAVE OVER IT", "editor-force")) {
                frob("save", {{"force", true}}, [this] {
                    buf_.markSaved();
                    stale(false);
                });
            }
        }
        ImGui::SameLine();
        const float textX = ImGui::GetCursorScreenPos().x + 4.0f;
        dl->PushClipRect(ImVec2(textX, sy), ImVec2(r.x + r.w, sy + statusH), true);
        dl->AddText(ImVec2(textX, sy + 2.0f), ink, left.c_str());
        if (!status_.empty()) {
            const float w = ImGui::CalcTextSize(status_.c_str()).x;
            const float x = std::max(textX + ImGui::CalcTextSize(left.c_str()).x + 12.0f, r.x + r.w - w - 4.0f);
            dl->AddText(ImVec2(x, sy + 2.0f), statusBad_ ? ui::Ink::fatal : ui::Ink::connected, status_.c_str());
        }
        dl->PopClipRect();
        ImGui::PopID();
    }

    nlohmann::json describe() const override {
        return {{"kind", "editor"}, {"text", buf_.text()}, {"dirty", buf_.dirty()}, {"cursor", buf_.lineCol()},
                {"status", status_}, {"lines", buf_.lineCount()}, {"version", version_}, {"wrap", wrap_},
                {"rows", rows_.size()}, {"scroll", scroll_}, {"rendered", rendered_}, {"conflict", conflict_}, {"field", field_}};
    }

    std::string selection() const override { return buf_.selectedText(); }

    void pasteText(const std::string& text) override {
        if (!text.empty()) {
            buf_.insert(text);
            sendEdits();
        }
    }

    std::string heldError() const override { return statusBad_ ? status_ : std::string(); }

  private:
    // One drawn row: part of a line. With wrap off, a row is a whole line.
    struct Row {
        int line = 0;
        int start = 0, end = 0;
    };

    void layoutRows(float width) {
        if (rowsFor_ == buf_.revision() + 1 && rowsWidth_ == width) {
            return;
        }
        rowsFor_ = buf_.revision() + 1;
        rowsWidth_ = width;
        rows_.clear();
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        for (int i = 0; i < buf_.lineCount(); ++i) {
            const std::string& line = buf_.line(i);
            if (!wrap_ || line.empty()) {
                rows_.push_back(Row{i, 0, static_cast<int>(line.size())});
                continue;
            }
            const char* begin = line.data();
            const char* end = begin + line.size();
            const char* p = begin;
            while (p < end) {
                const char* q = font->CalcWordWrapPosition(size, p, end, std::max(width, size));
                if (q <= p) {
                    q = p + 1;
                    while (q < end && (static_cast<unsigned char>(*q) & 0xC0) == 0x80) {
                        ++q;
                    }
                }
                while (q < end && *q == ' ') {
                    ++q; // spaces end a row: they never start the next
                }
                rows_.push_back(Row{i, static_cast<int>(p - begin), static_cast<int>(q - begin)});
                p = q;
            }
        }
    }

    // The row a position is on. The end of a wrapped row belongs to the row after it.
    int rowOf(TextPos pos) const {
        int found = 0;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const Row& r = rows_[static_cast<std::size_t>(i)];
            if (r.line > pos.line) {
                break;
            }
            if (r.line == pos.line && r.start <= pos.col) {
                found = i;
            } else if (r.line < pos.line) {
                found = i;
            }
        }
        return found;
    }

    void moveRows(int delta, bool shift) {
        if (rows_.empty() || !wrap_) {
            if (delta < 0) {
                buf_.moveUp(shift, -delta);
            } else {
                buf_.moveDown(shift, delta);
            }
            return;
        }
        const int at = rowOf(buf_.cursor());
        const Row from = rows_[static_cast<std::size_t>(at)];
        const int to = std::clamp(at + delta, 0, static_cast<int>(rows_.size()) - 1);
        const Row target = rows_[static_cast<std::size_t>(to)];
        if (from.line >= buf_.lineCount() || target.line >= buf_.lineCount()) {
            return; // the text moved since these rows were laid out; the next frame has them right
        }
        const float x = xOf(buf_.line(from.line), from.start, buf_.cursor().col);
        buf_.moveTo(TextPos{target.line, colAt(buf_.line(target.line), target, x)}, shift);
    }

    void drawText(Rect r, Rect body, bool focused, int digits, float bodyX, float lh, float cw, bool over) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        const int total = static_cast<int>(rows_.size());
        drive::hit("editor-body", topLeft(body), bottomRight(body));
        if (wantLine_ > 0 && total > 0 && buf_.lineCount() > 1) {
            buf_.moveTo(TextPos{std::min(wantLine_, buf_.lineCount()) - 1, 0}, false);
            scroll_ = std::max(0, rowOf(buf_.cursor()) - 2);
            scrollAcc_ = 0.0f;
            wantLine_ = 0;
        }
        if (follow_) {
            follow_ = false;
            const int row = rowOf(buf_.cursor());
            const int visible = std::max(1, static_cast<int>(body.h / lh));
            scroll_ = std::clamp(scroll_, std::max(0, row - visible + 1), std::max(row, 0));
        }
        if (over) {
            // A trackpad sends fractions of a notch, so they add up: cut to a whole line each, they were all lost.
            scrollAcc_ -= io.MouseWheel * 3.0f;
            if (io.MouseSource == ImGuiMouseSource_TouchScreen && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8.0f)) {
                scrollAcc_ -= io.MouseDelta.y / lh;
            }
            const int step = static_cast<int>(scrollAcc_);
            scroll_ += step;
            scrollAcc_ -= static_cast<float>(step);
        }
        scroll_ = std::clamp(scroll_, 0, std::max(0, total - 1));

        dl->PushClipRect(topLeft(body), bottomRight(body), true);
        const TextPos a = std::min(buf_.cursor(), buf_.anchor()), b = std::max(buf_.cursor(), buf_.anchor());
        const int last = std::min(total, scroll_ + static_cast<int>(body.h / lh) + 2);
        const int cursorRow = rowOf(buf_.cursor());
        for (int i = scroll_; i < last; ++i) {
            const Row& row = rows_[static_cast<std::size_t>(i)];
            if (row.line >= buf_.lineCount()) {
                continue;
            }
            const float y = body.y + static_cast<float>(i - scroll_) * lh;
            const std::string& line = buf_.line(row.line);
            if (row.start == 0) {
                char num[16];
                std::snprintf(num, sizeof num, "%*d", digits, row.line + 1);
                dl->AddText(ImVec2(r.x + 2.0f, y), row.line == buf_.cursor().line ? ui::Ink::nav : ui::Ink::secondary, num);
            }
            const TextPos rowFrom{row.line, row.start}, rowTo{row.line, row.end};
            const bool selected = buf_.hasSelection() && !(rowTo < a) && !(b < rowFrom);
            if (selected) {
                const int from = a.line == row.line ? std::max(a.col, row.start) : row.start;
                const bool runsOn = b.line > row.line || (b.line == row.line && b.col > row.end);
                const int to = b.line == row.line ? std::min(b.col, row.end) : row.end;
                const float ex = xOf(line, row.start, to) + (runsOn && row.end == static_cast<int>(line.size()) ? cw : 0.0f);
                dl->AddRectFilled(ImVec2(bodyX + xOf(line, row.start, from), y), ImVec2(bodyX + ex, y + lh), ui::Surface::selection);
            }
            if (row.end > row.start) {
                dl->AddText(ImVec2(bodyX, y), selected ? ui::Ink::onSelection : ui::Ink::primary, line.c_str() + row.start,
                            line.c_str() + row.end);
            }
            if (focused && i == cursorRow) {
                const bool on = std::fmod(ImGui::GetTime(), ui::kBlinkPeriod * 2) < ui::kBlinkPeriod;
                const float cx = bodyX + xOf(line, row.start, buf_.cursor().col);
                if (on) {
                    dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + 2.0f, y + lh), ui::Ink::inspected);
                }
            }
        }
        dl->PopClipRect();

        // The mouse.
        const auto posAt = [&](ImVec2 m) {
            const int i = std::clamp(scroll_ + static_cast<int>((m.y - body.y) / lh), 0, std::max(0, total - 1));
            const Row& row = rows_[static_cast<std::size_t>(i)];
            return TextPos{row.line, colAt(buf_.line(row.line), row, m.x - bodyX)};
        };
        if (over && total > 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const TextPos p = posAt(io.MousePos);
            if (ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) >= 2) {
                buf_.selectWord(p);
            } else {
                buf_.moveTo(p, io.KeyShift);
                dragging_ = true;
            }
            host_.wantKeyboard(true);
        }
        if (dragging_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (total > 0 && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
                    buf_.moveTo(posAt(io.MousePos), true);
                }
            } else {
                dragging_ = false;
            }
        }
    }

    // Reading a Markdown Field as it will read. The text cannot change here, so it is safe to scroll.
    void drawRendered(Rect body) {
        ImGui::SetCursorScreenPos(topLeft(body));
        ImGui::PushID(id_.c_str());
        if (ImGui::BeginChild("rendered", ImVec2(body.w, body.h), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
            ui::touchDragScroll();
            rendered_doc_.draw(buf_.text(), true, ImGui::GetContentRegionAvail().x, id_.c_str());
        }
        ImGui::EndChild();
        ImGui::PopID();
    }

    // x of a column on a row.
    float xOf(const std::string& line, int start, int col) const {
        const int to = std::clamp(col, start, static_cast<int>(line.size()));
        return ImGui::CalcTextSize(line.c_str() + start, line.c_str() + to).x;
    }

    int colAt(const std::string& line, const Row& row, float x) const {
        int col = row.start;
        const int stop = std::min(row.end, static_cast<int>(line.size()));
        while (col < stop) {
            int next = col + 1;
            while (next < stop && (static_cast<unsigned char>(line[static_cast<std::size_t>(next)]) & 0xC0) == 0x80) {
                ++next;
            }
            const float left = xOf(line, row.start, col), right = xOf(line, row.start, next);
            if (right - (right - left) / 2 > x) {
                break;
            }
            col = next;
        }
        return col;
    }

    void newline() {
        // Keep the indent.
        const std::string& line = buf_.line(buf_.cursor().line);
        std::string indent;
        for (char c : line) {
            if (c != ' ' && c != '\t') {
                break;
            }
            indent += c;
        }
        buf_.insert("\n" + indent);
    }

    void reload(bool foreign) {
        host_.act("pane.read", {{"pane", id_}, {"what", "buffer"}}, [this, foreign](const Response& r) {
            if (!r.ok) {
                status_ = errLine(r);
                statusBad_ = true;
                return;
            }
            std::string text;
            if (r.result.is_string()) {
                text = r.result.get<std::string>();
            } else if (r.result.is_object() && r.result.contains("text") && r.result["text"].is_string()) {
                text = r.result["text"].get<std::string>();
            }
            if (r.result.is_object() && r.result.contains("version") && r.result["version"].is_number_integer()) {
                version_ = r.result["version"].get<long long>();
            }
            if (text != buf_.text()) {
                buf_.setText(text, foreign);
            }
        });
    }

    void sendEdits() {
        for (TextEdit& e : buf_.takeEdits()) {
            const bool pure = e.start == e.end;
            nlohmann::json args = pure ? nlohmann::json{{"at", e.start}, {"text", e.text}}
                                       : nlohmann::json{{"at", e.start}, {"del", e.end - e.start}, {"with", e.text}};
            args["version"] = version_ + inFlight_++;
            frob(pure ? "insert" : "replace", std::move(args));
        }
    }

    void frob(const std::string& verb, nlohmann::json args, std::function<void()> done = {}) {
        host_.act("pane.frob", {{"pane", id_}, {"verb", verb}, {"args", std::move(args)}}, [this, done, verb](const Response& r) {
            inFlight_ = std::max(0, inFlight_ - (verb == "insert" || verb == "replace" ? 1 : 0));
            if (r.ok && r.result.is_object() && r.result.contains("version") && r.result["version"].is_number_integer()) {
                version_ = std::max(version_, r.result["version"].get<long long>());
            }
            if (!r.ok) {
                status_ = errLine(r);
                statusBad_ = true;
                conflict_ = r.err && r.err->code == "conflict";
                if (conflict_ && verb != "save") {
                    reload(true); // someone else holds the buffer: theirs wins, and the line says so
                }
            } else if (verb == "save") {
                conflict_ = false;
                status_ = "saved";
                statusBad_ = false;
            } else if (statusBad_) {
                status_.clear();
                statusBad_ = false;
            }
            if (r.ok && done) {
                done();
            }
        });
    }

    void save() {
        sendEdits();
        status_ = "saving";
        statusBad_ = false;
        frob("save", nlohmann::json::object(), [this] { buf_.markSaved(); });
    }

    void revert() {
        buf_.takeEdits();
        status_ = "reverting";
        statusBad_ = false;
        frob("revert", nlohmann::json::object(), [this] {
            buf_.markSaved();
            stale(false);
            status_ = "reverted";
            rendered_doc_.invalidate();
        });
    }

    // The Field moved under a held buffer: the holder decides, and nothing of theirs is touched.
    void stale(bool on) {
        conflict_ = on;
        if (on) {
            status_ = "changed elsewhere"; // the tab says what did; the row has little room
            statusBad_ = true;
        } else if (statusBad_) {
            status_.clear();
            statusBad_ = false;
        }
    }

    void drawFind(float width) {
        ImGui::PushID(id_.c_str());
        ui::dimText("FIND");
        ImGui::SameLine();
        if (focusFind_) {
            ImGui::SetKeyboardFocusHere();
            focusFind_ = false;
        }
        ImGui::SetNextItemWidth(width * 0.4f);
        const bool entered = ImGui::InputText("##find", &needle_, ImGuiInputTextFlags_EnterReturnsTrue);
        drive::hit("input:find", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        const int n = buf_.matchCount(needle_);
        ui::dimText(n == 1 ? "%d match" : "%d matches", n);
        ImGui::SameLine();
        if (ui::bracketButtonNamed("NEXT", "find-next", n > 0) || entered) {
            buf_.find(needle_, !ImGui::GetIO().KeyShift);
            follow_ = true;
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (ui::bracketButtonNamed("x", "find-close")) {
            finding_ = false;
        }
        ImGui::PopID();
    }

    Host& host_;
    std::string id_;
    std::string field_; // the Field this buffer sits on, if it is no file
    bool wrap_ = false;
    bool rendered_ = false, canRender_ = false;
    RenderedDoc rendered_doc_;
    std::vector<Row> rows_;
    unsigned long long rowsFor_ = 0; // the buffer's revision + 1; 0 says lay out again
    float rowsWidth_ = 0.0f;
    double revertArmedAt_ = -100.0;
    TextBuffer buf_;
    long long version_ = 0;
    int inFlight_ = 0;
    bool conflict_ = false;
    bool loaded_ = false, follow_ = false, dragging_ = false, finding_ = false, focusFind_ = false, statusBad_ = false;
    int scroll_ = 0, pageLines_ = 20, wantLine_ = 0;
    float scrollAcc_ = 0.0f;
    std::string needle_, status_, attribution_;
    double attributionAt_ = -100.0;
};

} // namespace

std::unique_ptr<Module> makeEditor(Host& host, const model::PaneInfo& pane) {
    return std::make_unique<EditorModule>(host, pane);
}

} // namespace ke::modules
