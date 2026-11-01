// Who we are on the wire: principal, session id, CARRIER state, head seq in white, per-sub
// cursor/acked/lag as slanted bars, the last error in red; and the BIND pane for the sprint-0
// workspace stubs.
#pragma once

#include "chat/Chat.hpp"
#include "model/Principals.hpp"
#include "model/EventStore.hpp"
#include "net/Protocol.hpp"
#include "net/Session.hpp"
#include "net/WsClient.hpp"
#include "ui/Theme.hpp"
#include "workspace/LocalFs.hpp"

#include <imgui.h>
#include <optional>
#include <vector>

namespace ke::ui {

struct BindRow {
    workspace::BindPlan plan;
    std::optional<Response> response;
    bool sent = false;
};

class SessionPane {
  public:
    void draw(const Session& session, const WsClient& client, const EventStore& store, const chat::Chat& chat, ImFont* mono);
    void drawBody(const Session& session, const WsClient& client, const EventStore& store, const chat::Chat& chat, ImFont* mono);
    void drawBinds(const std::vector<BindRow>& rows, ImFont* mono, bool canSend, bool& sendRequested);
    void setNames(const model::Principals* names) { names_ = names; }
    // The BOOT row: how the folds came up on this attach (snapshot or replay), from the Runtime.
    void setBoot(const std::string* bootLine) { bootLine_ = bootLine; }
    // The LINK row: reconnects so far, a backoff in progress, or why the client stopped.
    void setLink(const std::string* linkLine, bool stopped) {
        linkLine_ = linkLine;
        linkStopped_ = stopped;
    }
    void drawBindsBody(const std::vector<BindRow>& rows, ImFont* mono, bool canSend, bool& sendRequested);

  private:
    const model::Principals* names_ = nullptr;
    const std::string* bootLine_ = nullptr;
    const std::string* linkLine_ = nullptr;
    bool linkStopped_ = false;
};

} // namespace ke::ui
