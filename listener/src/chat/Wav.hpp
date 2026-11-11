// A WAV around 16-bit mono PCM: what a recording leaves the client as. Pure, so a test reads the header.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ke::chat {

inline std::string wavFromPcm16(std::string_view pcm, std::uint32_t rate) {
    const auto put = [](std::string& out, std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
        }
    };
    const auto size = static_cast<std::uint32_t>(pcm.size());
    std::string out;
    out.reserve(44 + pcm.size());
    out += "RIFF";
    put(out, 36 + size, 4);
    out += "WAVEfmt ";
    put(out, 16, 4);       // the PCM format block
    put(out, 1, 2);        // PCM
    put(out, 1, 2);        // mono
    put(out, rate, 4);     // samples a second
    put(out, rate * 2, 4); // bytes a second
    put(out, 2, 2);        // bytes a frame
    put(out, 16, 2);       // bits a sample
    out += "data";
    put(out, size, 4);
    out.append(pcm);
    return out;
}

} // namespace ke::chat
