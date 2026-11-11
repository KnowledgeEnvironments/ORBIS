#include "util/Qr.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace ke::qr {

namespace {

// ---- level M tables, versions 1..10 (index 0 unused) -----------------------------------------
constexpr int kTotalCodewords[11] = {0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
constexpr int kEcPerBlock[11] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
// blocks: {group 1 count, group 1 data codewords, group 2 count, group 2 data codewords}
constexpr int kBlocks[11][4] = {{0, 0, 0, 0},  {1, 16, 0, 0}, {1, 28, 0, 0}, {1, 44, 0, 0}, {2, 32, 0, 0}, {2, 43, 0, 0},
                                {4, 27, 0, 0}, {4, 31, 0, 0}, {2, 38, 2, 39}, {3, 36, 2, 37}, {4, 43, 1, 44}};
// alignment pattern centres (0 = none)
constexpr int kAlign[11][3] = {{0, 0, 0},  {0, 0, 0},  {6, 18, 0}, {6, 22, 0}, {6, 26, 0}, {6, 30, 0},
                               {6, 34, 0}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}};

int dataCodewords(int v) {
    return kBlocks[v][0] * kBlocks[v][1] + kBlocks[v][2] * kBlocks[v][3];
}

// ---- GF(256) ----------------------------------------------------------------------------------
struct Gf {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
            log[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if (x & 0x100) {
                x ^= 0x11D;
            }
        }
        for (int i = 255; i < 512; ++i) {
            exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
        }
    }
    std::uint8_t mul(std::uint8_t a, std::uint8_t b) const {
        if (a == 0 || b == 0) {
            return 0;
        }
        return exp[static_cast<std::size_t>(log[a] + log[b])];
    }
};

const Gf& gf() {
    static const Gf g;
    return g;
}

// ---- bit writer --------------------------------------------------------------------------------
struct Bits {
    std::vector<std::uint8_t> bits;
    void put(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((value >> i) & 1));
        }
    }
};

// ---- BCH for format and version ---------------------------------------------------------------
std::uint32_t bchRemainder(std::uint32_t value, std::uint32_t generator, int genBits, int shift) {
    std::uint32_t v = value << shift;
    for (int i = shift + genBits - 1 - (genBits - 1); i >= 0; --i) {
        // reduce from the top bit of the shifted value downwards
        (void)i;
        break;
    }
    // straightforward polynomial division
    const int top = 31 - __builtin_clz(generator);
    for (int i = 31; i >= top; --i) {
        if (v & (1u << i)) {
            v ^= generator << (i - top);
        }
    }
    return v;
}

// ---- matrix helpers ---------------------------------------------------------------------------
struct Grid {
    int size = 0;
    std::vector<std::uint8_t> dark;
    std::vector<std::uint8_t> function; // 1 where a function pattern or reserved area lives
    Grid(int n) : size(n), dark(static_cast<std::size_t>(n * n), 0), function(static_cast<std::size_t>(n * n), 0) {}
    std::size_t idx(int r, int c) const { return static_cast<std::size_t>(r * size + c); }
    void set(int r, int c, bool d, bool fn = true) {
        if (r < 0 || c < 0 || r >= size || c >= size) {
            return;
        }
        dark[idx(r, c)] = d ? 1 : 0;
        if (fn) {
            function[idx(r, c)] = 1;
        }
    }
    bool isFunction(int r, int c) const { return function[idx(r, c)] != 0; }
    bool get(int r, int c) const { return dark[idx(r, c)] != 0; }
};

void placeFinder(Grid& g, int r0, int c0) {
    for (int r = -1; r <= 7; ++r) {
        for (int c = -1; c <= 7; ++c) {
            const int rr = r0 + r, cc = c0 + c;
            if (rr < 0 || cc < 0 || rr >= g.size || cc >= g.size) {
                continue;
            }
            const bool border = r == -1 || r == 7 || c == -1 || c == 7;
            const bool ring = r == 0 || r == 6 || c == 0 || c == 6;
            const bool core = r >= 2 && r <= 4 && c >= 2 && c <= 4;
            g.set(rr, cc, !border && (ring || core));
        }
    }
}

void placeAlignment(Grid& g, int r0, int c0) {
    for (int r = -2; r <= 2; ++r) {
        for (int c = -2; c <= 2; ++c) {
            const bool on = std::abs(r) == 2 || std::abs(c) == 2 || (r == 0 && c == 0);
            g.set(r0 + r, c0 + c, on);
        }
    }
}

void placeFunctionPatterns(Grid& g, int version) {
    const int n = g.size;
    placeFinder(g, 0, 0);
    placeFinder(g, 0, n - 7);
    placeFinder(g, n - 7, 0);
    // timing
    for (int i = 8; i < n - 8; ++i) {
        g.set(6, i, i % 2 == 0);
        g.set(i, 6, i % 2 == 0);
    }
    // alignment
    std::vector<int> centres;
    for (int k = 0; k < 3; ++k) {
        if (kAlign[version][k] > 0) {
            centres.push_back(kAlign[version][k]);
        }
    }
    for (int a : centres) {
        for (int b : centres) {
            // skip the three that would sit on finders
            if ((a == 6 && b == 6) || (a == 6 && b == n - 7) || (a == n - 7 && b == 6)) {
                continue;
            }
            placeAlignment(g, a, b);
        }
    }
    // dark module
    g.set(4 * version + 9, 8, true);
    // reserve format areas
    for (int i = 0; i < 9; ++i) {
        if (!g.isFunction(8, i)) {
            g.set(8, i, false);
        }
        if (!g.isFunction(i, 8)) {
            g.set(i, 8, false);
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (!g.isFunction(8, n - 1 - i)) {
            g.set(8, n - 1 - i, false);
        }
        if (!g.isFunction(n - 1 - i, 8)) {
            g.set(n - 1 - i, 8, false);
        }
    }
    // reserve version areas
    if (version >= 7) {
        for (int i = 0; i < 6; ++i) {
            for (int j = 0; j < 3; ++j) {
                g.set(i, n - 11 + j, false);
                g.set(n - 11 + j, i, false);
            }
        }
    }
}

void placeData(Grid& g, const std::vector<std::uint8_t>& bits) {
    const int n = g.size;
    std::size_t k = 0;
    bool up = true;
    for (int col = n - 1; col > 0; col -= 2) {
        if (col == 6) {
            col = 5; // the timing column is skipped
        }
        for (int step = 0; step < n; ++step) {
            const int row = up ? n - 1 - step : step;
            for (int d = 0; d < 2; ++d) {
                const int c = col - d;
                if (g.isFunction(row, c)) {
                    continue;
                }
                const bool bit = k < bits.size() ? bits[k] != 0 : false;
                ++k;
                g.dark[g.idx(row, c)] = bit ? 1 : 0;
            }
        }
        up = !up;
    }
}

bool maskBit(int mask, int r, int c) {
    switch (mask) {
    case 0:
        return (r + c) % 2 == 0;
    case 1:
        return r % 2 == 0;
    case 2:
        return c % 3 == 0;
    case 3:
        return (r + c) % 3 == 0;
    case 4:
        return (r / 2 + c / 3) % 2 == 0;
    case 5:
        return (r * c) % 2 + (r * c) % 3 == 0;
    case 6:
        return ((r * c) % 2 + (r * c) % 3) % 2 == 0;
    default:
        return ((r + c) % 2 + (r * c) % 3) % 2 == 0;
    }
}

void applyMask(Grid& g, int mask) {
    for (int r = 0; r < g.size; ++r) {
        for (int c = 0; c < g.size; ++c) {
            if (!g.isFunction(r, c) && maskBit(mask, r, c)) {
                g.dark[g.idx(r, c)] ^= 1;
            }
        }
    }
}

void placeFormat(Grid& g, int ecl, int mask) {
    const std::uint32_t f = formatBits(ecl, mask);
    const int n = g.size;
    auto bit = [&](int i) { return ((f >> i) & 1) != 0; };
    // around the top-left finder (bits 14..0)
    const int rows[15] = {0, 1, 2, 3, 4, 5, 7, 8, 8, 8, 8, 8, 8, 8, 8};
    const int cols[15] = {8, 8, 8, 8, 8, 8, 8, 8, 7, 5, 4, 3, 2, 1, 0};
    for (int i = 0; i < 15; ++i) {
        g.set(rows[i], cols[i], bit(14 - i));
    }
    // second copy: bits 14..8 along the bottom-left column, 7..0 along the top-right row
    for (int i = 0; i < 7; ++i) {
        g.set(n - 1 - i, 8, bit(14 - i));
    }
    for (int i = 0; i < 8; ++i) {
        g.set(8, n - 8 + i, bit(7 - i));
    }
}

void placeVersion(Grid& g, int version) {
    if (version < 7) {
        return;
    }
    const std::uint32_t v = versionBits(version);
    const int n = g.size;
    for (int i = 0; i < 18; ++i) {
        const bool b = ((v >> i) & 1) != 0;
        const int a = i / 3, c = i % 3;
        g.set(a, n - 11 + c, b);
        g.set(n - 11 + c, a, b);
    }
}

int penalty(const Grid& g) {
    const int n = g.size;
    int score = 0;
    // rule 1: runs of 5+ in rows and columns
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < n; ++i) {
            int run = 1;
            for (int j = 1; j < n; ++j) {
                const bool a = pass == 0 ? g.get(i, j) : g.get(j, i);
                const bool b = pass == 0 ? g.get(i, j - 1) : g.get(j - 1, i);
                if (a == b) {
                    ++run;
                    if (run == 5) {
                        score += 3;
                    } else if (run > 5) {
                        ++score;
                    }
                } else {
                    run = 1;
                }
            }
        }
    }
    // rule 2: 2x2 blocks
    for (int r = 0; r + 1 < n; ++r) {
        for (int c = 0; c + 1 < n; ++c) {
            const bool v = g.get(r, c);
            if (g.get(r, c + 1) == v && g.get(r + 1, c) == v && g.get(r + 1, c + 1) == v) {
                score += 3;
            }
        }
    }
    // rule 3: finder-like patterns 1011101 with 4 light modules on a side
    auto seq = [&](int r, int c, int dr, int dc, const char* pat) {
        for (int i = 0; pat[i]; ++i) {
            const int rr = r + dr * i, cc = c + dc * i;
            if (rr < 0 || cc < 0 || rr >= n || cc >= n) {
                return false;
            }
            if (g.get(rr, cc) != (pat[i] == '1')) {
                return false;
            }
        }
        return true;
    };
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            if (seq(r, c, 0, 1, "10111010000") || seq(r, c, 0, 1, "00001011101")) {
                score += 40;
            }
            if (seq(r, c, 1, 0, "10111010000") || seq(r, c, 1, 0, "00001011101")) {
                score += 40;
            }
        }
    }
    // rule 4: dark proportion
    int darkCount = 0;
    for (std::uint8_t d : g.dark) {
        darkCount += d;
    }
    const int percent = darkCount * 100 / (n * n);
    const int prev = (percent / 5) * 5, next = prev + 5;
    score += std::min(std::abs(prev - 50) / 5, std::abs(next - 50) / 5) * 10;
    return score;
}

} // namespace

int capacityM(int version) {
    if (version < 1 || version > 10) {
        return 0;
    }
    const int bits = dataCodewords(version) * 8 - 4 - (version >= 10 ? 16 : 8);
    return bits / 8;
}

std::vector<std::uint8_t> rsEncode(const std::vector<std::uint8_t>& data, int count) {
    const Gf& f = gf();
    // generator polynomial: product of (x - a^i), i = 0..count-1
    std::vector<std::uint8_t> gen{1};
    for (int i = 0; i < count; ++i) {
        std::vector<std::uint8_t> next(gen.size() + 1, 0);
        const std::uint8_t ai = f.exp[static_cast<std::size_t>(i)];
        for (std::size_t j = 0; j < gen.size(); ++j) {
            next[j] ^= gen[j];
            next[j + 1] ^= f.mul(gen[j], ai);
        }
        gen = std::move(next);
    }
    std::vector<std::uint8_t> rem(static_cast<std::size_t>(count), 0);
    for (std::uint8_t d : data) {
        const std::uint8_t factor = d ^ rem[0];
        rem.erase(rem.begin());
        rem.push_back(0);
        if (factor != 0) {
            for (std::size_t j = 0; j < rem.size(); ++j) {
                rem[j] ^= f.mul(gen[j + 1], factor);
            }
        }
    }
    return rem;
}

std::uint32_t formatBits(int ecl, int mask) {
    const std::uint32_t data = (static_cast<std::uint32_t>(ecl & 3) << 3) | static_cast<std::uint32_t>(mask & 7);
    const std::uint32_t rem = bchRemainder(data, 0x537, 11, 10);
    return (((data << 10) | rem) ^ 0x5412) & 0x7FFF;
}

std::uint32_t versionBits(int version) {
    const std::uint32_t data = static_cast<std::uint32_t>(version & 63);
    const std::uint32_t rem = bchRemainder(data, 0x1F25, 13, 12);
    return ((data << 12) | rem) & 0x3FFFF;
}

Matrix encode(const std::string& text) {
    Matrix out;
    const int len = static_cast<int>(text.size());
    int version = 0;
    for (int v = 1; v <= 10; ++v) {
        if (len <= capacityM(v)) {
            version = v;
            break;
        }
    }
    if (version == 0) {
        return out;
    }
    // data bits
    Bits b;
    b.put(0b0100, 4);
    b.put(static_cast<std::uint32_t>(len), version >= 10 ? 16 : 8);
    for (unsigned char ch : text) {
        b.put(ch, 8);
    }
    const int dataCw = dataCodewords(version);
    const int capBits = dataCw * 8;
    const int terminator = std::min(4, capBits - static_cast<int>(b.bits.size()));
    b.put(0, terminator);
    while (b.bits.size() % 8 != 0) {
        b.bits.push_back(0);
    }
    std::vector<std::uint8_t> data;
    for (std::size_t i = 0; i < b.bits.size(); i += 8) {
        std::uint8_t byte = 0;
        for (int j = 0; j < 8; ++j) {
            byte = static_cast<std::uint8_t>((byte << 1) | b.bits[i + static_cast<std::size_t>(j)]);
        }
        data.push_back(byte);
    }
    for (int pad = 0; static_cast<int>(data.size()) < dataCw; ++pad) {
        data.push_back(pad % 2 == 0 ? 0xEC : 0x11);
    }
    // blocks
    std::vector<std::vector<std::uint8_t>> blocks, ecs;
    std::size_t at = 0;
    for (int grp = 0; grp < 2; ++grp) {
        const int count = kBlocks[version][grp * 2], sz = kBlocks[version][grp * 2 + 1];
        for (int i = 0; i < count; ++i) {
            std::vector<std::uint8_t> blk(data.begin() + static_cast<long>(at), data.begin() + static_cast<long>(at + static_cast<std::size_t>(sz)));
            at += static_cast<std::size_t>(sz);
            ecs.push_back(rsEncode(blk, kEcPerBlock[version]));
            blocks.push_back(std::move(blk));
        }
    }
    std::vector<std::uint8_t> all;
    std::size_t maxLen = 0;
    for (const auto& blk : blocks) {
        maxLen = std::max(maxLen, blk.size());
    }
    for (std::size_t i = 0; i < maxLen; ++i) {
        for (const auto& blk : blocks) {
            if (i < blk.size()) {
                all.push_back(blk[i]);
            }
        }
    }
    for (int i = 0; i < kEcPerBlock[version]; ++i) {
        for (const auto& ec : ecs) {
            all.push_back(ec[static_cast<std::size_t>(i)]);
        }
    }
    std::vector<std::uint8_t> bits;
    for (std::uint8_t byte : all) {
        for (int i = 7; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((byte >> i) & 1));
        }
    }
    (void)kTotalCodewords;
    // matrix
    const int n = 17 + 4 * version;
    Grid base(n);
    placeFunctionPatterns(base, version);
    placeData(base, bits);
    int bestMask = 0, bestScore = 1 << 30;
    Grid best = base;
    for (int m = 0; m < 8; ++m) {
        Grid g = base;
        applyMask(g, m);
        placeFormat(g, 0 /* M */, m);
        placeVersion(g, version);
        const int sc = penalty(g);
        if (sc < bestScore) {
            bestScore = sc;
            bestMask = m;
            best = g;
        }
    }
    out.size = n;
    out.version = version;
    out.mask = bestMask;
    out.dark = std::move(best.dark);
    return out;
}

} // namespace ke::qr
