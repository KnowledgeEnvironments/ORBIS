#include "platform/Platform.hpp"
#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

// ---- DEVICES ------------------------------------------------------------------------------------
//
// Tokens are Objects the world shows the fold (model/Tokens); the secret exists only in the answer
// to `token.issue` on this session and is shown once here, never logged, never a Field.

namespace {

std::string dayPart(const std::string& at) {
    // 2026-10-30T22:13:00.000Z -> 10-30 22:13
    if (at.size() < 16) {
        return at.empty() ? "-" : at;
    }
    return at.substr(5, 5) + " " + at.substr(11, 5);
}

bool writeTokenFile(const std::filesystem::path& file, const std::string& secret) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    FILE* f = std::fopen(file.c_str(), "w");
    if (!f) {
        return false;
    }
    std::fputs(secret.c_str(), f);
    std::fputc('\n', f);
    std::fclose(f);
    std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return !ec;
}

std::string safeFileName(std::string s) {
    for (char& c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) {
            c = '-';
        }
    }
    return s.empty() ? "device" : s;
}

} // namespace

void TerminalPane::drawDevices(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals,
                               chat::Chat& chat, const Session& session, ImFont* mono) {
    ImGui::BeginChild("devices", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    touchDragScroll();
    const std::string who = principals.nameOf(tab.principal);
    text(Ink::secondary, "// tokens that attach as " + who + "; each device its own, revocable, with an expiry");
    const double now = static_cast<double>(std::time(nullptr));
    const bool hasActions = actions_ && actions_->get("token.issue");
    if (!hasActions) {
        dimText("this world has no token actions yet (sprint 2 auth); the rows below are what the fold knows");
    }
    std::vector<const model::TokenInfo*> rows = tokens_ ? tokens_->forPrincipal(tab.principal) : std::vector<const model::TokenInfo*>{};
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_RowBg;
    if (rows.empty()) {
        dimText("no tokens on the log for %s%s", who.c_str(),
                info ? "" : " (principal not in the fold yet)");
    } else if (ImGui::BeginTable("tokens", 7, flags)) {
        ImGui::TableSetupColumn("label");
        ImGui::TableSetupColumn("issued");
        ImGui::TableSetupColumn("expires");
        ImGui::TableSetupColumn("last used");
        ImGui::TableSetupColumn("state");
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const model::TokenInfo* t : rows) {
            ImGui::PushID(t->id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(t->revoked() ? Ink::secondary : Ink::strong, t->label.empty() ? t->id.substr(0, 8) : t->label);
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("token %s\nissued by %s", t->id.c_str(), principals.nameOf(t->issuedBy).c_str());
            }
            ImGui::TableNextColumn();
            text(Ink::secondary, dayPart(t->createdAt));
            ImGui::TableNextColumn();
            text(Ink::secondary, t->expiresAt.empty() ? "never" : dayPart(t->expiresAt));
            ImGui::TableNextColumn();
            text(Ink::secondary, t->lastUsed.empty() ? "-" : dayPart(t->lastUsed));
            ImGui::TableNextColumn();
            const std::string st = t->state(now);
            chip(upper(st), st == "live" ? Ink::primary : (st == "revoked" ? Ink::inspected : Ink::inspected));
            if (!t->reason.empty() && ImGui::IsItemHovered()) {
                ke::ui::tip("%s", t->reason.c_str());
            }
            ImGui::TableNextColumn();
            if (!t->revoked()) {
                std::string& reason = tab.revokeReasons[t->id];
                ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 18);
                ImGui::InputTextWithHint("##reason", "reason", &reason);
                drive::hit("input:revoke-reason:" + t->id.substr(0, 8), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                ImGui::SameLine();
                if (bracketButtonNamed("revoke", "revoke:" + t->id.substr(0, 8), hasActions && !t->revoked())) {
                    const std::string id = t->id;
                    tab.tokenResults[id] = "revoking\xe2\x80\xa6";
                    tab.tokenResultErr[id] = false;
                    json args = {{"token", id}};
                    if (!reason.empty()) {
                        args["reason"] = reason;
                    }
                    chat.actNamed("token.revoke", args, [this, id](const Response& r) {
                        for (Tab& t2 : tabs_) {
                            if (t2.tokenResults.count(id)) {
                                t2.tokenResults[id] = r.ok ? "revoked" + std::string(r.seq ? " seq " + std::to_string(r.seq) : "")
                                                           : "err " + (r.err ? r.err->code : std::string("?")) +
                                                                 (r.err && !r.err->message.empty() ? " " + r.err->message : "");
                                t2.tokenResultErr[id] = !r.ok;
                            }
                        }
                    });
                }
                if (ImGui::IsItemHovered()) {
                    ke::ui::tip("act token.revoke {token, reason}: the device stops attaching at its next attach");
                }
            }
            ImGui::TableNextColumn();
            if (auto it = tab.tokenResults.find(t->id); it != tab.tokenResults.end()) {
                text(tab.tokenResultErr[t->id] ? Ink::inspected : Ink::connected, it->second);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    // NEW DEVICE: token.issue {principal label ttl-s}; the secret comes back once, to this session only.
    dimText("this agent's devices");
    ImGui::SameLine();
    if (bracketButtonNamed("pair phone", "pair-phone", actions_ && actions_->get("pair.begin") != nullptr, tab.pairOpen)) {
        tab.pairOpen = !tab.pairOpen;
        tab.pairStatus.clear();
    }
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("act pair.begin: a 6-digit code the phone claims for a token of its own (5 minutes)");
    }
    ImGui::SameLine(contentRight() - squareSide());
    if (squareButton(tab.newDevice ? "x" : "+", "new-device", hasActions, tab.newDevice)) {
        tab.newDevice = !tab.newDevice;
        tab.issueStatus.clear();
        if (tab.newDeviceLabel.empty()) {
            tab.newDeviceLabel = "device";
        }
    }
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("act token.issue {principal, label, ttl-s}: a new token for another device; the secret shows once");
    }
    if (tab.newDevice) {
        ImGui::Indent();
        dimText("LABEL");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 24);
        ImGui::InputText("##device-label", &tab.newDeviceLabel);
        drive::hit("input:device-label", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        dimText("EXPIRES IN");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 8);
        ImGui::InputDouble("##device-days", &tab.newDeviceDays, 0.0, 0.0, "%.0f");
        ImGui::SameLine();
        dimText("days (0 = never)");
        if (bracketButtonNamed("issue", "issue", hasActions && !tab.newDeviceLabel.empty())) {
            tab.issueStatus = "issuing\xe2\x80\xa6";
            tab.issueErr = false;
            json args = {{"principal", tab.principal}, {"label", tab.newDeviceLabel}};
            if (tab.newDeviceDays > 0) {
                args["ttl-s"] = static_cast<long long>(tab.newDeviceDays * 86400.0);
            }
            const std::string label = tab.newDeviceLabel;
            const std::string principal = tab.principal;
            chat.actNamed("token.issue", args, [this, label, principal](const Response& r) {
                for (Tab& t2 : tabs_) {
                    if (t2.principal != principal) {
                        continue;
                    }
                    if (!r.ok) {
                        t2.issueStatus = "err " + (r.err ? r.err->code : std::string("?")) +
                                         (r.err && !r.err->message.empty() ? " " + r.err->message : "");
                        t2.issueErr = true;
                        continue;
                    }
                    const json& res = r.result;
                    std::string secret;
                    for (const char* k : {"secret", "token-secret", "token_secret"}) {
                        if (res.is_object() && res.contains(k) && res[k].is_string()) {
                            secret = res[k].get<std::string>();
                            break;
                        }
                    }
                    t2.issuedObject = res.is_object() && res.contains("token") && res["token"].is_string() ? res["token"].get<std::string>() : "";
                    t2.issuedLabel = label;
                    t2.issuedSecret = secret;
                    t2.issuedSaved = false;
                    t2.issueErr = false;
                    if (secret.empty()) {
                        const std::string path = res.is_object() && res.contains("secret-written-to") && res["secret-written-to"].is_string()
                                                     ? res["secret-written-to"].get<std::string>()
                                                     : std::string();
                        t2.issueStatus = path.empty() ? "issued (no secret in the answer: the world keeps it)" : "issued; secret written to " + path;
                    } else {
                        t2.issueStatus = "issued" + std::string(r.seq ? " seq " + std::to_string(r.seq) : "");
                    }
                    t2.newDevice = false;
                }
            });
        }
        ImGui::Unindent();
    }
    if (!tab.issueStatus.empty()) {
        text(tab.issueErr ? Ink::inspected : Ink::connected, tab.issueStatus);
    }
    if (!tab.issuedSecret.empty()) {
        // The one-time secret: copy it or save it as <label>.token (0600) under the data dir; it is
        // never written anywhere else and never logged.
        ImGui::Spacing();
        outlineRect(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
                           ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeightWithSpacing() * 3.2f), Ink::inspected);
        ImGui::Indent();
        text(Ink::inspected, "TOKEN FOR " + upper(tab.issuedLabel) + " // shown once");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("[ COPY ] [ SAVE TO FILE ] [ DISMISS ]").x - 40.0f);
        ImGui::InputText("##secret", &tab.issuedSecret, ImGuiInputTextFlags_ReadOnly);
        drive::hit("input:secret", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        if (bracketButtonNamed("copy", "copy-secret")) {
            platform::setClipboard(tab.issuedSecret);
        }
        ImGui::SameLine();
        if (bracketButtonNamed("save to file", "save-secret", !tokenDir_.empty())) {
            const std::filesystem::path file = tokenDir_ / (safeFileName(tab.issuedLabel) + ".token");
            tab.issuedSaved = writeTokenFile(file, tab.issuedSecret);
            tab.issueStatus = tab.issuedSaved ? "saved to " + file.string() + " (0600)" : "could not write " + file.string();
            tab.issueErr = !tab.issuedSaved;
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("%s", (tokenDir_ / (safeFileName(tab.issuedLabel) + ".token")).string().c_str());
        }
        ImGui::SameLine();
        if (bracketButtonNamed("dismiss", "dismiss-secret")) {
            tab.issuedSecret.clear();
        }
        dimText("use it as --token-file on another machine, or ADD WORLD with it; the world stores only its hash");
        ImGui::Unindent();
    }
    drawPairing(tab, chat, session, mono);
    ImGui::EndChild();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// ---- pairing ------------------------------------------------------------------------------------

void TerminalPane::drawPairing(Tab& tab, chat::Chat& chat, const Session& session, ImFont* mono) {
    if (!tab.pairOpen) {
        return;
    }
    ImGui::Spacing();
    ImGui::Separator();
    text(Ink::strong, "PAIR A PHONE");
    ImGui::SameLine();
    text(Ink::secondary, "// the phone enters the code (or scans) and claims a token of its own, labelled, revocable here");
    const double now = static_cast<double>(std::time(nullptr));
    const double expires = tab.pairExpiresAt.empty() ? 0.0 : model::epochSeconds(tab.pairExpiresAt);
    const bool live = !tab.pairCode.empty() && (expires <= 0.0 || expires > now);
    if (!live) {
        if (!tab.pairCode.empty()) {
            text(Ink::inspected, "the code expired; begin again");
        }
        if (bracketButtonNamed("begin pairing", "pair-begin", session.attached())) {
            tab.pairStatus = "asking the world\xe2\x80\xa6";
            tab.pairErr = false;
            const std::string principal = tab.principal;
            // the url the phone should use: what the world says it is, else this socket's host
            std::string url = session.world().url;
            if (url.empty()) {
                url = socketUrl_;
            }
            chat.actNamed("pair.begin", json{{"principal", principal}, {"label", "phone"}, {"ttl-s", 300}},
                          [this, principal, url](const Response& r) {
                              for (Tab& t2 : tabs_) {
                                  if (t2.principal != principal) {
                                      continue;
                                  }
                                  if (!r.ok || !r.result.is_object()) {
                                      t2.pairStatus = "err " + (r.err ? r.err->code : std::string("?")) +
                                                      (r.err && !r.err->message.empty() ? " " + r.err->message : "");
                                      t2.pairErr = true;
                                      continue;
                                  }
                                  const json& res = r.result;
                                  // the world answers `pairing-code` (`code` is a JSON-form discriminator); older
                                  // drafts said `code`, kept as a fallback
                                  const char* codeKey = res.contains("pairing-code") ? "pairing-code" : "code";
                                  t2.pairCode = res.contains(codeKey) ? (res[codeKey].is_string() ? res[codeKey].get<std::string>()
                                                                                                   : res[codeKey].dump())
                                                                       : std::string();
                                  t2.pairExpiresAt = res.value("expires-at", "");
                                  t2.pairUrl = res.contains("url") && res["url"].is_string() ? res["url"].get<std::string>() : url;
                                  t2.pairLink = app::makePairLink(t2.pairUrl, t2.pairCode);
                                  const qr::Matrix m = qr::encode(t2.pairLink);
                                  t2.pairQr = m.dark;
                                  t2.pairQrSize = m.size;
                                  t2.pairStatus = m.ok() ? "" : "link too long for a QR; type the code";
                                  t2.pairErr = !m.ok();
                              }
                          });
        }
        drive::hit("button:pair-begin", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    } else {
        // the code, large
        ImGui::PushFont(mono, ImGui::GetFontSize() * 3.0f);
        std::string spacedCode;
        for (std::size_t i = 0; i < tab.pairCode.size(); ++i) {
            if (i == 3) {
                spacedCode += ' ';
            }
            spacedCode += tab.pairCode[i];
        }
        text(Ink::strong, spacedCode);
        ImGui::PopFont();
        drive::hit("text:pair-code", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        text(Ink::secondary, "WORLD  ");
        ImGui::SameLine();
        text(Ink::nav, tab.pairUrl);
        if (expires > 0.0) {
            const int left = static_cast<int>(expires - now);
            text(Ink::secondary, "expires in " + std::to_string(left / 60) + " m " + std::to_string(left % 60) + " s");
        }
        // the QR, as squares
        if (tab.pairQrSize > 0) {
            const float cell = std::max(3.0f, std::floor(ImGui::GetTextLineHeight() * 0.42f));
            const float quiet = cell * 4.0f;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float side = quiet * 2.0f + cell * static_cast<float>(tab.pairQrSize);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side), kQrLight);
            for (int r = 0; r < tab.pairQrSize; ++r) {
                for (int c = 0; c < tab.pairQrSize; ++c) {
                    if (tab.pairQr[static_cast<std::size_t>(r * tab.pairQrSize + c)]) {
                        const ImVec2 a(origin.x + quiet + cell * static_cast<float>(c), origin.y + quiet + cell * static_cast<float>(r));
                        dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), kQrDark);
                    }
                }
            }
            ImGui::Dummy(ImVec2(side, side));
            drive::hit("image:pair-qr", origin, ImVec2(origin.x + side, origin.y + side));
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("%s", tab.pairLink.c_str());
            }
        }
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x - 120.0f, ImGui::CalcTextSize("0").x * 80));
        ImGui::InputText("##pair-link", &tab.pairLink, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (bracketButtonNamed("copy link", "copy-pair-link")) {
            platform::setClipboard(tab.pairLink);
        }
        text(Ink::secondary, "on the phone: SETTINGS > PAIR WITH CODE, or open the link; the new device appears above once it claims");
        if (bracketButtonNamed("done", "pair-done")) {
            tab.pairOpen = false;
            tab.pairCode.clear();
        }
    }
    if (!tab.pairStatus.empty()) {
        text(tab.pairErr ? Ink::inspected : Ink::connected, tab.pairStatus);
    }
}

bool TerminalPane::countdown() const {
    const double now = static_cast<double>(std::time(nullptr));
    for (const Tab& t : tabs_) {
        if (t.pairOpen && !t.pairCode.empty()) {
            const double exp = t.pairExpiresAt.empty() ? 0.0 : model::epochSeconds(t.pairExpiresAt);
            if (exp > now) {
                return true;
            }
        }
    }
    return false;
}

} // namespace ke::ui
