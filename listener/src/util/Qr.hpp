// A small QR encoder for the pairing link (sprint 2): byte mode, versions 1 to 10 picked by
// length, error correction level M, the mask chosen by the standard penalty. No dependency; the
// matrix is drawn as squares by whoever shows it. Not a general encoder: no alphanumeric or
// kanji modes, no ECI, nothing above version 10 (213 bytes at level M).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ke::qr {

struct Matrix {
    int size = 0;      // modules per side: 17 + 4 * version
    int version = 0;   // 1..10, 0 when the text did not fit
    int mask = -1;     // the mask pattern used
    std::vector<std::uint8_t> dark; // size*size, row-major, 1 = dark

    bool ok() const { return version > 0; }
    bool at(int row, int col) const { return dark[static_cast<std::size_t>(row * size + col)] != 0; }
};

// Encode `text` as bytes. An empty matrix (version 0) when it does not fit version 10-M.
Matrix encode(const std::string& text);

// Pieces exposed for the tests (and anyone who wants them).
// Reed-Solomon error-correction codewords for `data` (GF(256), 0x11D), `count` of them.
std::vector<std::uint8_t> rsEncode(const std::vector<std::uint8_t>& data, int count);
// The 15-bit format information for level M and `mask` (BCH(15,5), masked with 0x5412);
// `ecl` as the two level bits (L 01, M 00, Q 11, H 10).
std::uint32_t formatBits(int ecl, int mask);
// The 18-bit version information (BCH(18,6)) for versions 7 and up.
std::uint32_t versionBits(int version);
// Byte capacity of a version at level M.
int capacityM(int version);

} // namespace ke::qr
