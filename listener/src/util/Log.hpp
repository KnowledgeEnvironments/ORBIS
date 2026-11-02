// Minimal stderr logger with the `orbis` prefix and a monotonic timestamp. No dependencies so listener_core stays small.
#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include <chrono>
#include <cstdio>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace ke::log {

enum class Level { Debug, Info, Warn, Error };

namespace detail {
inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}
// The last lines, for [ COPY LOG ] in the SESSION pane (bug reports without a terminal at hand).
inline std::deque<std::string>& ring() {
    static std::deque<std::string> r;
    return r;
}
constexpr std::size_t kRing = 400;
} // namespace detail

inline void write(Level level, std::string_view msg) {
#ifdef NDEBUG
    if (level == Level::Debug) {
        return;
    }
#endif
    static const auto start = std::chrono::steady_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    const char* tag = level == Level::Debug ? "debug"
                      : level == Level::Info ? "info"
                      : level == Level::Warn ? "warn"
                                             : "error";
    std::lock_guard<std::mutex> lock(detail::mutex());
    std::fprintf(stderr, "orbis [%8lld ms] %-5s %.*s\n", static_cast<long long>(ms), tag,
                 static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);
#if defined(__ANDROID__)
    __android_log_print(level == Level::Debug  ? ANDROID_LOG_DEBUG
                        : level == Level::Info ? ANDROID_LOG_INFO
                        : level == Level::Warn ? ANDROID_LOG_WARN
                                               : ANDROID_LOG_ERROR,
                        "orbis", "%.*s", static_cast<int>(msg.size()), msg.data());
#endif
    std::deque<std::string>& r = detail::ring();
    r.push_back(std::format("[{:8} ms] {:<5} {}", static_cast<long long>(ms), tag, msg));
    if (r.size() > detail::kRing) {
        r.pop_front();
    }
}

// The last `n` log lines, newest last, joined by newlines.
inline std::string recent(std::size_t n) {
    std::lock_guard<std::mutex> lock(detail::mutex());
    const std::deque<std::string>& r = detail::ring();
    std::string out;
    const std::size_t from = r.size() > n ? r.size() - n : 0;
    for (std::size_t i = from; i < r.size(); ++i) {
        out += r[i];
        out += '\n';
    }
    return out;
}

template <class... Args> void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args> void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args> void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args> void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace ke::log
