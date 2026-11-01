// Images a Message carries, in the transcript: fetched once from the world, decoded, kept by Message
// and index, and drawn fitted into a slot of fixed height, so a picture arriving never moves the text.
// A click opens it in the image module.
#pragma once

#include "chat/Chat.hpp"
#include "ui/Textures.hpp"

#include <imgui.h>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace ke::ui {

class MessageImages {
  public:
    ~MessageImages();
    // The height of the row one Message's images take: the largest slot until they load, then what they need.
    float rowHeight(const std::string& message, const nlohmann::json& images, float lineHeight) const;
    // Bumped when an image lands, so the transcript can fit its row.
    unsigned long long version() const { return *version_; }
    // Draws the row. Returns the index clicked, or -1.
    int draw(chat::Chat& chat, const std::string& message, const nlohmann::json& images, ImVec2 a, ImVec2 z);
    void clear();

  private:
    struct Item {
        textures::Texture tex;
        std::string error;
        bool asked = false;
    };
    std::shared_ptr<Item> item(chat::Chat& chat, const std::string& message, int index);
    std::map<std::string, std::shared_ptr<Item>> items_;
    std::shared_ptr<unsigned long long> version_ = std::make_shared<unsigned long long>(0);
};

MessageImages& messageImages();

} // namespace ke::ui
