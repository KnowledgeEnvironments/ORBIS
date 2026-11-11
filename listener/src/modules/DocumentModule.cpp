// The two modules that read a document and draw it: the Markdown viewer and the TeX view. One file
// because they differ only in how text becomes Markdown, and in that a .tex document can also be
// rendered to a page by the world. Both reload when the file changes and flip to source at a click.
#include "modules/Module.hpp"
#include "modules/ModuleUi.hpp"
#include "modules/RenderedDoc.hpp"
#include "modules/TexDocument.hpp"

#include "drive/Commands.hpp"
#include "ui/MathView.hpp"
#include "ui/Textures.hpp"
#include "ui/Theme.hpp"
#include "util/Base64.hpp"

#include <algorithm>
#include <cmath>

namespace ke::modules {

namespace {

constexpr double kPollSeconds = 2.0; // the world's push is the fast path; this catches an edit on disk

class DocumentModule final : public Module {
  public:
    DocumentModule(Host& host, const model::PaneInfo& p, bool tex) : host_(host), id_(p.id), tex_(tex) {}
    ~DocumentModule() override { ui::textures::release(pageImage_); }

    void goToLine(int line) override { wantLine_ = line; }

    void restore(const model::PaneInfo&) override {
        if (!loaded_) {
            loaded_ = true;
            reload();
            host_.act("pane.subscribe", {{"pane", id_}}, [](const Response&) {});
        }
    }

    void data(const PaneData& d) override {
        if (d.full && !d.data.empty() && d.data != text_) {
            text_ = d.data;
            doc_.invalidate();
        } else if (!d.full) {
            reload();
        }
    }
    void driven(const model::PaneInput&, const std::string&) override { reload(); }
    bool hasSource() const override { return true; }
    bool showingSource() const override { return source_; }
    void toggleSource() override {
        source_ = !source_;
        doc_.invalidate();
    }

    void keyboard() override {
        if (ImGui::GetIO().WantTextInput) {
            return;
        }
        const float lh = ImGui::GetTextLineHeight();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            nudge_ += lh * 2;
        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            nudge_ -= lh * 2;
        } else if (ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_Space)) {
            nudge_ += page_;
        } else if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
            nudge_ -= page_;
        }
    }

    void draw(Rect r, bool focused) override {
        (void)focused;
        if (ImGui::GetTime() - lastPoll_ > kPollSeconds) {
            reload();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(topLeft(r), bottomRight(r), ui::Surface::background);
        const float barH = ImGui::GetFrameHeightWithSpacing();
        ImGui::SetCursorScreenPos(ImVec2(r.x + 4.0f, r.y + 2.0f));
        ImGui::PushID(id_.c_str());
        if (ui::bracketButtonNamed(source_ ? "RENDERED" : "SOURCE", "doc-source", true, source_)) {
            toggleSource();
        }
        if (tex_) {
            ImGui::SameLine();
            if (ui::bracketButtonNamed("RENDER", "doc-render", !rendering_)) {
                render();
            }
            if (pageImage_.valid()) {
                ImGui::SameLine();
                if (ui::bracketButtonNamed("PAGE", "doc-page", true, showPage_)) {
                    showPage_ = !showPage_;
                }
            }
            ImGui::SameLine();
            if (ui::bracketButtonNamed("CONTRAST", "doc-contrast", true, contrast_)) {
                contrast_ = !contrast_;
            }
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("black on white, as a page would read; off shows it in the theme");
            }
        }
        if (!status_.empty()) {
            ImGui::SameLine();
            ui::text(statusBad_ ? ui::Ink::fatal : ui::Ink::secondary, status_);
        }
        Rect body{r.x, r.y + barH, r.w, r.h - barH};
        page_ = body.h * 0.9f;
        ImGui::SetCursorScreenPos(topLeft(body));
        if (ImGui::BeginChild("doc", ImVec2(body.w, body.h), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
            if (nudge_ != 0.0f) {
                ImGui::SetScrollY(ImGui::GetScrollY() + nudge_);
                nudge_ = 0.0f;
            }
            // Rendered text has no source lines, so a citation lands by how far down the file it is.
            if (wantLine_ > 0 && !text_.empty() && ImGui::GetScrollMaxY() > 0.0f) {
                const auto lines = std::count(text_.begin(), text_.end(), '\n') + 1;
                ImGui::SetScrollY(ImGui::GetScrollMaxY() * static_cast<float>(wantLine_ - 1) / static_cast<float>(lines));
                wantLine_ = 0;
            }
            ui::touchDragScroll();
            if (showPage_ && pageImage_.valid()) {
                const float w = ImGui::GetContentRegionAvail().x;
                const float h = w * static_cast<float>(pageImage_.h) / static_cast<float>(std::max(1, pageImage_.w));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + w, at.y + h), ui::Surface::paper);
                ImGui::Image(pageImage_.id, ImVec2(w, h));
            } else if (tex_ && contrast_ && !source_) {
                // Typesetting is read as a page: black ink on white, whatever the theme.
                const ImVec2 at = ImGui::GetWindowPos();
                ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + ImGui::GetWindowWidth(), at.y + ImGui::GetWindowHeight()),
                                                          ui::Surface::paper);
                const float inset = ImGui::CalcTextSize("0").x * 2.0f;
                ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + inset, ImGui::GetCursorPosY() + inset / 2.0f));
                ui::setMathInk(ui::Ink::onPaper);
                document(ImGui::GetContentRegionAvail().x - inset, ui::Ink::onPaper);
                ui::setMathInk(0);
            } else {
                document(ImGui::GetContentRegionAvail().x);
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }

    nlohmann::json describe() const override {
        return {{"kind", tex_ ? "latex" : "markdown"}, {"source", source_}, {"text", text_}, {"status", status_},
                {"math", doc_.mathCount()}, {"page", pageImage_.valid()}, {"contrast", tex_ && contrast_}};
    }

    std::string selection() const override { return doc_.selection(); }

    std::string heldError() const override { return statusBad_ ? status_ : std::string(); }

  private:
    void reload() {
        lastPoll_ = ImGui::GetTime();
        host_.act("pane.read", {{"pane", id_}, {"what", "buffer"}}, [this](const Response& r) {
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
            if (statusBad_) {
                status_.clear();
                statusBad_ = false;
            }
            if (text != text_) {
                text_ = std::move(text);
                doc_.invalidate();
            }
        });
    }

    void render() {
        rendering_ = true;
        status_ = "rendering in the world";
        statusBad_ = false;
        host_.act("pane.frob", {{"pane", id_}, {"verb", "render"}, {"args", nlohmann::json::object()}}, [this](const Response& r) {
            rendering_ = false;
            if (!r.ok) {
                status_ = errLine(r) + " (the view below needs no model)";
                statusBad_ = true;
                return;
            }
            // {view {kind image, data {bytes receipt, mime}}, log}: the first page, or a PDF when
            // this host has no rasterizer. The client draws images.
            const nlohmann::json view = r.result.is_object() ? r.result.value("view", nlohmann::json::object()) : nlohmann::json::object();
            const nlohmann::json viewData = view.is_object() ? view.value("data", nlohmann::json::object()) : nlohmann::json::object();
            if (!viewData.is_object() || viewData.value("mime", std::string()) != "image/png") {
                status_ = "the world rendered no image";
                statusBad_ = true;
                return;
            }
            host_.readBytes(id_, "pane/image", [this](std::vector<unsigned char> bytes, std::string err) {
                if (!err.empty() || bytes.empty()) {
                    status_ = err.empty() ? "the rendered page did not arrive" : err;
                    statusBad_ = true;
                    return;
                }
                ui::textures::release(pageImage_);
                std::string why;
                pageImage_ = ui::textures::upload(bytes.data(), bytes.size(), why);
                status_ = pageImage_.valid() ? std::string() : why;
                statusBad_ = !pageImage_.valid();
                showPage_ = pageImage_.valid();
            });
        });
    }

    void document(float wrap, ImU32 ink = 0) {
        if (doc_.stale()) {
            shown_ = source_ || !tex_ ? text_ : texToMarkdown(text_);
        }
        doc_.draw(shown_, !source_, wrap, id_.c_str(), ink);
    }

    Host& host_;
    std::string id_;
    bool tex_;
    std::string text_, shown_, status_;
    bool loaded_ = false, source_ = false, statusBad_ = false, rendering_ = false, showPage_ = false, contrast_ = true;
    double lastPoll_ = -100.0;
    float nudge_ = 0.0f, page_ = 300.0f;
    int wantLine_ = 0;
    RenderedDoc doc_;
    ui::textures::Texture pageImage_;
};

} // namespace

std::unique_ptr<Module> makeMarkdown(Host& host, const model::PaneInfo& pane) {
    return std::make_unique<DocumentModule>(host, pane, false);
}

std::unique_ptr<Module> makeLatex(Host& host, const model::PaneInfo& pane) {
    return std::make_unique<DocumentModule>(host, pane, true);
}

} // namespace ke::modules
