#include "app/Smoke.hpp"

#include "app/Runtime.hpp"
#include "util/Log.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

namespace ke {

int runSmoke(const Options& opts) {
    using clock = std::chrono::steady_clock;
    Runtime rt(opts, std::make_unique<MemorySessionStore>());

    std::mutex m;
    std::condition_variable cv;
    bool woken = false;
    rt.client.setWake([&] {
        std::lock_guard<std::mutex> lock(m);
        woken = true;
        cv.notify_one();
    });

    const auto start = clock::now();
    const auto end = start + std::chrono::seconds(opts.smokeSeconds);
    const auto dropAt = start + std::chrono::seconds(opts.smokeSeconds) / 2;
    const auto sendAt = start + std::chrono::seconds(opts.smokeSeconds) / 4;
    const char* kLine = "smoke: hello from ORBIS";
    bool dropped = false;
    bool sent = false;
    bool asked = false;
    std::size_t rowsBeforeDrop = 0;
    int opensBeforeDrop = 0;

    log::info("smoke: {} s against {} (drop at {} s)", opts.smokeSeconds, rt.client.url(),
              opts.smokeSeconds / 2.0);
    rt.start();
    while (clock::now() < end) {
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return woken; });
            woken = false;
        }
        rt.drain();
        if (rt.session.attached() && !rt.bindsSentThisAttach) {
            rt.sendBinds();
        }
        // The chat path: one line down the inbox route once it is known (or a probe send with
        // the principal standing in when the world's root has no route yet), then `who am i`.
        if (!sent && rt.session.attached() && clock::now() >= sendAt &&
            rt.chat.routeState() != chat::RouteState::Searching &&
            rt.chat.routeState() != chat::RouteState::Unknown) {
            sent = true;
            if (rt.chat.routeState() == chat::RouteState::Found) {
                rt.chat.submit(kLine);
            } else {
                log::info("smoke: no route on the principal; probing send with the principal as route");
                rt.chat.probeSend(kLine);
            }
        }
        if (sent && !asked && rt.chat.stats().sendsOk + rt.chat.stats().sendsErr > 0) {
            asked = true;
            rt.chat.submit("who am i");
        }
        if (!dropped && clock::now() >= dropAt) {
            dropped = true;
            rowsBeforeDrop = rt.events.rows().size();
            opensBeforeDrop = static_cast<int>(rt.client.opens());
            rt.client.drop();
        }
    }
    rt.client.stop();
    rt.drain();

    const auto& s = rt.session;
    const std::size_t rows = rt.events.rows().size();
    std::printf("smoke summary\n");
    std::printf("  state        %s\n", name(rt.client.state()));
    std::printf("  session      %s  principal %s\n", s.sessionId().c_str(),
                s.principal().value_or("-").c_str());
    std::printf("  attaches     %d  resumed %d  socket opens %llu\n", s.attaches(), s.resumes(),
                static_cast<unsigned long long>(rt.client.opens()));
    std::printf("  rows         %zu  (before drop %zu, after %zu)  head %lld\n", rows, rowsBeforeDrop,
                rows - rowsBeforeDrop, static_cast<long long>(s.head()));
    std::printf("  gaps         %zu  duplicates(store) %zu\n", rt.events.gaps().size(),
                rt.events.duplicates());
    for (const auto& [sub, st] : s.subs()) {
        std::printf(
            "  sub %-8s cursor %lld acked %lld unacked %lld lag %lld received %llu dupes %llu acks %llu%s\n",
            sub.c_str(), static_cast<long long>(st.cursor.value_or(-1)),
            static_cast<long long>(st.acked.value_or(-1)), static_cast<long long>(st.unacked()),
            static_cast<long long>(st.lag()), static_cast<unsigned long long>(st.received),
            static_cast<unsigned long long>(st.duplicates), static_cast<unsigned long long>(st.acksSent),
            st.lagging ? " LAGGING" : "");
    }
    for (const auto& b : rt.binds) {
        std::printf("  bind %-22s -> %s\n", b.plan.name.c_str(),
                    !b.response      ? "(no answer)"
                    : b.response->ok ? "ok"
                                     : ("err " + (b.response->err ? b.response->err->code : "?")).c_str());
    }
    std::printf("  last error   %s\n", s.lastError().empty() ? "-" : s.lastError().c_str());
    const chat::ChatStats& cs = rt.chat.stats();
    std::printf("  route        %s%s%s\n", chat::name(rt.chat.routeState()), rt.chat.route().empty() ? "" : " ",
                rt.chat.route().c_str());
    std::printf("  send         %s\n",
                cs.sendsOk > 0        ? ("ok message " + cs.lastMessageId).c_str()
                : cs.sendsErr > 0     ? ("err " + cs.lastSendCode).c_str()
                : sent                ? "(no answer)"
                                      : "(not sent)");
    const chat::MessageRow* echoed = cs.lastMessageId.empty() ? nullptr : rt.chat.messages().get(cs.lastMessageId);
    std::printf("  messages     %zu read, %zu pending%s%s\n", rt.chat.messages().ready(), rt.chat.messages().pending(),
                echoed ? "; received: " : "", echoed ? echoed->text.c_str() : "");
    std::printf("  who am i     %d row(s)\n", cs.whoRows);

    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
        ok = ok && cond;
    };
    check(s.attaches() >= 2, "attached before and after the drop");
    check(s.resumes() >= 1, "reattach resumed the session");
    check(static_cast<int>(rt.client.opens()) > opensBeforeDrop, "socket reopened after the drop");
    check(rowsBeforeDrop > 0, "rows streamed before the drop");
    check(rows > rowsBeforeDrop, "rows streamed after the drop");
    check(rt.events.gaps().empty(), "no gap in the event store");
    check(rt.events.duplicates() == 0, "no duplicate reached the store");
    check(cs.sendsOk + cs.sendsErr > 0, "send was answered (ok or an err such as not-implemented)");
    if (cs.sendsOk > 0) {
        check(echoed != nullptr && echoed->text == kLine, "the sent Message came back as sent/delivered rows and read");
    }
    check(cs.whoRows > 0, "who am i printed the principal's fields");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

} // namespace ke
