#include "ui/MessageImages.hpp"

#include "drive/Commands.hpp"
#include "ui/Theme.hpp"
#include "util/Base64.hpp"

#include <algorithm>
#include <vector>

namespace ke::ui {

namespace {

using json = nlohmann::json;

constexpr float kSlotLines = 13.0f;

// One chunk, then the next from where it ended, until the world says there is no more.
void readChunk(chat::Chat& chat, const std::string& message, int index, std::vector<unsigned char> have,
               std::function<void(std::vector<unsigned char>, std::string)> done) {
    json args = {{"message", message}, {"index", index}, {"offset", have.size()}};
    chat.actNamed("image.read", args, [&chat, message, index, have = std::move(have), done = std::move(done)](const Response& r) mutable {
        if (!r.ok) {
            done({}, r.err ? r.err->code : std::string("?"));
            return;
        }
        const json& result = r.result.is_object() && r.result.contains("result") ? r.result["result"] : r.result;
        std::vector<unsigned char> chunk;
        if (!result.is_object() || !result.contains("base64") || !util::base64Decode(result["base64"].get<std::string>(), chunk)) {
            done({}, "the world sent no bytes");
            return;
        }
        have.insert(have.end(), chunk.begin(), chunk.end());
        if (result.value("more", false) && !chunk.empty()) {
            readChunk(chat, message, index, std::move(have), std::move(done));
        } else {
            done(std::move(have), "");
        }
    });
}

} // namespace

MessageImages::~MessageImages() { clear(); }

void MessageImages::clear() {
    for (auto& [key, it] : items_) {
        textures::release(it->tex);
    }
    items_.clear();
}

float MessageImages::rowHeight(const std::string& message, const json& images, float lineHeight) const {
    const float most = std::floor(lineHeight * kSlotLines);
    float tallest = 0.0f;
    const std::size_t n = images.is_array() ? images.size() : 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto it = items_.find(message + "#" + std::to_string(i));
        if (it == items_.end() || !it->second->tex.valid()) {
            return most;
        }
        tallest = std::max(tallest, static_cast<float>(it->second->tex.h));
    }
    return std::min(most, tallest + lineHeight + 4.0f);
}

std::shared_ptr<MessageImages::Item> MessageImages::item(chat::Chat& chat, const std::string& message, int index) {
    const std::string key = message + "#" + std::to_string(index);
    auto it = items_.find(key);
    if (it != items_.end()) {
        return it->second;
    }
    auto made = std::make_shared<Item>();
    items_[key] = made;
    made->asked = true;
    readChunk(chat, message, index, {}, [made, version = version_](std::vector<unsigned char> bytes, std::string err) {
        ++*version;
        if (!err.empty()) {
            made->error = err;
            return;
        }
        if (bytes.empty()) {
            made->error = "empty";
            return;
        }
        made->tex = textures::upload(bytes.data(), bytes.size(), made->error);
    });
    return made;
}

int MessageImages::draw(chat::Chat& chat, const std::string& message, const json& images, ImVec2 a, ImVec2 z) {
    int clicked = -1;
    if (!images.is_array()) {
        return clicked;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    const float gap = std::floor(lineH * 0.5f);
    const float maxH = std::max(lineH, z.y - a.y - lineH - 2.0f);
    const float maxW = std::min((z.x - a.x), maxH * 2.0f);
    float x = a.x;
    int index = 0;
    for (const json& img : images) {
        if (!img.is_object()) {
            ++index;
            continue;
        }
        const std::string name = img.value("name", std::string());
        const std::string label = name.empty() ? img.value("mime", std::string("image")) : name;
        std::shared_ptr<Item> it = item(chat, message, index);
        ImVec2 size(maxW, maxH);
        if (it->tex.valid()) {
            const float scale = std::min({maxW / static_cast<float>(it->tex.w), maxH / static_cast<float>(it->tex.h), 1.0f});
            size = ImVec2(std::floor(static_cast<float>(it->tex.w) * scale), std::floor(static_cast<float>(it->tex.h) * scale));
        }
        if (x + size.x > z.x) {
            break; // the rest are one click away from the Message's own row
        }
        const ImVec2 p0(x, a.y);
        const ImVec2 p1(x + size.x, a.y + size.y);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(message.c_str());
        ImGui::PushID(index);
        // the document under it holds the press, so this reads the mouse rather than being a button
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("image", size);
        const bool hovered = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            clicked = index;
        }
        drive::hit("image:" + label, p0, p1);
        ImGui::PopID();
        ImGui::PopID();
        if (it->tex.valid()) {
            dl->AddImage(it->tex.id, p0, p1);
        } else {
            dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 4.0f), it->error.empty() ? Ink::secondary : Ink::fatal,
                        it->error.empty() ? "loading\xe2\x80\xa6" : ("no image: " + it->error).c_str());
        }
        dl->AddRect(p0, p1, hovered ? Ink::strong : Ink::secondary);
        dl->PushClipRect(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y + lineH + 2.0f), true);
        dl->AddText(ImVec2(p0.x, p1.y + 1.0f), Ink::secondary, label.c_str());
        dl->PopClipRect();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s\nclick: open it", label.c_str());
        }
        x += size.x + gap;
        ++index;
    }
    ui::restoreCursor(keep);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    return clicked;
}

MessageImages& messageImages() {
    static MessageImages one;
    return one;
}

} // namespace ke::ui
