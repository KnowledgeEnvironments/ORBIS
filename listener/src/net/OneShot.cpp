#include "net/OneShot.hpp"

#include "net/WsClient.hpp"
#include "util/Log.hpp"

#include <chrono>

namespace ke {

OneShot::~OneShot() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void OneShot::reset() {
    if (running_.load()) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    done_.store(false);
    ok_ = false;
    result_ = json();
    error_.clear();
}

void OneShot::start(const std::string& url, const std::string& op, json args, int timeoutMs) {
    if (running_.load()) {
        return;
    }
    reset();
    running_.store(true);
    std::string u = url;
    if (u.find('?') == std::string::npos) {
        u += "?format=json";
    }
    thread_ = std::thread([this, u, op, a = std::move(args), timeoutMs] { run(u, op, a, timeoutMs); });
}

bool OneShot::ok() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ok_;
}

json OneShot::result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

std::string OneShot::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void OneShot::run(std::string url, std::string op, json args, int timeoutMs) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string error;
    bool ok = false;
    json result;
    {
        WsClient client(url);
        client.start();
        bool opened = false, sent = false, answered = false;
        const std::string id = "o1";
        while (Clock::now() < deadline && !answered) {
            Inbound in;
            bool any = false;
            while (client.poll(in)) {
                any = true;
                switch (in.type) {
                case Inbound::Type::Opened:
                    opened = true;
                    break;
                case Inbound::Type::Closed:
                    if (sent) {
                        error = "closed before answering (" + in.text + ")";
                        answered = true;
                    }
                    break;
                case Inbound::Type::Error:
                    error = in.text;
                    break;
                case Inbound::Type::Frame:
                    if (in.frame && kindOf(*in.frame) == FrameKind::Response) {
                        const Response& r = std::get<Response>(*in.frame);
                        if (r.id == id) {
                            ok = r.ok;
                            result = r.ok ? r.result : json{{"code", r.err ? r.err->code : "invalid"},
                                                            {"message", r.err ? r.err->message : ""}};
                            if (!r.ok) {
                                error = (r.err ? r.err->code : std::string("invalid")) +
                                        (r.err && !r.err->message.empty() ? " " + r.err->message : "");
                            } else {
                                error.clear();
                            }
                            answered = true;
                        }
                    }
                    break;
                }
            }
            if (opened && !sent) {
                client.send(Request{id, op, args});
                sent = true;
            }
            if (!any) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (!answered && error.empty()) {
            error = opened ? "no answer within " + std::to_string(timeoutMs) + " ms" : "could not connect";
        }
        client.stop();
    }
    log::info("oneshot: {} on {} -> {}", op, url, ok ? "ok" : error);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ok_ = ok;
        result_ = std::move(result);
        error_ = std::move(error);
    }
    done_.store(true);
    running_.store(false);
}

} // namespace ke
