#pragma once

// Platform-independent palette blitter. Coordinates refer to pixel edges;
// sampling at pixel centres avoids seams when scaling adjoining UI elements.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ZoomMath {

inline int Clamp(int n, int lo, int hi) { return n < lo ? lo : (n > hi ? hi : n); }
inline int Round(double n) { return static_cast<int>(std::floor(n + 0.5)); }
inline int Edge(double n) { return static_cast<int>(std::ceil(n - 0.5)); }

// Frame-rate independent ease-out for UI scale transitions. A long frame gap
// pauses the transition for one frame so focus changes do not cause a jump.
inline double EaseToward(double value, double target, unsigned elapsedMs) {
    if (value == target || !elapsedMs) return value;
    if (elapsedMs > 100) return value;
    value += (target - value) * (1.0 - std::exp(-static_cast<double>(elapsedMs) / 55.0));
    return std::fabs(target - value) < 0.02 ? target : value;
}

struct Rect { int left, top, right, bottom; };
struct Image {
    std::uint8_t* pixels;
    int width, height, pitch;
    Rect clip;
};

// The game uses palette index zero as transparent only in sprite mode.
// remap is the game's 256 x 256 destination/source colour blend table.
inline bool Blit(Image dst, Image src, int x, int y, int w, int h,
                 int sx, int sy, double scaleX, double scaleY,
                 double originX, double originY, unsigned flags,
                 const std::uint8_t* remap = 0)
{
    const unsigned mode = flags & 0xFFFF;
    if (mode > 3 || !dst.pixels || !src.pixels || w <= 0 || h <= 0 ||
        scaleX <= 0 || scaleY <= 0) return false;
    const double left = originX + (x - originX) * scaleX;
    const double top = originY + (y - originY) * scaleY;
    const int x0 = Clamp(Edge(left), Clamp(dst.clip.left, 0, dst.width), dst.width);
    const int y0 = Clamp(Edge(top), Clamp(dst.clip.top, 0, dst.height), dst.height);
    const int x1 = Clamp(Edge(left + w * scaleX), 0, Clamp(dst.clip.right, 0, dst.width));
    const int y1 = Clamp(Edge(top + h * scaleY), 0, Clamp(dst.clip.bottom, 0, dst.height));
    const bool flipX = (flags & 0x10000) != 0;
    const bool flipY = (flags & 0x20000) != 0;
    for (int dy = y0; dy < y1; ++dy) {
        const int iy = Clamp(static_cast<int>((dy + 0.5 - top) / scaleY), 0, h - 1);
        const int sourceY = sy + (flipY ? h - 1 - iy : iy);
        if (sourceY < 0 || sourceY >= src.height) continue;
        std::uint8_t* out = dst.pixels + static_cast<std::ptrdiff_t>(dy) * dst.pitch;
        const std::uint8_t* in = src.pixels + static_cast<std::ptrdiff_t>(sourceY) * src.pitch;
        for (int dx = x0; dx < x1; ++dx) {
            const int ix = Clamp(static_cast<int>((dx + 0.5 - left) / scaleX), 0, w - 1);
            const int sourceX = sx + (flipX ? w - 1 - ix : ix);
            if (sourceX < 0 || sourceX >= src.width) continue;
            const std::uint8_t c = in[sourceX];
            if (mode && !c) continue;
            if (mode == 2 && !out[dx]) continue; // draw onto nonzero pixels
            if (mode == 3 && out[dx]) continue;  // draw underneath
            out[dx] = mode == 1 && remap && !flipX ? remap[(out[dx] << 8) | c] : c;
        }
    }
    return true;
}

inline int LogicalExtent(int worldExtent, double worldScale, double uiScale)
{
    return Clamp(Round(worldExtent * worldScale / uiScale), 1, 32767);
}

struct MouseRemainder {
    double x, y;
    MouseRemainder() : x(0), y(0) {}
    void Apply(int& dx, int& dy, double scale) {
        x += dx / scale; y += dy / scale;
        dx = static_cast<int>(x); dy = static_cast<int>(y);
        x -= dx; y -= dy;
    }
};

} // namespace ZoomMath
