// The globe panel's shockwave: what a hit sets off around the moon.
//   - a pressure wave: a thin cream and violet circle that races out to the panel's edge, lighting the grid lines it
//     crosses and leaving a dithered wake;
//   - a blast in the plane of the moon's own ring: a thick orange ring, printed with a violet misregistration, that
//     sweeps out more slowly, its far half behind the moon and its near half over it;
//   - sparks thrown out from behind the moon.
//
// Every pose is worked out from the hits' ages alone, so a frame lands on the same pixels at any frame rate, a paint
// repeated at the same time touches nothing, and a partial paint equals a whole one. Nothing is blended: each part
// fades by stepping down a six-ink ramp of the page's own colours. Rings are filled a row at a time between exact
// edges, a square root a row for each edge, and bands meet without gaps. Four hits can ring at once; a fifth takes
// the oldest's place.
#pragma once

#include "raster.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace hdui
{
// `a` taken `part` / `whole` of the way to `b`, each channel rounded: the shockwave's in-between shades are mixes of
// two of the page's own colours.
constexpr Ink mix (Ink a, Ink b, int part, int whole)
{
    Ink out = 0xff000000u;
    for (int shift = 0; shift < 24; shift += 8)
    {
        const int x = (int) ((a >> shift) & 255u), y = (int) ((b >> shift) & 255u);
        out |= (Ink) ((x * (whole - part) + y * part + whole / 2) / whole) << shift;
    }
    return out;
}

// The shockwave's six-step ramps, brightest first: the cream edge of a ring, its orange band, the violet of the
// pressure wave and the misprint, the grid's lights and the dithered wake. The page gives them, so the shockwave wears
// the page's colours.
struct ShockInks
{
    Ink edge[6], body[6], violet[6], glow[6], wake[6];
};

class Shockwave
{
public:
    explicit Shockwave (const ShockInks& inks) : ink (inks) {}

    // The moon's centre on the globe panel: the sprite's sphere (its rows 10-101, columns 29-126), drawn at 5, 2.
    static constexpr int cx = 83, cy = 58;
    // The panel's grid: lines every 15 pixels, vertical ones from x 6 over rows 5-114, horizontal ones from y 7 over
    // columns 5-158. Everything stays inside the grid's box.
    static constexpr int gridX = 6, gridY = 7, gridStep = 15, left = 5, top = 5, right = 158, bottom = 114;

    // A hit at `now` (ms). `level` is the hit's strength (1 a full hit, up to 2 with Accent),
    // `lifeMs` how long its blast lasts, and `seed` scatters its sparks.
    void strike (uint64_t now, float level, float lifeMs, uint32_t seed)
    {
        Wave& w = waves[next];
        next = (next + 1) % slots;
        w.on = true;
        w.at = now;
        // (Written so that a value that is not a number lands on the short or small end.)
        w.life = lifeMs >= 1.0f ? std::min (lifeMs, 60000.0f) : 1.0f;
        w.size = level >= 0.15f ? std::min (level, 1.3f) : 0.15f;
        w.sparks = std::min (sparksMax, 6 + (int) std::lround (8.0f * std::min (w.size, 1.25f)));
        uint32_t s = seed * 2654435761u + 0x9e3779b9u;
        auto random = [&s] {
            s ^= s << 13, s ^= s >> 17, s ^= s << 5;
            return (float) (s >> 8) * (1.0f / 16777216.0f);
        };
        const float turn = 6.28318531f / (float) w.sparks, spin = turn * random();
        for (int k = 0; k < w.sparks; ++k)
        {
            const float angle = spin + turn * ((float) k + 0.7f * (random() - 0.5f));
            w.dx[k] = std::cos (angle);
            w.dy[k] = std::sin (angle);
            w.speed[k] = 1.3f + 0.9f * random();
        }
    }

    bool live (uint64_t now) const
    {
        for (const Wave& w : waves)
            if (w.on && now >= w.at && (float) (now - w.at) < w.life)
                return true;
        return false;
    }

    // Beneath the grid: each pressure wave's dithered wake.
    void under (const Raster& r, uint64_t now) const
    {
        each (now, [&] (const Wave& w, const Pose& p) {
            if (p.pressure >= 1.0f) return;
            const float inner = p.radius - p.thick, width = (5.0f + 11.0f * w.size) * (1.0f - p.pressure);
            const float radii[] = { inner, inner - width };
            const Ink inks[] = { ink.wake[p.pressureFade] };
            circle (r, radii, inks, true);
        });
    }

    // Over the grid, beneath the moon: the pressure waves and the grid's lights, the blasts' far halves, the sparks.
    void behind (const Raster& r, uint64_t now) const
    {
        each (now, [&] (const Wave& w, const Pose& p) {
            if (p.pressure < 1.0f)
            {
                const float inner = p.radius - p.thick;
                const float radii[] = { p.radius, p.radius - 1.0f, inner };
                const Ink inks[] = { ink.edge[p.pressureFade], ink.violet[p.pressureFade] };
                circle (r, radii, inks, false);
                lights (r, inner, p.radius, 3.0f * std::min (w.size, 1.0f) * (1.0f - p.pressure), ink.glow[p.pressureFade]);
            }
            blast (r, p, false);
            for (int s = 0; s < w.sparks; ++s)
            {
                const float t = p.u * w.speed[s];
                if (t >= 1.0f) continue;
                const float head = start + (110.0f + 30.0f * w.size - start) * outwards (t);
                const float tail = head - (4.0f + 18.0f * (1.0f - t)) * std::min (1.0f, 0.4f + w.size);
                const int fade = std::min (5, p.fade + (int) (4.0f * t));
                const Ink trail = s % 3 == 0 ? ink.edge[fade] : (s % 3 == 1 ? ink.body[fade] : ink.violet[fade]);
                const int hx = cx + (int) std::lround (w.dx[s] * head), hy = cy + (int) std::lround (w.dy[s] * head);
                const int tx = cx + (int) std::lround (w.dx[s] * tail), ty = cy + (int) std::lround (w.dy[s] * tail);
                // Two pixels wide while fresh: the second line beside the first, across the spark's way.
                const bool steep = std::fabs (w.dy[s]) > std::fabs (w.dx[s]);
                line (r, tx, ty, hx, hy, trail);
                if (t < 0.5f) line (r, tx + steep, ty + ! steep, hx + steep, hy + ! steep, trail);
                dot (r, hx, hy, ink.edge[std::max (0, fade - 1)]);
            }
        });
    }

    // Over the moon: the blasts' near halves.
    void front (const Raster& r, uint64_t now) const
    {
        each (now, [&] (const Wave&, const Pose& p) { blast (r, p, true); });
    }

private:
    static constexpr int slots = 4, sparksMax = 16;
    // The pressure wave and sparks start inside the moon, so they come out from behind it, even with the moon
    // squashed to 40 pixels' half height by an accented hit.
    static constexpr float start = 34.0f;
    // The blast's plane is the moon's ring: an ellipse about (83, 60) leaning 18.3 degrees up to the right, 0.178 as
    // tall as it is wide. Its near half is the lower one.
    static constexpr int ex = 83, ey = 60;
    static constexpr float tiltCos = 0.949425f, tiltSin = -0.313992f, flat = 0.178f;

    struct Wave
    {
        bool on = false;
        uint64_t at = 0;
        float life = 1.0f, size = 0.0f;
        int sparks = 0;
        float dx[sparksMax] {}, dy[sparksMax] {}, speed[sparksMax] {};
    };
    struct Pose
    {
        float u;                     // the blast's age over its life
        float pressure, radius, thick; // the pressure wave's age over its life (1: gone), outer radius and width
        float reach, band;           // the blast's half width along the ring and its band's width there (at least 8)
        int fade, pressureFade, shift;
    };
    const ShockInks& ink;
    std::array<Wave, slots> waves {};
    int next = 0; // the slot the next hit takes: the oldest

    static float outwards (float u) { const float v = 1.0f - u; return 1.0f - v * v * v; } // fast, then settling

    // Bright at first, then down the ramp; a soft hit starts a step or two down it.
    static int fading (float size, float u)
    {
        return std::min (5, (size < 0.45f ? 2 : (size < 0.75f ? 1 : 0)) + (int) (6.0f * u * u));
    }

    template<typename Draw>
    void each (uint64_t now, Draw draw) const
    {
        for (int k = 0; k < slots; ++k)
        {
            const Wave& w = waves[(next + k) % slots]; // the oldest first, so the newest lies on top
            if (! w.on || now < w.at || (float) (now - w.at) >= w.life) continue;
            Pose p;
            p.u = (float) (now - w.at) / w.life;
            p.pressure = std::min (1.0f, p.u * 1.7f);
            const float q = 1.0f - p.pressure;
            p.radius = start + (60.0f + 40.0f * w.size - start) * (1.0f - q * q);
            p.thick = 1.5f + 2.5f * std::min (w.size, 1.2f) * q;
            p.pressureFade = fading (w.size, p.pressure);
            // The blast slows but keeps moving: a full hit's arcs leave the panel's top and bottom at about 0.85
            // of its life. (Easing out to a stop would leave two still lines at the end.)
            p.reach = 47.0f + (48.0f + 60.0f * w.size) * p.u * (1.6f - 0.6f * p.u);
            p.band = std::max (8.0f, 2.0f + (5.0f + 8.0f * std::min (w.size, 1.2f)) * (1.0f - p.u));
            p.fade = fading (w.size, p.u);
            p.shift = (int) std::lround (3.0f * (1.0f - p.u));
            draw (w, p);
        }
    }

    static bool inside (int x, int y) { return x >= left && x <= right && y >= top && y <= bottom; }
    static void dot (const Raster& r, int x, int y, Ink c)
    {
        if (inside (x, y)) r.px[y * r.w + x] = c;
    }
    static void line (const Raster& r, int x, int y, int endX, int endY, Ink c)
    {
        const int dx = std::abs (endX - x), dy = -std::abs (endY - y);
        const int sx = x < endX ? 1 : -1, sy = y < endY ? 1 : -1;
        for (int error = dx + dy;;)
        {
            dot (r, x, y, c);
            if (x == endX && y == endY) return;
            const int twice = 2 * error;
            if (twice >= dy) error += dy, x += sx;
            if (twice <= dx) error += dx, y += sy;
        }
    }
    static void run (Ink* pixels, int row, int from, int to, Ink c, bool dither)
    {
        from = std::max (from, (int) left), to = std::min (to, (int) right);
        if (! dither)
        {
            if (from <= to) std::fill (pixels + from, pixels + to + 1, c);
            return;
        }
        for (int x = from + ((from + row) & 1); x <= to; x += 2) pixels[x] = c;
    }

    // The widest |offset| whose pixel centre lies inside a circle of `radius`, on a row `d2` (squared) from its
    // centre; -1 when the circle misses the row.
    static int half (float radius, float d2)
    {
        const float rest = radius * radius - d2;
        return radius > 0.0f && rest > 0.0f ? (int) std::ceil (std::sqrt (rest)) - 1 : -1;
    }

    // Concentric bands around the moon, outermost first: band i lies between radii[i] and radii[i + 1] and is filled
    // with inks[i]; `dither` fills one colour of a checkerboard. A pixel is in a band when its centre is inside the
    // outer circle and not inside the inner one.
    template<int N>
    static void circle (const Raster& r, const float (&radii)[N], const Ink (&inks)[N - 1], bool dither)
    {
        if (radii[0] <= 0.0f) return;
        const int reach = (int) std::ceil (radii[0]);
        for (int row = std::max (top, cy - reach + 1), last = std::min (bottom, cy + reach - 1); row <= last; ++row)
        {
            const float d2 = (float) ((row - cy) * (row - cy));
            int widest[N];
            for (int i = 0; i < N; ++i) widest[i] = half (radii[i], d2);
            Ink* pixels = r.px + row * r.w;
            for (int i = 0; i + 1 < N && widest[i] >= 0; ++i)
            {
                const int from = widest[i + 1] + 1, to = widest[i];
                if (from > to) continue;
                if (from == 0)
                    run (pixels, row, cx - to, cx + to, inks[i], dither);
                else
                {
                    run (pixels, row, cx - to, cx - from, inks[i], dither);
                    run (pixels, row, cx + from, cx + to, inks[i], dither);
                }
            }
        }
    }

    // A blast's ring: the violet misprint, then the cream edge and orange band over it; the far half or the near one.
    void blast (const Raster& r, const Pose& p, bool near) const
    {
        // Widths along the ring's long axis: across it they are `flat` as wide, so the cream edge is kept to a
        // pixel there and the whole band to 1.4 pixels.
        const float inner = std::max (0.0f, p.reach - p.band), edge = p.reach - std::max (1.0f / flat, 0.4f * p.band);
        const float misprint[] = { p.reach, inner };
        const Ink misprintInk[] = { ink.violet[p.fade] };
        ellipse (r, ex - p.shift, ey + p.shift, misprint, misprintInk, near);
        const float ring[] = { p.reach, edge, inner };
        const Ink ringInks[] = { ink.edge[p.fade], ink.body[p.fade] };
        ellipse (r, ex, ey, ring, ringInks, near);
    }

    // Bands between scaled copies of the ring's ellipse about (x, y), widest first (`reach` is each one's half width
    // along the ring), as circle() does them; only the near half (below the ring's long axis) or only the far half.
    // On a row, every copy's pixels are centred on the same point, so each copy costs one square root a row.
    template<int N>
    static void ellipse (const Raster& r, int x, int y, const float (&reach)[N], const Ink (&inks)[N - 1], bool near)
    {
        if (reach[0] <= 0.0f) return;
        // A point (u, v) from the centre is inside the copy of half width a when A u^2 + B(v) u + C(v) < a^2.
        constexpr float k2 = 1.0f / (flat * flat);
        constexpr float A = tiltCos * tiltCos + tiltSin * tiltSin * k2;
        constexpr float Bv = 2.0f * tiltCos * tiltSin * (1.0f - k2), Cv = tiltSin * tiltSin + tiltCos * tiltCos * k2;
        // The rows the widest copy reaches: its half height is a * sqrt (sin^2 + flat^2 cos^2).
        const int rows = (int) std::ceil (reach[0] * std::sqrt (tiltSin * tiltSin + flat * flat * tiltCos * tiltCos));
        for (int row = std::max (top, y - rows), last = std::min (bottom, y + rows); row <= last; ++row)
        {
            const float v = (float) (row - y);
            const float middle = -Bv * v / (2.0f * A), rest = middle * middle - Cv * v * v / A;
            // The near half is where the ring's short axis points down the screen: u > v cos / sin.
            const float split = v * tiltCos / tiltSin;
            const int nearFrom = (int) std::floor (split) + 1;
            int lo[N], hi[N]; // each copy's pixels on this row, lo..hi (lo > hi: none)
            for (int i = 0; i < N; ++i)
            {
                const float room = rest + reach[i] * reach[i] / A;
                if (reach[i] <= 0.0f || room <= 0.0f) { lo[i] = 1, hi[i] = 0; continue; }
                const float width = std::sqrt (room);
                lo[i] = (int) std::floor (middle - width) + 1;
                hi[i] = (int) std::ceil (middle + width) - 1;
            }
            Ink* pixels = r.px + row * r.w;
            auto paint = [&] (int a, int b, Ink c) {
                if (near) a = std::max (a, nearFrom);
                else b = std::min (b, nearFrom - 1);
                run (pixels, row, x + a, x + b, c, false);
            };
            for (int i = 0; i + 1 < N && lo[i] <= hi[i]; ++i)
            {
                if (lo[i + 1] > hi[i + 1])
                    paint (lo[i], hi[i], inks[i]);
                else
                {
                    paint (lo[i], lo[i + 1] - 1, inks[i]);
                    paint (hi[i + 1] + 1, hi[i], inks[i]);
                }
            }
        }
    }

    // Each grid line lights where it crosses the ring between `inner` and `outer`, kinked `push` pixels outwards (as
    // far as it points away from the moon) while the wave goes through it.
    static void lights (const Raster& r, float inner, float outer, float push, Ink c)
    {
        // The offsets from the centre, along a line `d` from it, that lie in the ring: one run, or one each side.
        auto across = [&] (int d, int centre, int low, int high, auto&& paint) {
            const float d2 = (float) (d * d);
            const int from = half (inner, d2) + 1, to = half (outer, d2);
            if (from > to) return;
            auto clipped = [&] (int a, int b) { if ((a = std::max (a, low)) <= (b = std::min (b, high))) paint (a, b); };
            if (from == 0)
                clipped (centre - to, centre + to);
            else
                clipped (centre - to, centre - from), clipped (centre + from, centre + to);
        };
        for (int gx = gridX; gx <= right; gx += gridStep)
        {
            const int d = gx - cx, x = gx + (int) std::lround (push * (float) d / outer);
            if (x >= left && x <= right)
                across (d, cy, top, bottom, [&] (int a, int b) { for (int y = a; y <= b; ++y) r.px[y * r.w + x] = c; });
        }
        for (int gy = gridY; gy <= bottom; gy += gridStep)
        {
            const int d = gy - cy, y = gy + (int) std::lround (push * (float) d / outer);
            if (y >= top && y <= bottom)
                across (d, cx, left, right, [&] (int a, int b) { std::fill (r.px + y * r.w + a, r.px + y * r.w + b + 1, c); });
        }
    }
};
} // namespace hdui
