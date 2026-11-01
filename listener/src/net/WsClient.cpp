#include "net/WsClient.hpp"

#include "util/Log.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace ke {

const char* name(ConnState s) {
    switch (s) {
    case ConnState::Disconnected:
        return "Disconnected";
    case ConnState::Connecting:
        return "Connecting";
    case ConnState::Attaching:
        return "Attaching";
    case ConnState::Attached:
        return "Attached";
    }
    return "?";
}

WsClient::WsClient(std::string url)
    : url_(std::move(url)), ws_(std::make_unique<ix::WebSocket>()), queue_(256) {
    ix::initNetSystem();
    ws_->setUrl(url_);
    ws_->enableAutomaticReconnection();
    ws_->setMinWaitBetweenReconnectionRetries(250);  // ms
    // Reconnects back off exponentially (IXWebSocket doubles from the minimum): 1 s, 2 s, 4 s …
    // capped at 30 s, so a world that keeps closing us never sees a storm of attaches.
    ws_->setMinWaitBetweenReconnectionRetries(1000); // ms
    ws_->setMaxWaitBetweenReconnectionRetries(30000); // ms
    ws_->setPingInterval(30);
    ws_->disablePerMessageDeflate();

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            opens_.fetch_add(1);
            state_.store(ConnState::Attaching);
            enqueue(Inbound{Inbound::Type::Opened, std::nullopt, msg->openInfo.uri});
            break;
        case ix::WebSocketMessageType::Close:
            // IXWebSocket reconnects by itself unless stop() was called, so the next state is
            // Connecting; stop() overrides it with Disconnected.
            state_.store(ConnState::Connecting);
            enqueue(Inbound{Inbound::Type::Closed, std::nullopt,
                            std::format("code {} {}", msg->closeInfo.code, msg->closeInfo.reason)});
            break;
        case ix::WebSocketMessageType::Error:
            state_.store(ConnState::Connecting);
            enqueue(Inbound{Inbound::Type::Error, std::nullopt,
                            std::format("{} (retry {}, next in {} ms)", msg->errorInfo.reason,
                                        msg->errorInfo.retries, msg->errorInfo.wait_time)});
            break;
        case ix::WebSocketMessageType::Message: {
            framesIn_.fetch_add(1);
            try {
                enqueue(Inbound{Inbound::Type::Frame, decode(msg->str), {}});
            } catch (const std::exception& e) {
                enqueue(Inbound{Inbound::Type::Error, std::nullopt,
                                std::format("bad frame: {}: {}", e.what(), msg->str.substr(0, 200))});
            }
            break;
        }
        case ix::WebSocketMessageType::Ping:
        case ix::WebSocketMessageType::Pong:
        case ix::WebSocketMessageType::Fragment:
            break;
        }
    });
}

WsClient::~WsClient() {
    stop();
}

void WsClient::setWake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(wakeMutex_);
    wake_ = std::move(wake);
}

void WsClient::start() {
    state_.store(ConnState::Connecting);
    ws_->enableAutomaticReconnection(); // suspend() turns it off
    ws_->start();
}

void WsClient::stop() {
    ws_->stop();
    state_.store(ConnState::Disconnected);
}

void WsClient::drop() {
    log::warn("ws: dropping connection on request (no detach)");
    ws_->close(1000, "debug drop");
}

void WsClient::suspend() {
    log::info("ws: suspending (closing without detach)");
    ws_->disableAutomaticReconnection(); // the thread exits once the close completes
    ws_->close(1000, "suspend");
    state_.store(ConnState::Disconnected);
}

void WsClient::reconnect() {
    log::info("ws: reconnect requested");
    stop();
    start();
}

void WsClient::send(const Frame& frame) {
    const std::string text = encode(frame);
    auto info = ws_->sendText(text);
    if (!info.success) {
        log::warn("ws: send failed (state {}): {} bytes", name(state()), text.size()); // never the text: it may be off the record
        return;
    }
    framesOut_.fetch_add(1);
}

bool WsClient::poll(Inbound& out) {
    return queue_.try_dequeue(out);
}

void WsClient::enqueue(Inbound in) {
    queue_.enqueue(std::move(in));
    std::lock_guard<std::mutex> lock(wakeMutex_);
    if (wake_) {
        wake_();
    }
}

} // namespace ke
