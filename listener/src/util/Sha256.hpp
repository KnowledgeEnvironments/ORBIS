// SHA-256 (FIPS 180-4) for the `file/sha256` Field of an attached file: the world records the
// same digest for blobs, so a dropped file and its blob can be matched. Header-only, no deps.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ke::util {

class Sha256 {
  public:
    Sha256() { reset(); }
    void reset() {
        h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        len_ = 0;
        bufLen_ = 0;
    }
    void update(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        len_ += n;
        while (n > 0) {
            const std::size_t take = std::min<std::size_t>(n, 64 - bufLen_);
            std::memcpy(buf_.data() + bufLen_, p, take);
            bufLen_ += take;
            p += take;
            n -= take;
            if (bufLen_ == 64) {
                block(buf_.data());
                bufLen_ = 0;
            }
        }
    }
    std::array<unsigned char, 32> digest() {
        const uint64_t bits = len_ * 8;
        const unsigned char one = 0x80;
        update(&one, 1);
        const unsigned char zero = 0;
        while (bufLen_ != 56) {
            update(&zero, 1);
        }
        unsigned char lenBytes[8];
        for (int i = 0; i < 8; ++i) {
            lenBytes[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        }
        update(lenBytes, 8);
        std::array<unsigned char, 32> out{};
        for (std::size_t i = 0; i < 8; ++i) {
            out[i * 4] = static_cast<unsigned char>(h_[i] >> 24);
            out[i * 4 + 1] = static_cast<unsigned char>(h_[i] >> 16);
            out[i * 4 + 2] = static_cast<unsigned char>(h_[i] >> 8);
            out[i * 4 + 3] = static_cast<unsigned char>(h_[i]);
        }
        return out;
    }
    static std::string hex(const void* data, std::size_t n) {
        Sha256 s;
        s.update(data, n);
        const auto d = s.digest();
        static constexpr char hx[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (const unsigned char b : d) {
            out.push_back(hx[b >> 4]);
            out.push_back(hx[b & 15]);
        }
        return out;
    }
    static std::string hex(std::string_view s) { return hex(s.data(), s.size()); }

  private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
                   uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }
    std::array<uint32_t, 8> h_{};
    std::array<unsigned char, 64> buf_{};
    std::size_t bufLen_ = 0;
    uint64_t len_ = 0;
};

} // namespace ke::util
