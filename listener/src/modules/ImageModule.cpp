// An image: bytes from the world, decoded and fitted to the pane; the wheel or a pinch zooms.
#include "modules/Module.hpp"
#include "modules/ModuleUi.hpp"

#include "ui/Textures.hpp"
#include "ui/Theme.hpp"
#include "util/Base64.hpp"

#include <algorithm>

namespace ke::modules {

namespace {

class ImageModule final : public Module {
  public:
    ImageModule(Host& host, const model::PaneInfo& p) : host_(host), id_(p.id) {}
    ~ImageModule() override { ui::textures::release(tex_); }

    void restore(const model::PaneInfo&) override {
        if (loaded_) {
            return;
        }
        loaded_ = true;
        host_.readBytes(id_, "pane/image", [this](std::vector<unsigned char> bytes, std::string err) {
            if (!err.empty()) {
                status_ = err;
                return;
            }
            tex_ = ui::textures::upload(bytes.data(), bytes.size(), status_);
        });
    }

    void draw(Rect r, bool) override {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(topLeft(r), bottomRight(r), ui::Surface::background);
        if (!tex_.valid()) {
            dl->AddText(ImVec2(r.x + 6.0f, r.y + 6.0f), status_.empty() ? ui::Ink::secondary : ui::Ink::fatal,
                        status_.empty() ? "loading" : status_.c_str());
            return;
        }
        if (hoveredRect(r)) {
            zoom_ = std::clamp(zoom_ * (1.0f + ImGui::GetIO().MouseWheel * 0.1f), 0.1f, 16.0f);
        }
        const float fit = std::min(r.w / static_cast<float>(tex_.w), r.h / static_cast<float>(tex_.h));
        const float w = static_cast<float>(tex_.w) * fit * zoom_, h = static_cast<float>(tex_.h) * fit * zoom_;
        const ImVec2 a(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2);
        dl->PushClipRect(topLeft(r), bottomRight(r), true);
        // A transparent image has no ground of its own, and black ink needs one: a typeset page has only ink.
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), ui::Surface::paper);
        dl->AddImage(tex_.id, a, ImVec2(a.x + w, a.y + h));
        dl->PopClipRect();
    }

    nlohmann::json describe() const override {
        return {{"kind", "image"}, {"w", tex_.w}, {"h", tex_.h}, {"status", status_}};
    }

    std::string heldError() const override { return status_; }

  private:
    Host& host_;
    std::string id_, status_;
    bool loaded_ = false;
    float zoom_ = 1.0f;
    ui::textures::Texture tex_;
};

} // namespace

std::unique_ptr<Module> makeImage(Host& host, const model::PaneInfo& pane) {
    return std::make_unique<ImageModule>(host, pane);
}

} // namespace ke::modules
