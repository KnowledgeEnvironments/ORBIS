// What the TerminalPane's translation units share: small helpers over Fields and text. Private to
// src/ui/TerminalPane.cpp and src/ui/terminal/.
#pragma once

#include "ui/TerminalPane.hpp"

#include "model/Snapshot.hpp"
#include "chat/Attach.hpp"
#include "util/Sha256.hpp"
#include "drive/Commands.hpp"
#include "ui/ReasoningRow.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/MessageImages.hpp"
#include "platform/Platform.hpp"
#include "chat/FieldValue.hpp"
#include "app/Worlds.hpp"
#include "util/Qr.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <unordered_map>
#include <SDL3/SDL.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace ke::ui::term {

using chat::editText;
using chat::facetOf;
using chat::scalarName;
using chat::typeName;
using chat::valueFor;


inline const std::string kNone;
void drawMarkdownExtras(const MdExtras& md, const SelDoc& doc, const SelLayout& lay, ImVec2 origin, float wrap,
                        int hoveredPara, const char* key, ImDrawListSplitter* split);
void registerSymbolHits(const MdExtras& md, const SelDoc& doc, const SelLayout& lay, ImFont* font, ImVec2 origin);
constexpr double kEscEscSeconds = 0.4; // two Escapes this close stop the running Evaluation

inline double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::string clockPart(const std::string& at) {
    return at.size() >= 19 ? at.substr(11, 8) : at;
}






inline std::string nowIso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
    return buf;
}


inline std::string hintOf(const model::PrincipalInfo* info, const std::string& base) {
    if (!info) {
        return "";
    }
    const model::FieldState* f = info->field(base + "/hint");
    return f && f->value.is_string() ? f->value.get<std::string>() : std::string();
}

inline json commentsOf(const model::PrincipalInfo* info, const std::string& base) {
    if (!info) {
        return json::array();
    }
    const model::FieldState* f = info->field(base + "/comment");
    return f && f->value.is_array() ? f->value : json::array();
}

} // namespace ke::ui::term
