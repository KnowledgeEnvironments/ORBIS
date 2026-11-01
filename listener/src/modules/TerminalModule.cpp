// A terminal pane. The process and its truth live in the world; this is a libvterm screen fed by
// `pane.subscribe`, drawn cell by cell in theme colours, with input encoded for the program and
// sent through `pane.send`.
#include "modules/Module.hpp"
#include "modules/TermGlyphs.hpp"
#include "modules/ModuleUi.hpp"
#include "modules/TerminalScreen.hpp"

#include "drive/Commands.hpp"
#include "platform/Platform.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <imgui_internal.h>

namespace ke::modules {

namespace {

using Clock = std::chrono::steady_clock;

ImU32 colourOf(const TermColor& c, ImU32 dflt) {
    switch (c.kind) {
    case TermColor::Default:
        return dflt;
    case TermColor::Indexed:
        return ui::ansiInk(c.index);
    case TermColor::Rgb:
        return ui::rgbInk(c.r, c.g, c.b);
    }
    return dflt;
}

struct KeyMap {
    ImGuiKey key;
    TermKey term;
};

constexpr KeyMap kKeys[] = {
    {ImGuiKey_Enter, TermKey::Enter}, {ImGuiKey_KeypadEnter, TermKey::Enter}, {ImGuiKey_Tab, TermKey::Tab},
    {ImGuiKey_Backspace, TermKey::Backspace}, {ImGuiKey_Escape, TermKey::Escape}, {ImGuiKey_UpArrow, TermKey::Up},
    {ImGuiKey_DownArrow, TermKey::Down}, {ImGuiKey_LeftArrow, TermKey::Left}, {ImGuiKey_RightArrow, TermKey::Right},
    {ImGuiKey_Insert, TermKey::Insert}, {ImGuiKey_Delete, TermKey::Delete}, {ImGuiKey_Home, TermKey::Home},
    {ImGuiKey_End, TermKey::End}, {ImGuiKey_PageUp, TermKey::PageUp}, {ImGuiKey_PageDown, TermKey::PageDown},
    {ImGuiKey_F1, TermKey::F1}, {ImGuiKey_F2, TermKey::F2}, {ImGuiKey_F3, TermKey::F3}, {ImGuiKey_F4, TermKey::F4},
    {ImGuiKey_F5, TermKey::F5}, {ImGuiKey_F6, TermKey::F6}, {ImGuiKey_F7, TermKey::F7}, {ImGuiKey_F8, TermKey::F8},
    {ImGuiKey_F9, TermKey::F9}, {ImGuiKey_F10, TermKey::F10}, {ImGuiKey_F11, TermKey::F11}, {ImGuiKey_F12, TermKey::F12},
};

class TerminalModule final : public Module {
  public:
    TerminalModule(Host& host, const model::PaneInfo& p) : host_(host), id_(p.id), screen_(p.rows > 0 ? p.rows : 24, p.cols > 0 ? p.cols : 80) {}

    void restore(const model::PaneInfo& p) override {
        exited_ = !p.open();
        exitCode_ = p.exitCode;
        hasExit_ = p.hasExit;
        if (!subscribed_ && !exited_) {
            subscribed_ = true;
            host_.act("pane.subscribe", {{"pane", id_}}, [this](const Response& r) {
                if (!r.ok) {
                    status_ = errLine(r);
                }
            });
        } else if (exited_ && !readBack_) {
            // The process is gone; what it left is still there to read.
            readBack_ = true;
            host_.act("pane.read", {{"pane", id_}, {"what", "scrollback"}}, [this](const Response& r) {
                if (!r.ok || !r.result.is_object() || screen_.screenText().find_first_not_of(" \n") != std::string::npos) {
                    return;
                }
                if (r.result.contains("lines") && r.result["lines"].is_array()) {
                    for (const nlohmann::json& l : r.result["lines"]) {
                        if (l.is_string()) {
                            screen_.feed(l.get<std::string>() + "\r\n");
                        }
                    }
                } else if (r.result.contains("text") && r.result["text"].is_string()) {
                    screen_.feed(r.result["text"].get<std::string>());
                }
            });
        }
    }

    void data(const PaneData& d) override {
        if (d.exited) {
            exited_ = true;
            hasExit_ = d.exitCode.has_value();
            exitCode_ = d.exitCode.value_or(0);
            return;
        }
        if (d.base && *d.base != lastSeq_ && !d.full) {
            // A gap: a dropped frame leaves the screen wrong, so ask from where this client stands.
            host_.act("pane.subscribe", {{"pane", id_}, {"after", lastSeq_}}, [](const Response&) {});
            return;
        }
        lastSeq_ = d.seq;
        if (d.full) {
            screen_.reset();
            if (d.cols > 0 && d.rows > 0 && (d.cols != screen_.cols() || d.rows != screen_.rows())) {
                screen_.resize(d.rows, d.cols);
            }
        }
        screen_.feed(d.data);
        view_ = std::min(view_, static_cast<int>(screen_.scrollback().size()));
    }

    void keyboard() override {
        if (exited_) {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        TermMods mods;
        mods.shift = io.KeyShift;
        mods.alt = io.KeyAlt;
        mods.ctrl = physicalCtrl();
        const bool mac = io.ConfigMacOSXBehaviors;
        const bool cmd = mac && io.KeyCtrl;
        // Copy and paste belong to the client: Cmd on a Mac, Ctrl-Shift elsewhere.
        const bool clipboardChord = mac ? cmd : (io.KeyCtrl && io.KeyShift);
        if (clipboardChord && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            if (hasSelection()) {
                host_.copy(selectedText());
            }
        } else if (clipboardChord && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            pasteText(host_.paste());
        } else if (!cmd) {
            for (const KeyMap& k : kKeys) {
                if (ImGui::IsKeyPressed(k.key, true)) {
                    pending_ += screen_.encodeKey(k.term, mods);
                    jumpToLive();
                }
            }
            if (mods.ctrl) {
                for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k) {
                    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), true)) {
                        const char c = static_cast<char>('a' + (k - ImGuiKey_A));
                        pending_ += screen_.encodeText(std::string_view(&c, 1), mods);
                        jumpToLive();
                    }
                }
            }
        }
        if (!mods.ctrl && !cmd) {
            for (const ImWchar c : io.InputQueueCharacters) {
                char utf8[5] = {};
                const int n = ImTextCharToUtf8(utf8, c);
                pending_ += screen_.encodeText(std::string_view(utf8, static_cast<std::size_t>(n)), TermMods{});
                jumpToLive();
            }
        }
        ImGui::GetIO().InputQueueCharacters.resize(0);
        flush();
    }

    void draw(Rect r, bool focused) override {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        const float cw = ImGui::CalcTextSize("0").x;
        const float ch = std::floor(ImGui::GetTextLineHeight());
        const bool touch = platform::touchScreen();
        const float softH = touch ? ch * 2.0f : 0.0f;
        Rect grid = r;
        grid.h -= softH;
        fit(grid, cw, ch);

        dl->AddRectFilled(topLeft(r), bottomRight(r), ui::Surface::background);
        dl->PushClipRect(topLeft(grid), bottomRight(grid), true);
        const int rows = screen_.rows();
        const int cols = screen_.cols();
        const int back = static_cast<int>(screen_.scrollback().size());
        const int firstAbs = back - view_;
        for (int row = 0; row < rows; ++row) {
            const int abs = firstAbs + row;
            const TermLine cells = lineAt(abs);
            const float y = grid.y + static_cast<float>(row) * ch;
            float x = grid.x;
            std::string run;
            ImU32 runFg = 0;
            float runX = x;
            const auto flushRun = [&] {
                if (!run.empty()) {
                    dl->AddText(font, fontSize, ImVec2(runX, y), runFg, run.c_str());
                    run.clear();
                }
            };
            const int n = std::min(cols, static_cast<int>(cells.size()));
            for (int c = 0; c < n; ++c) {
                const TermCell& cell = cells[static_cast<std::size_t>(c)];
                if (cell.width == 0) {
                    x += cw;
                    continue;
                }
                ImU32 fg = colourOf(cell.fg, cell.bold ? ui::Ink::strong : ui::Ink::primary);
                ImU32 bg = colourOf(cell.bg, 0);
                if (cell.reverse) {
                    const ImU32 t = fg;
                    fg = bg != 0 ? bg : ui::Surface::background;
                    bg = t;
                }
                const bool chosen = selected(abs, c);
                if (chosen) {
                    fg = ui::Ink::onSelection;
                    bg = ui::Surface::selection;
                }
                const float w = cw * static_cast<float>(std::max(1, cell.width));
                if (bg != 0) {
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + ch), bg);
                }
                if (cell.underline) {
                    dl->AddLine(ImVec2(x, y + ch - 1.0f), ImVec2(x + w, y + ch - 1.0f), fg);
                }
                if (cell.strike) {
                    dl->AddLine(ImVec2(x, y + ch / 2), ImVec2(x + w, y + ch / 2), fg);
                }
                if (cell.text.empty() || cell.text == " ") {
                    flushRun();
                    x += w;
                    runX = x;
                    continue;
                }
                // Only ASCII runs together: a font's other glyphs need not be a cell wide, and a run
                // lets one of them move everything after it off the grid.
                if (static_cast<unsigned char>(cell.text[0]) >= 0x80 || cell.text.size() > 1) {
                    flushRun();
                    if (!drawCellGlyph(dl, soleCodePoint(cell.text), ImVec2(x, y), w, ch, fg)) {
                        dl->AddText(font, fontSize, ImVec2(x, y), fg, cell.text.c_str());
                    }
                    x += w;
                    runX = x;
                    continue;
                }
                if (fg != runFg || cell.width != 1) {
                    flushRun();
                    runFg = fg;
                    runX = x;
                }
                run += cell.text;
                if (cell.width != 1) {
                    flushRun();
                    runX = x + w;
                }
                x += w;
            }
            flushRun();
        }
        drawCursor(dl, grid, cw, ch, focused);
        dl->PopClipRect();
        if (view_ > 0) {
            const std::string where = "scrollback -" + std::to_string(view_);
            dl->AddText(ImVec2(grid.x + grid.w - ImGui::CalcTextSize(where.c_str()).x - 4.0f, grid.y), ui::Ink::inspected, where.c_str());
        }
        if (exited_) {
            const std::string line = hasExit_ ? "exited " + std::to_string(exitCode_) + ", scrollback kept" : "exited, scrollback kept";
            dl->AddText(ImVec2(grid.x + 4.0f, grid.y + grid.h - ch), ui::Ink::inspected, line.c_str());
        } else if (!status_.empty()) {
            dl->AddText(ImVec2(grid.x + 4.0f, grid.y + grid.h - ch), ui::Ink::fatal, status_.c_str());
        }
        mouse(grid, cw, ch);
        if (touch) {
            softKeys(Rect{r.x, r.y + grid.h, r.w, softH});
        }
    }

    bool hasSelection() const { return sel_ && !(selA_ == selB_); }

    std::string selection() const override { return hasSelection() ? selectedText() : ""; }

    void pasteText(const std::string& text) override {
        if (text.empty() || exited_) {
            return;
        }
        pending_ += screen_.encodePaste(text);
        jumpToLive();
        flush();
    }

    nlohmann::json describe() const override {
        return {{"kind", "terminal"},
                {"rows", screen_.rows()},
                {"cols", screen_.cols()},
                {"text", screen_.screenText()},
                {"alt", screen_.altScreen()},
                {"title", screen_.title()},
                {"cursor", {screen_.cursorRow(), screen_.cursorCol()}},
                {"mouse", screen_.mouseReporting()},
                {"bracketed-paste", screen_.bracketedPaste()},
                {"scrollback", screen_.scrollback().size()}};
    }

    std::string heldError() const override { return status_; }

  private:
    struct Cell {
        int row = 0, col = 0;
        bool operator==(const Cell&) const = default;
        bool operator<(const Cell& o) const { return row != o.row ? row < o.row : col < o.col; }
    };

    TermLine lineAt(int abs) const {
        const int back = static_cast<int>(screen_.scrollback().size());
        if (abs < back) {
            return screen_.scrollback()[static_cast<std::size_t>(abs)];
        }
        const int row = abs - back;
        return row < screen_.rows() ? screen_.line(row) : TermLine{};
    }

    bool selected(int row, int col) const {
        if (!hasSelection()) {
            return false;
        }
        const Cell a = std::min(selA_, selB_), b = std::max(selA_, selB_), c{row, col};
        return !(c < a) && c < b;
    }

    std::string selectedText() const {
        const Cell a = std::min(selA_, selB_), b = std::max(selA_, selB_);
        std::string out;
        for (int row = a.row; row <= b.row; ++row) {
            const TermLine cells = lineAt(row);
            std::string l;
            const int from = row == a.row ? a.col : 0;
            const int to = row == b.row ? b.col : static_cast<int>(cells.size());
            for (int c = from; c < to && c < static_cast<int>(cells.size()); ++c) {
                const TermCell& cell = cells[static_cast<std::size_t>(c)];
                if (cell.width != 0) {
                    l += cell.text.empty() ? " " : cell.text;
                }
            }
            while (!l.empty() && l.back() == ' ') {
                l.pop_back();
            }
            out += l;
            if (row != b.row) {
                out += '\n';
            }
        }
        return out;
    }

    void jumpToLive() {
        view_ = 0;
        sel_ = false;
    }

    void flush() {
        if (pending_.empty()) {
            return;
        }
        // Some mouse reports are bytes and not text: past column 94 the old encoding is not UTF-8.
        const bool text = validUtf8(pending_);
        nlohmann::json args{{"pane", id_}, {"text", text ? pending_ : bytesAsText(pending_)}};
        if (!text) {
            args["raw"] = true;
        }
        host_.act("pane.send", std::move(args), [this](const Response& r) {
            status_ = r.ok ? std::string() : errLine(r);
        });
        pending_.clear();
    }

    // Cells from pixels. The world hears of a new size once the size has rested, so a drag of a
    // divider does not resize a full-screen program on every frame.
    void fit(const Rect& r, float cw, float ch) {
        const int cols = std::max(10, static_cast<int>(r.w / cw));
        const int rows = std::max(3, static_cast<int>(r.h / ch));
        if (cols != screen_.cols() || rows != screen_.rows()) {
            screen_.resize(rows, cols);
            sizeChanged_ = Clock::now();
            sizeDirty_ = true;
        }
        if (sizeDirty_ && Clock::now() - sizeChanged_ > std::chrono::milliseconds(120)) {
            sizeDirty_ = false;
            if (!exited_) {
                host_.act("pane.resize", {{"pane", id_}, {"cols", cols}, {"rows", rows}}, [](const Response&) {});
            }
        }
    }

    void drawCursor(ImDrawList* dl, const Rect& r, float cw, float ch, bool focused) const {
        if (!screen_.cursorVisible() || view_ != 0 || exited_) {
            return;
        }
        const double t = ImGui::GetTime();
        const bool on = !focused || !screen_.cursorBlink() || std::fmod(t, ui::kBlinkPeriod * 2) < ui::kBlinkPeriod;
        const ImVec2 a(r.x + static_cast<float>(screen_.cursorCol()) * cw, r.y + static_cast<float>(screen_.cursorRow()) * ch);
        const ImVec2 b(a.x + cw, a.y + ch);
        if (!focused) {
            dl->AddRect(a, b, ui::Ink::secondary);
            return;
        }
        if (!on) {
            return;
        }
        switch (screen_.cursorShape()) {
        case TermCursorShape::Block: {
            dl->AddRectFilled(a, b, ui::Ink::inspected);
            const TermLine l = screen_.line(screen_.cursorRow());
            const std::size_t c = static_cast<std::size_t>(screen_.cursorCol());
            if (c < l.size() && !l[c].text.empty()) {
                dl->AddText(a, ui::Surface::background, l[c].text.c_str());
            }
            break;
        }
        case TermCursorShape::Underline:
            dl->AddRectFilled(ImVec2(a.x, b.y - 2.0f), b, ui::Ink::inspected);
            break;
        case TermCursorShape::Bar:
            dl->AddRectFilled(a, ImVec2(a.x + 2.0f, b.y), ui::Ink::inspected);
            break;
        }
    }

    void mouse(const Rect& r, float cw, float ch) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool over = hoveredRect(r);
        const int col = std::clamp(static_cast<int>((io.MousePos.x - r.x) / cw), 0, screen_.cols() - 1);
        const int row = std::clamp(static_cast<int>((io.MousePos.y - r.y) / ch), 0, screen_.rows() - 1);
        TermMods mods;
        mods.shift = io.KeyShift;
        mods.alt = io.KeyAlt;
        mods.ctrl = physicalCtrl();
        const bool report = screen_.mouseReporting() && !io.KeyShift && !exited_;
        if (report) {
            if (over) {
                for (int b = 0; b < 3; ++b) {
                    if (ImGui::IsMouseClicked(b)) {
                        pending_ += screen_.encodeMouseButton(row, col, b == 1 ? 3 : b == 2 ? 2 : 1, true, mods);
                    }
                }
                if (io.MouseWheel != 0.0f) {
                    pending_ += screen_.encodeMouseButton(row, col, io.MouseWheel > 0 ? 4 : 5, true, mods);
                }
            }
            for (int b = 0; b < 3; ++b) {
                if (ImGui::IsMouseReleased(b)) {
                    pending_ += screen_.encodeMouseButton(row, col, b == 1 ? 3 : b == 2 ? 2 : 1, false, mods);
                }
            }
            if (over && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
                pending_ += screen_.encodeMouseMove(row, col, mods);
            }
            flush();
            return;
        }
        if (over && io.MouseWheel != 0.0f) {
            const int lines = static_cast<int>(std::ceil(std::fabs(io.MouseWheel))) * 3;
            if (screen_.altScreen()) {
                for (int i = 0; i < lines && !exited_; ++i) {
                    pending_ += screen_.encodeKey(io.MouseWheel > 0 ? TermKey::Up : TermKey::Down);
                }
                flush();
            } else {
                view_ = std::clamp(view_ + (io.MouseWheel > 0 ? lines : -lines), 0, static_cast<int>(screen_.scrollback().size()));
            }
        }
        if (over && io.MouseSource == ImGuiMouseSource_TouchScreen && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8.0f)) {
            view_ = std::clamp(view_ + static_cast<int>(io.MouseDelta.y / ch), 0, static_cast<int>(screen_.scrollback().size()));
            return;
        }
        const int abs = static_cast<int>(screen_.scrollback().size()) - view_ + row;
        if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selA_ = selB_ = Cell{abs, col};
            sel_ = true;
            dragging_ = true;
            host_.wantKeyboard(true);
            if (ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) == 2) {
                selectWordAt(abs, col);
                dragging_ = false;
            } else if (ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) >= 3) {
                selA_ = Cell{abs, 0};
                selB_ = Cell{abs, screen_.cols()};
                dragging_ = false;
            }
        }
        if (dragging_) {
            selB_ = Cell{abs, col + (col == screen_.cols() - 1 ? 1 : 0)};
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                dragging_ = false;
            }
        }
    }

    void selectWordAt(int abs, int col) {
        const TermLine cells = lineAt(abs);
        const auto word = [&](int c) {
            if (c < 0 || c >= static_cast<int>(cells.size())) {
                return false;
            }
            const std::string& t = cells[static_cast<std::size_t>(c)].text;
            return !t.empty() && t != " ";
        };
        int a = col, b = col;
        while (word(a - 1)) {
            --a;
        }
        while (word(b)) {
            ++b;
        }
        selA_ = Cell{abs, a};
        selB_ = Cell{abs, b};
    }

    // Keys a phone keyboard does not have.
    void softKeys(Rect r) {
        struct Soft {
            const char* word;
            TermKey key;
        };
        static constexpr Soft kSoft[] = {{"ESC", TermKey::Escape}, {"TAB", TermKey::Tab}, {"UP", TermKey::Up}, {"DOWN", TermKey::Down},
                                         {"LEFT", TermKey::Left}, {"RIGHT", TermKey::Right}};
        ImGui::SetCursorScreenPos(ImVec2(r.x + 4.0f, r.y + 2.0f));
        for (const Soft& s : kSoft) {
            if (ui::bracketButton(s.word)) {
                pending_ += screen_.encodeKey(s.key);
            }
            ImGui::SameLine();
        }
        if (ui::bracketButton("CTRL", true, softCtrl_)) {
            softCtrl_ = !softCtrl_;
        }
        ImGui::SameLine();
        if (ui::bracketButton("PASTE")) {
            pasteText(host_.paste());
        }
        if (softCtrl_ && !ImGui::GetIO().InputQueueCharacters.empty()) {
            TermMods ctrl;
            ctrl.ctrl = true;
            char utf8[5] = {};
            const int n = ImTextCharToUtf8(utf8, ImGui::GetIO().InputQueueCharacters[0]);
            pending_ += screen_.encodeText(std::string_view(utf8, static_cast<std::size_t>(n)), ctrl);
            softCtrl_ = false;
        }
        flush();
    }

    Host& host_;
    std::string id_;
    TerminalScreen screen_;
    bool subscribed_ = false, readBack_ = false, exited_ = false, hasExit_ = false;
    Seq lastSeq_ = 0;
    int exitCode_ = 0;
    std::string pending_, status_;
    int view_ = 0; // lines scrolled back
    bool sel_ = false, dragging_ = false, softCtrl_ = false;
    Cell selA_, selB_;
    Clock::time_point sizeChanged_;
    bool sizeDirty_ = false;
};

} // namespace

std::unique_ptr<Module> makeTerminal(Host& host, const model::PaneInfo& pane) {
    return std::make_unique<TerminalModule>(host, pane);
}

} // namespace ke::modules
