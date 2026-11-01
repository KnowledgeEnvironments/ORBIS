#include "chat/Wav.hpp"

#include <catch2/catch_test_macros.hpp>

using ke::chat::wavFromPcm16;

TEST_CASE("a recording leaves as a plain WAV", "[voice]") {
    const std::string pcm("\x01\x02\x03\x04", 4);
    const std::string w = wavFromPcm16(pcm, 16000);
    CHECK(w.size() == 48);
    CHECK(w.substr(0, 4) == "RIFF");
    CHECK(w.substr(8, 8) == "WAVEfmt ");
    CHECK(static_cast<unsigned char>(w[4]) == 40); // 36 + the data
    CHECK(static_cast<unsigned char>(w[24]) == 0x80);
    CHECK(static_cast<unsigned char>(w[25]) == 0x3e); // 16000, little-endian
    CHECK(w.substr(36, 4) == "data");
    CHECK(w.substr(44) == pcm);
}
