// IXWebSocket wrapper. The socket runs on its own thread (inside ix::WebSocket); every message is
// parsed there into a Frame, queued on a single-producer single-consumer queue, and a wake callback
// is fired so the UI thread can drain with poll() (the App posts an SDL user event from it).
// Transport is docs/protocol.md "Transport": ws://host:port/ws?format=json, text frames.
#pragma once

#include "net/Protocol.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <readerwriterqueue.h>
#include <string>

namespace ix {
class WebSocket;
}

namespace ke {

// Disconnected/Connecting are owned by the transport; Attaching/Attached are promoted by the
// Session once the socket is open and once attach has been answered.
enum class ConnState { Disconnected, Connecting, Attaching, Attached };
const char* name(ConnState s);

struct Inbound {
    enum class Type { Opened, Closed, Frame, Error };
    Type type = Type::Error;
    std::optional<Frame> frame; // Type::Frame
    std::string text;           // close reason, error text, or the raw text of an unparsable frame
};

class WsClient {
  public:
    explicit WsClient(std::string url);
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // Called on the socket thread after an Inbound was queued. Must be cheap and thread-safe.
    void setWake(std::function<void()> wake);

    void start();     // connect and keep reconnecting with backoff
    void stop();      // close and stop reconnecting; joins the socket thread
    void drop();      // Debug: close the socket without detach; auto-reconnect takes over
    void reconnect(); // stop + start, resets the backoff
    // Close the socket (no detach) and let the socket thread wind down without waiting for it.
    // For callers that must not block, such as SDL's event watch while iOS suspends the app;
    // stop() or reconnect() from the main loop finishes the job.
    void suspend();

    void send(const Frame& frame);
    bool poll(Inbound& out); // consumer side; returns false when the queue is empty

    ConnState state() const { return state_.load(); }
    void setState(ConnState s) { state_.store(s); }
    const std::string& url() const { return url_; }

    std::uint64_t framesIn() const { return framesIn_.load(); }
    std::uint64_t framesOut() const { return framesOut_.load(); }
    std::uint64_t opens() const { return opens_.load(); }

  private:
    void enqueue(Inbound in);

    std::string url_;
    std::unique_ptr<ix::WebSocket> ws_;
    moodycamel::ReaderWriterQueue<Inbound> queue_;
    std::mutex wakeMutex_;
    std::function<void()> wake_;
    std::atomic<ConnState> state_{ConnState::Disconnected};
    std::atomic<std::uint64_t> framesIn_{0}, framesOut_{0}, opens_{0};
};

} // namespace ke
