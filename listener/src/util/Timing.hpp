// Frame-time probe (LISTENER_TIMING=1 logs p50/p95 per series every 2 s; LISTENER_TIMING=spin
// also keeps drawing frames so the cost can be read without touching the keyboard). Header-only,
// no dependencies beyond the logger; off, every call is one branch on a cached bool.
#pragma once

#include "util/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ke::timing {

inline int mode() {
    static const int m = [] {
        const char* v = std::getenv("LISTENER_TIMING");
        if (!v || !*v || std::strcmp(v, "0") == 0) {
            return 0;
        }
        return std::strcmp(v, "spin") == 0 ? 2 : 1;
    }();
    return m;
}
inline bool enabled() { return mode() != 0; }
inline bool spin() { return mode() == 2; }

struct Series {
    std::vector<double> ms;
};

inline std::map<std::string, Series>& series() {
    static std::map<std::string, Series> s;
    return s;
}

inline void record(const char* name, double ms) {
    if (!enabled()) {
        return;
    }
    series()[name].ms.push_back(ms);
}

// Called once per frame; prints and clears every 2 s.
inline void flush(bool force = false) {
    if (!enabled()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    const auto now = Clock::now();
    if (!force && now - last < std::chrono::seconds(2)) {
        return;
    }
    last = now;
    std::string line;
    for (auto& [name, s] : series()) {
        if (s.ms.empty()) {
            continue;
        }
        std::sort(s.ms.begin(), s.ms.end());
        const double p50 = s.ms[s.ms.size() / 2];
        const double p95 = s.ms[std::min(s.ms.size() - 1, static_cast<std::size_t>(s.ms.size() * 95 / 100))];
        const double mx = s.ms.back();
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s %zu p50 %.2f p95 %.2f max %.2f | ", name.c_str(), s.ms.size(), p50, p95, mx);
        line += buf;
        s.ms.clear();
    }
    if (!line.empty()) {
        log::info("timing ms: {}", line);
    }
}

class Scope {
  public:
    explicit Scope(const char* name) : name_(name), on_(enabled()) {
        if (on_) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~Scope() {
        if (on_) {
            const auto d = std::chrono::steady_clock::now() - start_;
            record(name_, std::chrono::duration<double, std::milli>(d).count());
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    const char* name_;
    bool on_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace ke::timing
