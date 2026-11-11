// The QR encoder behind the pairing link: Reed-Solomon against a published vector, the format
// information against the standard's table, and the structure of a code for a real link.
#include "util/Qr.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;

TEST_CASE("Reed-Solomon codewords match the published 1-M example") {
    // The worked example in the QR tutorial literature (thonky): 16 data codewords, 10 EC.
    const std::vector<std::uint8_t> data{32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17};
    const std::vector<std::uint8_t> ec = qr::rsEncode(data, 10);
    CHECK(ec == std::vector<std::uint8_t>{196, 35, 39, 119, 235, 215, 231, 226, 93, 23});
}

TEST_CASE("format and version information match the standard") {
    CHECK(qr::formatBits(0 /* M */, 0) == 0b101010000010010);
    CHECK(qr::formatBits(1 /* L */, 0) == 0b111011111000100);
    CHECK(qr::formatBits(0 /* M */, 7) == 0b100101010100000); // table row M, mask 7
    CHECK(qr::versionBits(7) == 0b000111110010010100);
    CHECK(qr::versionBits(10) == 0b001010010011010011);
}

TEST_CASE("byte capacities at level M and the version picked for a pairing link") {
    CHECK(qr::capacityM(1) == 14);
    CHECK(qr::capacityM(4) == 62);
    CHECK(qr::capacityM(10) == 213);
    const std::string link = "orbis://pair?url=ws%3A%2F%2F192.0.2.10%3A7777%2Fws&code=482913&label=duo"; // 72 bytes: version 5
    const qr::Matrix m = qr::encode(link);
    REQUIRE(m.ok());
    CHECK(m.version == 5);
    CHECK(m.size == 37);
    // finder patterns: dark ring and core, light separator
    for (const auto& [r0, c0] : std::vector<std::pair<int, int>>{{0, 0}, {0, 30}, {30, 0}}) {
        CHECK(m.at(r0, c0));
        CHECK(m.at(r0 + 3, c0 + 3));
        CHECK_FALSE(m.at(r0 + 1, c0 + 1));
        CHECK(m.at(r0 + 6, c0 + 6));
    }
    CHECK_FALSE(m.at(7, 7)); // separator
    // timing pattern alternates between the finders
    for (int i = 8; i < 29; ++i) {
        CHECK(m.at(6, i) == (i % 2 == 0));
        CHECK(m.at(i, 6) == (i % 2 == 0));
    }
    CHECK(m.at(4 * 5 + 9, 8)); // the dark module
    // alignment pattern at (30, 30) for version 5: dark centre, dark ring, light inside
    CHECK(m.at(30, 30));
    CHECK_FALSE(m.at(29, 30));
    CHECK(m.at(28, 30));
    CHECK(m.mask >= 0);
    CHECK(m.mask <= 7);
    CHECK_FALSE(qr::encode(std::string(214, 'x')).ok());
    CHECK(qr::encode(std::string(213, 'x')).version == 10);
}
