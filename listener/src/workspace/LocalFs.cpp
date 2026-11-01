#include "workspace/LocalFs.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <unistd.h>

namespace ke::workspace {

std::string hostName() {
    std::array<char, 256> buf{};
    if (gethostname(buf.data(), buf.size()) != 0) {
        return "localhost";
    }
    buf.back() = '\0';
    std::string h(buf.data());
    if (auto dot = h.find('.'); dot != std::string::npos) {
        h.erase(dot);
    }
    return h.empty() ? "localhost" : h;
}

// Two FNV-1a 64-bit hashes laid out as an RFC 4122 string. Not a real v5 uuid; it only needs to
// be stable per host so the world's answer is reproducible while the host Object does not exist.
std::string placeholderUuid(const std::string& seed) {
    auto fnv = [](const std::string& s, std::uint64_t basis) {
        std::uint64_t h = basis;
        for (char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return h;
    };
    const std::uint64_t a = fnv(seed, 0xcbf29ce484222325ULL);
    const std::uint64_t b = fnv(seed + "/b", 0x84222325cbf29ce4ULL);
    return std::format("{:08x}-{:04x}-5{:03x}-{:04x}-{:012x}", static_cast<std::uint32_t>(a >> 32),
                       static_cast<std::uint16_t>(a >> 16), static_cast<std::uint16_t>(a & 0x0fff),
                       static_cast<std::uint16_t>(0x8000 | (b >> 48 & 0x3fff)), b & 0xffffffffffffULL);
}

BindPlan localFsBind() {
    const std::string host = hostName();
    return BindPlan{"workspace (local filesystem)", "workspace/" + host,
                    placeholderUuid("workspace/" + host)};
}

} // namespace ke::workspace
