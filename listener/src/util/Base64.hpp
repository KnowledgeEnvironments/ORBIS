// Base64 (RFC 4648, standard alphabet, padded): the JSON form of a `bytes` Field is a base64
// string (docs/protocol.md), so uploads encode and image views decode. Header-only, no deps.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ke::util {

inline std::string base64Encode(const void* data, std::size_t n) {
    static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | uint32_t(p[i + 2]);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
    }
    if (i < n) {
        uint32_t v = uint32_t(p[i]) << 16;
        if (i + 1 < n) {
            v |= uint32_t(p[i + 1]) << 8;
        }
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

inline std::string base64Encode(std::string_view s) {
    return base64Encode(s.data(), s.size());
}

// Decodes standard or url-safe base64, ignoring whitespace; false on a bad character.
inline bool base64Decode(std::string_view s, std::vector<unsigned char>& out) {
    out.clear();
    out.reserve(s.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : s) {
        int v = -1;
        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+' || c == '-') {
            v = 62;
        } else if (c == '/' || c == '_') {
            v = 63;
        } else if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        } else {
            return false;
        }
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

} // namespace ke::util
