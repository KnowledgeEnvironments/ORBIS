// Does a frame ImGui laid out fit the swapchain texture it is about to draw into?
//
// ImGui's viewport is the truncation of DisplaySize * FramebufferScale. At a fractional pixel
// density that lands a pixel or two away from the swapchain, for ever, so an exact comparison
// refuses every frame. A real resize differs by far more. Within the tolerance the scale is
// nudged down until the viewport fits: Metal refuses a viewport larger than its target.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace ke {

struct FrameFit {
    bool draw = false;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

inline float fitScale(std::uint32_t swap, float display, float scale) {
    float s = scale;
    if (static_cast<std::uint32_t>(display * s) > swap) {
        s = static_cast<float>(swap) / display;
        while (static_cast<std::uint32_t>(display * s) > swap) {
            s = std::nextafter(s, 0.0f);
        }
    }
    return s;
}

inline FrameFit fitFrame(std::uint32_t swapW, std::uint32_t swapH, float displayW, float displayH, float scaleX,
                         float scaleY) {
    constexpr long tolerance = 3; // pixels of rounding; anything more is a resize in flight
    FrameFit fit{false, scaleX, scaleY};
    if (swapW == 0 || swapH == 0 || displayW <= 0.0f || displayH <= 0.0f) {
        return fit;
    }
    const long dw = static_cast<long>(displayW * scaleX) - static_cast<long>(swapW);
    const long dh = static_cast<long>(displayH * scaleY) - static_cast<long>(swapH);
    if (std::labs(dw) > tolerance || std::labs(dh) > tolerance) {
        return fit;
    }
    fit.draw = true;
    fit.scaleX = fitScale(swapW, displayW, scaleX);
    fit.scaleY = fitScale(swapH, displayH, scaleY);
    return fit;
}

} // namespace ke
