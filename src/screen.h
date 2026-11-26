// Orbital Impact. Drawing and navigation are confined to Melodee's screen thread.
#pragma once

#include "melodee_screen.h"
#include "params.h"
#include "ui/orbital_art.h"
#include "ui/raster.h"
#include "ui/shockwave.h"
#include <array>
#include <atomic>
#include <cctype>

namespace moonbass
{
// The page's inks, besides the art's own palette (art::palette: the moon's orange ramp, its greys and cream).
namespace inks
{
using hdui::Ink;
constexpr Ink orange = 0xffff7131, lilac = 0xffbc99f5, paper = 0xfffff4db;
constexpr Ink muted = 0xffd0b4e7, grid = 0xff3d3046, panel = 0xff241d32, globeGrid = 0xff352c56;

// The moon as a hit lights it (`steps` 2) and as it cools (1): its orange climbs that many steps of the art's ramp
// (past the top: cream), its greys as many towards cream, and the dark inside takes on the page's lilac.
constexpr std::array<Ink, 16> flash (int steps)
{
    const Ink* a = art::palette;
    std::array<Ink, 16> ink {};
    ink[1] = a[1];
    ink[2] = hdui::mix (a[2], lilac, steps, 8);
    ink[3] = hdui::mix (a[3], lilac, steps, 5);
    for (int i = 4; i <= 10; ++i) ink[i] = a[i + steps <= 10 ? i + steps : 15];
    for (int i = 11; i <= 15; ++i) ink[i] = a[std::min (i + steps, 15)];
    return ink;
}

// The shockwave's ramps: the ring's cream edge and its orange band step down the art's ramp; the violet, the grid's
// lights and the wake are the page's lilac mixed towards the background or the globe's grid.
constexpr hdui::ShockInks shock()
{
    const Ink* a = art::palette;
    hdui::ShockInks s {};
    const Ink edge[6] = { a[15], a[10], a[9], a[8], a[7], a[6] };
    const Ink glow[6] = { paper, muted, lilac, hdui::mix (lilac, globeGrid, 1, 3), hdui::mix (lilac, globeGrid, 2, 3), globeGrid };
    for (int k = 0; k < 6; ++k)
    {
        s.edge[k] = edge[k];
        s.body[k] = a[8 - k];
        s.violet[k] = hdui::mix (lilac, hdui::background, k, 7);
        s.glow[k] = glow[k];
        s.wake[k] = hdui::mix (lilac, hdui::background, 14 + k, 20);
    }
    return s;
}
inline constexpr hdui::ShockInks shockInks = shock();
} // namespace inks

// The moon's runs of non-clear pixels, row by row, found from the art when compiling: at most two a row (the sphere
// and an end of its ring), 7,281 of its 18,408 pixels. The moon is drawn from these alone.
struct MoonRuns
{
    static constexpr int w = 156, h = 118, most = 2;
    uint8_t count[h] {}, from[h][most] {}, to[h][most] {};
    bool fits = true;
};
constexpr MoonRuns findMoonRuns()
{
    MoonRuns runs {};
    for (int y = 0; y < MoonRuns::h; ++y)
    {
        auto clear = [y] (int x) { return ((art::moon[y * MoonRuns::w / 2 + x / 2] >> ((x & 1) ? 0 : 4)) & 15) == 0; };
        for (int x = 0; x < MoonRuns::w;)
        {
            if (clear (x)) { ++x; continue; }
            const int start = x;
            while (x < MoonRuns::w && ! clear (x)) ++x;
            const int n = runs.count[y]++;
            if (n < MoonRuns::most) runs.from[y][n] = (uint8_t) start, runs.to[y][n] = (uint8_t) (x - 1);
            else runs.fits = false;
        }
    }
    return runs;
}
inline constexpr MoonRuns moonRuns = findMoonRuns();

// The moon's pixels a byte each (its palette index), unpacked from the art's nibbles when compiling.
struct MoonPixels
{
    uint8_t at[MoonRuns::h][MoonRuns::w] {};
};
constexpr MoonPixels unpackMoon()
{
    MoonPixels m {};
    for (int y = 0; y < MoonRuns::h; ++y)
        for (int x = 0; x < MoonRuns::w; ++x)
            m.at[y][x] = (uint8_t) ((art::moon[y * MoonRuns::w / 2 + x / 2] >> ((x & 1) ? 0 : 4)) & 15);
    return m;
}
inline constexpr MoonPixels moonPixels = unpackMoon();
static_assert (moonRuns.fits, "a moon row with more runs than MoonRuns::most: raise it");

class Screen
{
    using Ink = hdui::Ink;
    using Raster = hdui::Raster;
    static constexpr Ink orange = inks::orange, lilac = inks::lilac, paper = inks::paper;
    static constexpr Ink muted = inks::muted, grid = inks::grid, panel = inks::panel;
    static constexpr int pages = paramCount / 3;
    static_assert (paramCount % 3 == 0, "a page holds three parameters: one added alone would not be reachable");
    hdui::Picture picture;
    std::array<float, ParamId::count> values {};
    int page = 0, lastPage = -1;
    int16_t lastKnobs[3] { -1, -1, -1 };
    bool lastKnobsLive[3] {};
    bool lastShift = false, moving = false;
    uint32_t seenHits = 0;
    uint64_t hitAt = 0;
    int kick = 0;      // the newest hit's squash, in pixels each side
    bool soft = false; // the newest hit was soft: it warms the moon but never makes it hot
    hdui::Shockwave shock { inks::shockInks };
    static constexpr std::array<Ink, 16> hot = inks::flash (2), warm = inks::flash (1);

    static void sprite (Raster& r, const uint8_t* data, int stride, int srcX, int w, int h,
                        int x, int y, int destW, int destH)
    {
        for (int j = 0; j < destH; ++j)
        {
            const uint8_t* row = data + (j * h / destH) * (stride / 2);
            int source = 0;
            for (int i = 0; i < destW; ++i, source += w)
            {
                const int sx = srcX + (destW == w ? i : source / destW);
                const int colour = (row[sx / 2] >> ((sx & 1) ? 0 : 4)) & 15;
                if (colour) r.dot (x + i, y + j, art::palette[colour]);
            }
        }
    }

    // The moon at destW x destH, clipped to the raster: the pixels sprite() would draw (each takes the source pixel
    // floor (i * 156 / destW) along, floor (j * 118 / destH) down), but only along its runs, so the clear pixels cost
    // nothing and nothing is divided a pixel. Each column's source is stepped by a remainder once a call.
    static void moon (Raster& r, int x, int y, int destW, int destH, const Ink* ink)
    {
        constexpr int w = MoonRuns::w, h = MoonRuns::h, widest = 256;
        const int firstI = std::max (0, -x), endI = std::min (destW, r.w - x);
        if (firstI >= endI || destW > widest || destW <= 0) return;
        uint8_t source[widest];
        for (int i = firstI, column = firstI * w / destW, rest = firstI * w % destW; i < endI; ++i)
        {
            source[i] = (uint8_t) column;
            for (rest += w; rest >= destW; rest -= destW) ++column;
        }
        for (int j = std::max (0, -y); j < destH && y + j < r.h; ++j)
        {
            const int sy = j * h / destH;
            const uint8_t* row = moonPixels.at[sy];
            Ink* out = r.px + (y + j) * r.w + x;
            for (int k = 0; k < moonRuns.count[sy]; ++k)
            {
                // The columns whose source lies in the run: floor (i * w / destW) from `from` to `to`.
                const int a = std::max (firstI, (moonRuns.from[sy][k] * destW + w - 1) / w);
                const int b = std::min (endI - 1, ((moonRuns.to[sy][k] + 1) * destW + w - 1) / w - 1);
                int i = a;
                if (destW == w)
                {
                    for (; i + 3 <= b; i += 4)
                    {
                        const Ink p0 = ink[row[i]], p1 = ink[row[i + 1]], p2 = ink[row[i + 2]], p3 = ink[row[i + 3]];
                        out[i] = p0, out[i + 1] = p1, out[i + 2] = p2, out[i + 3] = p3;
                    }
                    for (; i <= b; ++i) out[i] = ink[row[i]];
                }
                else
                {
                    for (; i + 3 <= b; i += 4)
                    {
                        const Ink p0 = ink[row[source[i]]], p1 = ink[row[source[i + 1]]], p2 = ink[row[source[i + 2]]], p3 = ink[row[source[i + 3]]];
                        out[i] = p0, out[i + 1] = p1, out[i + 2] = p2, out[i + 3] = p3;
                    }
                    for (; i <= b; ++i) out[i] = ink[row[source[i]]];
                }
            }
        }
    }

    static void upper (const char* s, char (&out)[32])
    {
        int n = 0;
        while (*s && n < 31) out[n++] = (char) std::toupper ((unsigned char) *s++);
        out[n] = 0;
    }

    void header (Raster& r) const
    {
        sprite (r, art::logo, 232, 0, 232, 40, 0, 0, 232, 40);
        char text[32];
        upper (paramTable[page * 3].module, text);
        r.right (312, 10, text, paper);
        std::snprintf (text, sizeof (text), "%02d / %02d", page + 1, pages);
        r.right (312, 26, text, lilac);
        r.box (5, 43, 310, 1, 0xff69536e);
    }

    void globe (Raster& r, uint64_t now, int pulse, const Ink* ink) const
    {
        shock.under (r, now);
        // The vertical lines a row at a time (as box() would draw them, a pixel wide over rows 5-114).
        for (int y = 5; y < 115; ++y)
            for (Ink* p = r.px + y * r.w, *end = p + 159, *x = p + 6; x < end; x += 15) *x = inks::globeGrid;
        for (int y = 7; y < 115; y += 15) r.box (5, y, 154, 1, inks::globeGrid);
        shock.behind (r, now);
        // A struck shell widens and flattens, then lands back on its native pixels.
        moon (r, 5 - pulse, 2 + pulse, 156 + 2 * pulse, 118 - 2 * pulse, ink);
        shock.front (r, now);
    }

    // How long a hit's shock rings: longer for a longer kick (the Decay it was struck with), within 240-560 ms
    // (about 310 ms at the default Decay).
    float shockMs() const
    {
        const float decayMs = 1000.0f * decaySeconds (withCv (ParamId::decay, CvDest::decay));
        return std::clamp (200.0f + 0.3f * decayMs, 240.0f, 560.0f);
    }

    // The engine's own CV sum (params.h), so the graphs follow the same destination arithmetic as the sound.
    float withCv (uint32_t id, int dest) const
    {
        return std::clamp (values[id] + cvSum (values.data(), dest), 0.0f, 1.0f);
    }

    // Envelopes of the patch at its base pitch. The pitch graph shows the excursion above
    // the resting note; the amp graph includes the gate hold and compression threshold.
    void envelope (Raster& r, bool pitch) const
    {
        const Ink colour = pitch ? orange : lilac;
        const float tune = withCv (ParamId::tune, CvDest::tune) * tuneOctavesMax;
        const float hz = pitchHz (withCv (ParamId::pitch, CvDest::pitch));
        const float decayMs = 1000.0f * decaySeconds (withCv (ParamId::decay, CvDest::decay));
        const float gate = gateMs (values[ParamId::gateTime]);
        const float cut = 0.9f * withCv (ParamId::compression, CvDest::compression);
        const int span = pitch ? 100 : (decayMs + gate <= 400 ? 400 : (decayMs + gate <= 1000 ? 1000 : 2100));
        const float rangeHz = std::max (150.0f, hz * (std::exp2 (tune) - 1.0f));

        r.text (7, 6, pitch ? "PITCH" : "AMP", colour);
        char label[20];
        std::snprintf (label, sizeof (label), "0-%d ms", span);
        r.right (148, 6, label, 0xffd9cce5);
        constexpr int x0 = 7, y0 = 20, w = 139, h = 32;
        for (int k = 0; k <= 4; ++k) r.box (x0 + w * k / 4, y0, 1, h + 1, grid);
        for (int k = 0; k <= 2; ++k) r.box (x0, y0 + h * k / 2, w + 1, 1, grid);
        int previous = 0;
        for (int i = 0; i <= w; ++i)
        {
            const float ms = (float) (i * span) / w;
            float level;
            if (pitch)
                level = hz * (std::exp2 (tune * std::exp (-ms / (1000.0f * tuneDecaySeconds))) - 1.0f) / rangeHz;
            else
                level = std::max (0.0f, (std::exp (-6.90775528f * std::max (ms - gate, 0.0f) / decayMs) - cut) / (1.0f - cut));
            const int y = y0 + h - 1 - (int) std::lround (level * (h - 2));
            if (i) r.line (x0 + i - 1, previous, x0 + i, y, colour);
            else r.box (x0 - 1, y - 1, 3, 3, colour);
            previous = y;
        }
    }

    static void readout (Raster& r, int x, const char* text)
    {
        const char* glyphs = "+-.0123456789";
        const size_t n = std::strspn (text, glyphs);
        if (n == 0 || (text[n] && text[n] != ' '))
        {
            r.text (x, 28, text, paper);
            return;
        }
        for (size_t i = 0; i < n; ++i)
        {
            const int index = (int) (std::strchr (glyphs, text[i]) - glyphs);
            sprite (r, art::digits, 208, 16 * index, 16, 24, x, 18, 16, 24);
            x += text[i] == '.' ? 6 : (text[i] == '-' ? 8 : 12);
        }
        if (text[n]) r.text (x + 3, 31, text + n + 1, muted);
    }

    void footer (Raster& r, const melodee_screen_frame_t& c) const
    {
        r.box (0, 0, 320, 51, panel);
        r.box (0, 0, 320, 1, 0xff66507d);
        if (c.shift)
        {
            r.text (8, 9, "-/+  Page", paper);
            r.text (119, 9, "Push  Play kick", paper);
            r.text (8, 28, "Wheel  K1", muted);
            r.text (119, 28, "Shift+wheel  Fine", muted);
            return;
        }
        for (int k = 0; k < 3; ++k)
        {
            const int x = 8 + k * 106;
            const auto& p = paramTable[page * 3 + k];
            if (k) r.box (x - 7, 7, 1, 36, 0xff55415d);
            char key[] = { 'K', (char) ('1' + k), 0 };
            r.text (x, 7, key, muted);
            const char* name = p.name;
            const size_t prefix = std::strlen (p.module);
            if (std::strncmp (name, p.module, prefix) == 0 && name[prefix] == ' ') name += prefix + 1;
            if (p.id == ParamId::metalNoise1) name = "Metal 1";
            if (p.id == ParamId::metalNoise2) name = "Metal 2";
            if (p.id == ParamId::noise2Mask) name = "Mask 2";
            char label[32], text[32];
            upper (name, label);
            r.text (x + 18, 7, label, paper);
            if (p.kind == Kind::button)
            {
                r.text (x, 27, "Push to play", paper);
                continue;
            }
            const float v = values[p.id];
            formatParam (p, v, text, sizeof (text));
            readout (r, x, text);
            const int at = (int) std::lround ((v - p.min) / (p.max - p.min) * 87.0f);
            r.box (x, 42, 88, 2, 0xff49364f);
            r.box (x, 42, at + 1, 2, k == 2 ? lilac : orange);
            if (!c.pots_live[k] && c.pots[k] >= 0)
                r.frame (x + c.pots[k] * 87 / 127 - 2, 45, 5, 3, paper);
        }
    }

public:
    int firstRow() const { return page * 3; }
    void navigate (int delta)
    {
        page = std::clamp (page + std::clamp (delta, -pages, pages), 0, pages - 1);
    }
    void knobs (uint32_t ids[3]) const
    {
        for (int k = 0; k < 3; ++k)
        {
            const auto& p = paramTable[firstRow() + k];
            ids[k] = p.kind == Kind::button ? MELODEE_SCREEN_NO_CONTROL : p.id;
        }
    }
    bool animating() const { return moving; }

    // `hits` counts the engine's hits; `level` is the newest one's strength (1 a full hit, up to 2 with Accent; 0 a
    // hit that makes no sound, which draws nothing).
    bool paint (const melodee_screen_frame_t& c, const std::atomic<float>* params, uint32_t hits, float level = 1.0f)
    {
        const bool whole = c.redraw_all != 0 || lastPage < 0;
        const bool moved = lastPage != page;
        bool pitchChanged = false, ampChanged = false, cardsChanged = false, knobsMoved = false;
        for (uint32_t i = 0; i < paramCount; ++i)
        {
            const float v = params[i].load (std::memory_order_relaxed);
            const uint32_t id = paramTable[i].id;
            if (values[id] == v) continue;
            values[id] = v;
            const bool cv = id >= ParamId::cv1Dest && id <= ParamId::cv4In;
            pitchChanged |= cv || id == ParamId::pitch || id == ParamId::tune;
            ampChanged |= cv || id == ParamId::gateTime || id == ParamId::decay || id == ParamId::compression;
            cardsChanged |= (int) (i / 3) == page;
        }
        for (int k = 0; k < 3; ++k)
        {
            const bool liveNow = c.pots_live[k] != 0;
            knobsMoved |= c.pots[k] != lastKnobs[k] || liveNow != lastKnobsLive[k];
            lastKnobs[k] = c.pots[k];
            lastKnobsLive[k] = liveNow;
        }
        // A hit while the page was away is counted, not shown: a whole paint opens without a pulse.
        if (seenHits != hits)
        {
            seenHits = hits;
            // Within 0-2 (Accent's most); not a number, a full hit.
            level = level >= 0.0f ? std::min (level, 2.0f) : (level < 0.0f ? 0.0f : 1.0f);
            if (! whole && level > 0.0f)
            {
                hitAt = c.time_ms;
                kick = std::clamp ((int) std::lround (6.0f * level), 2, 7);
                soft = level < 0.45f;
                shock.strike (c.time_ms, level, shockMs(), hits);
            }
        }
        // The squash steps back a pixel every 30 ms; the flash lasts 110 ms (hot for 50, unless soft); the shock as
        // long as shockMs().
        const uint64_t age = c.time_ms - hitAt;
        const bool struck = kick > 0 && c.time_ms >= hitAt;
        const int pulse = struck && age < 30u * (uint64_t) kick ? kick - (int) (age / 30) : 0;
        const Ink* ink = struck && age < 50 && ! soft ? hot.data() : (struck && age < 110 ? warm.data() : art::palette);
        const bool live = pulse != 0 || ink != art::palette || shock.live (c.time_ms);
        bool drew = false;
        if (whole || moved)
            drew |= picture.region<320,45> (c.pixels, c.stride, whole, 0, 0, [&](Raster& r) { header (r); });
        // Every frame while it moves, and once more as it comes to rest.
        if (whole || live || moving)
            drew |= picture.region<164,122> (c.pixels, c.stride, whole, 0, 45, [&](Raster& r) { globe (r, c.time_ms, pulse, ink); });
        moving = live;
        if (whole || pitchChanged)
            drew |= picture.region<156,61> (c.pixels, c.stride, whole, 164, 45, [&](Raster& r) { envelope (r, true); });
        if (whole || ampChanged)
            drew |= picture.region<156,61> (c.pixels, c.stride, whole, 164, 106, [&](Raster& r) { envelope (r, false); });
        const bool shiftNow = c.shift != 0;
        if (whole || moved || cardsChanged || knobsMoved || shiftNow != lastShift)
            drew |= picture.region<320,51> (c.pixels, c.stride, whole, 0, 167, [&](Raster& r) { footer (r, c); });
        lastPage = page;
        lastShift = shiftNow;
        return drew;
    }
};
} // namespace moonbass
