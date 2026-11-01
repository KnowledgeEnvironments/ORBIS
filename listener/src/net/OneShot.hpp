// One request on a fresh socket, before any attach: `hello` (who is this world) and `pair.claim`
// (a code for a token) are the two ops the protocol accepts unauthenticated. The socket lives on
// its own thread for the few seconds it takes; the UI polls done() and reads the answer.
#pragma once

#include "net/Protocol.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ke {

class OneShot {
  public:
    OneShot() = default;
    ~OneShot();
    OneShot(const OneShot&) = delete;
    OneShot& operator=(const OneShot&) = delete;

    // Connect to `url` (ws://host:port/ws, ?format=json added when missing), send {op args}, wait
    // for the response up to timeoutMs, close. A second start() while running is ignored.
    void start(const std::string& url, const std::string& op, json args, int timeoutMs = 8000);
    bool running() const { return running_.load(); }
    bool done() const { return done_.load(); }
    // Valid once done(): ok and result / err, or `error` when the socket never answered.
    bool ok() const;
    json result() const;
    std::string error() const; // "" when ok
    void reset();

  private:
    void run(std::string url, std::string op, json args, int timeoutMs);
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> done_{false};
    mutable std::mutex mutex_;
    bool ok_ = false;
    json result_;
    std::string error_;
};

} // namespace ke
