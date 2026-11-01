// Box drawing, block elements and braille as shapes, not font glyphs: drawn from the cell, so lines
// meet the next cell exactly and a sparkline's bars are exact, whatever the font has for them.
#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace ke::modules {

inline constexpr float kPi = 3.14159265358979f;

// The one code point of a cell's text, or 0 when it holds more or is not valid UTF-8.
inline std::uint32_t soleCodePoint(std::string_view s) {
    if (s.empty()) {
        return 0;
    }
    const auto b = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    if (b(0) < 0x80) {
        return s.size() == 1 ? b(0) : 0;
    }
    if ((b(0) & 0xE0) == 0xC0 && s.size() == 2) {
        return ((b(0) & 0x1Fu) << 6) | (b(1) & 0x3Fu);
    }
    if ((b(0) & 0xF0) == 0xE0 && s.size() == 3) {
        return ((b(0) & 0x0Fu) << 12) | ((b(1) & 0x3Fu) << 6) | (b(2) & 0x3Fu);
    }
    return 0;
}

// Draws cp into the cell if it is one of these. False leaves it to the font.
inline bool drawCellGlyph(ImDrawList* dl, std::uint32_t cp, ImVec2 a, float w, float h, ImU32 col) {
    const ImVec2 z(a.x + w, a.y + h);
    const float cx = std::floor(a.x + w / 2.0f), cy = std::floor(a.y + h / 2.0f);
    const auto fill = [&](float x0, float y0, float x1, float y1, ImU32 c) { dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), c); };

    // blocks: U+2580..U+259F
    if (cp >= 0x2581 && cp <= 0x2588) { // lower eighths, up to the full block
        fill(a.x, z.y - h * static_cast<float>(cp - 0x2580) / 8.0f, z.x, z.y, col);
        return true;
    }
    if (cp >= 0x2589 && cp <= 0x258F) { // left eighths, seven down to one
        fill(a.x, a.y, a.x + w * static_cast<float>(0x2590 - cp) / 8.0f, z.y, col);
        return true;
    }
    switch (cp) {
    case 0x2580: fill(a.x, a.y, z.x, a.y + h / 2.0f, col); return true;
    case 0x2590: fill(a.x + w / 2.0f, a.y, z.x, z.y, col); return true;
    case 0x2594: fill(a.x, a.y, z.x, a.y + h / 8.0f, col); return true;
    case 0x2595: fill(z.x - w / 8.0f, a.y, z.x, z.y, col); return true;
    case 0x2591:
    case 0x2592:
    case 0x2593: {
        const ImU32 alpha = (col >> IM_COL32_A_SHIFT) & 0xFFu;
        fill(a.x, a.y, z.x, z.y, (col & ~IM_COL32_A_MASK) | ((alpha * (cp - 0x2590) / 4u) << IM_COL32_A_SHIFT));
        return true;
    }
    default:
        break;
    }

    // braille: U+2800..U+28FF, dots by bit
    if (cp >= 0x2801 && cp <= 0x28FF) {
        static constexpr int kCol[8] = {0, 0, 0, 1, 1, 1, 0, 1};
        static constexpr int kRow[8] = {0, 1, 2, 0, 1, 2, 3, 3};
        const float r = std::max(1.0f, std::floor(w / 7.0f));
        for (int bit = 0; bit < 8; ++bit) {
            if ((cp - 0x2800) & (1u << bit)) {
                const float x = a.x + w * (kCol[bit] == 0 ? 0.3f : 0.7f), y = a.y + h * (0.125f + 0.25f * static_cast<float>(kRow[bit]));
                fill(x - r, y - r, x + r, y + r, col);
            }
        }
        return true;
    }

    // box drawing: the arms that leave the centre of the cell
    enum : unsigned { U = 1, D = 2, L = 4, R = 8 };
    unsigned arms = 0;
    bool heavy = false, arc = false;
    switch (cp) {
    case 0x2501: heavy = true; [[fallthrough]];
    case 0x2500: arms = L | R; break;
    case 0x2503: heavy = true; [[fallthrough]];
    case 0x2502: arms = U | D; break;
    case 0x250F: heavy = true; [[fallthrough]];
    case 0x250C: arms = R | D; break;
    case 0x2513: heavy = true; [[fallthrough]];
    case 0x2510: arms = L | D; break;
    case 0x2517: heavy = true; [[fallthrough]];
    case 0x2514: arms = R | U; break;
    case 0x251B: heavy = true; [[fallthrough]];
    case 0x2518: arms = L | U; break;
    case 0x2523: heavy = true; [[fallthrough]];
    case 0x251C: arms = U | D | R; break;
    case 0x252B: heavy = true; [[fallthrough]];
    case 0x2524: arms = U | D | L; break;
    case 0x2533: heavy = true; [[fallthrough]];
    case 0x252C: arms = L | R | D; break;
    case 0x253B: heavy = true; [[fallthrough]];
    case 0x2534: arms = L | R | U; break;
    case 0x254B: heavy = true; [[fallthrough]];
    case 0x253C: arms = U | D | L | R; break;
    case 0x256D: arms = R | D; arc = true; break;
    case 0x256E: arms = L | D; arc = true; break;
    case 0x256F: arms = L | U; arc = true; break;
    case 0x2570: arms = R | U; arc = true; break;
    default: break;
    }
    const float t = std::max(1.0f, std::floor(h / (heavy ? 8.0f : 16.0f)));
    if (arms != 0 && arc) {
        const float rad = std::floor(std::min(w, h) / 2.0f);
        const float ex = (arms & R) ? cx + rad : cx - rad;
        const float ey = (arms & D) ? cy + rad : cy - rad;
        const float start = (arms & R) ? ((arms & D) ? kPi : kPi / 2.0f) : ((arms & D) ? kPi * 1.5f : 0.0f);
        const float off = t / 2.0f;
        dl->PathArcTo(ImVec2(ex + off, ey + off), rad, start, start + kPi / 2.0f, 10);
        dl->PathStroke(col, t);
        if (arms & R) {
            fill(ex, cy, z.x, cy + t, col);
        } else {
            fill(a.x, cy, ex + t, cy + t, col);
        }
        if (arms & D) {
            fill(cx, ey, cx + t, z.y, col);
        } else {
            fill(cx, a.y, cx + t, ey + t, col);
        }
        return true;
    }
    if (arms != 0) {
        if (arms & L) { fill(a.x, cy, cx + t, cy + t, col); }
        if (arms & R) { fill(cx, cy, z.x, cy + t, col); }
        if (arms & U) { fill(cx, a.y, cx + t, cy + t, col); }
        if (arms & D) { fill(cx, cy, cx + t, z.y, col); }
        return true;
    }

    // double lines: two strokes, either side of the centre
    const float g = std::max(2.0f, std::floor(h / 8.0f));
    const float one = std::max(1.0f, std::floor(h / 16.0f));
    const auto horiz = [&](float x0, float x1, float y) { fill(x0, y, x1, y + one, col); };
    const auto vert = [&](float x, float y0, float y1) { fill(x, y0, x + one, y1, col); };
    switch (cp) {
    case 0x2550: horiz(a.x, z.x, cy - g); horiz(a.x, z.x, cy + g); return true;
    case 0x2551: vert(cx - g, a.y, z.y); vert(cx + g, a.y, z.y); return true;
    case 0x2554: horiz(cx - g, z.x, cy - g); horiz(cx + g, z.x, cy + g); vert(cx - g, cy - g, z.y); vert(cx + g, cy + g, z.y); return true;
    case 0x2557: horiz(a.x, cx + g + one, cy - g); horiz(a.x, cx - g + one, cy + g); vert(cx + g, cy - g, z.y); vert(cx - g, cy + g, z.y); return true;
    case 0x255A: horiz(cx - g, z.x, cy + g); horiz(cx + g, z.x, cy - g); vert(cx - g, a.y, cy + g + one); vert(cx + g, a.y, cy - g + one); return true;
    case 0x255D: horiz(a.x, cx + g + one, cy + g); horiz(a.x, cx - g + one, cy - g); vert(cx + g, a.y, cy + g + one); vert(cx - g, a.y, cy - g + one); return true;
    default: return false;
    }
}

} // namespace ke::modules
