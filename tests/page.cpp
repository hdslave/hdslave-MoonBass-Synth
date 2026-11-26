// The Orbital Impact page outside a host, at the default patch.
//   page render DIR LEVEL STEP_MS COUNT [GAP_MS HITS]
//       one hit (or HITS hits GAP_MS apart) at LEVEL, then COUNT frames STEP_MS apart as DIR/frame-NNN.ppm
//   page check
//       a hit's level that is not a number draws as a full hit's, an infinite one as Accent's most, 0 or a negative
//       one nothing; no level or clock writes outside the page; prints the failures and exits 1 if there are any
//   page bench FRAMES [MODE]
//       paints FRAMES frames 16 ms apart for callgrind: MODE hit (the default) a full hit every 400 ms, rest no
//       hits, roll a hit every 100 ms at Accent's level (four shocks at once, each its whole life)
#include "screen.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace moonbass;

// -DPAGE_WITHOUT_LEVEL builds this against a page from before the hit level reached it (0.1.6), for comparisons.
namespace
{
std::atomic<float> params[paramCount];

bool paint (Screen& screen, const melodee_screen_frame_t& c, uint32_t hits, float level)
{
#ifdef PAGE_WITHOUT_LEVEL
    (void) level;
    return screen.paint (c, params, hits);
#else
    return screen.paint (c, params, hits, level);
#endif
}
std::vector<uint32_t> pixels (320 * 218);

melodee_screen_frame_t frame()
{
    melodee_screen_frame_t c {};
    c.pixels = pixels.data();
    c.width = 320, c.height = 218, c.stride = 320, c.area = MELODEE_SCREEN_PAGE;
    for (int k = 0; k < 3; ++k) c.pots[k] = -1;
    return c;
}

void save (const std::string& path)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) { std::perror (path.c_str()); std::exit (1); }
    std::fprintf (f, "P6\n320 218\n255\n");
    for (uint32_t p : pixels)
    {
        const unsigned char rgb[3] = { (unsigned char) (p >> 16), (unsigned char) (p >> 8), (unsigned char) p };
        std::fwrite (rgb, 1, 3, f);
    }
    std::fclose (f);
}
} // namespace

int main (int argc, char** argv)
{
    for (uint32_t i = 0; i < paramCount; ++i) params[i].store (paramTable[i].def);
    Screen screen;
    auto c = frame();
    c.time_ms = 1000, c.redraw_all = 1;
    paint (screen, c, 0, 1.0f);
    c.redraw_all = 0;
    if (argc >= 6 && std::strcmp (argv[1], "render") == 0)
    {
        const std::string dir = argv[2];
        const float level = std::strtof (argv[3], nullptr);
        const int step = std::atoi (argv[4]), count = std::atoi (argv[5]);
        const int gap = argc > 6 ? std::atoi (argv[6]) : 0, hits = argc > 7 ? std::atoi (argv[7]) : 1;
        const uint64_t t0 = 2000;
        uint32_t struck = 0;
        for (int n = 0; n < count; ++n)
        {
            c.time_ms = t0 + (uint64_t) (n * step);
            while ((int) struck < hits && c.time_ms >= t0 + (uint64_t) (struck * gap)) ++struck;
            paint (screen, c, struck, level);
            char name[32];
            std::snprintf (name, sizeof (name), "/frame-%03d.ppm", n);
            save (dir + name);
        }
        return 0;
    }
    if (argc >= 2 && std::strcmp (argv[1], "check") == 0)
    {
        int failures = 0;
        auto check = [&failures] (bool ok, const char* what) {
            if (! ok) std::printf ("FAIL %s\n", what), ++failures;
        };
        // The frames of one hit at `level`, 10 ms apart, from a fresh page.
        auto frames = [] (float level) {
            Screen fresh;
            auto f = frame();
            f.time_ms = 1000, f.redraw_all = 1;
            paint (fresh, f, 0, 1.0f);
            f.redraw_all = 0;
            std::vector<uint32_t> all;
            for (int n = 0; n < 70; ++n)
            {
                f.time_ms = 2000 + (uint64_t) n * 10;
                paint (fresh, f, 1, level);
                all.insert (all.end(), pixels.begin(), pixels.end());
            }
            return all;
        };
        const float nan = std::nanf (""), inf = HUGE_VALF;
        check (frames (nan) == frames (1.0f), "a level that is not a number draws as a full hit");
        check (frames (inf) == frames (2.0f) && frames (1e30f) == frames (2.0f), "a huge level draws as Accent's most");
        check (frames (-inf) == frames (0.0f) && frames (-1.0f) == frames (0.0f), "a negative level draws as 0");
        {
            Screen fresh; // its resting page, 70 times
            auto f = frame();
            f.time_ms = 1000, f.redraw_all = 1;
            paint (fresh, f, 0, 1.0f);
            std::vector<uint32_t> still;
            for (int n = 0; n < 70; ++n) still.insert (still.end(), pixels.begin(), pixels.end());
            check (frames (0.0f) == still, "a hit that makes no sound (level 0) draws nothing");
        }
        check (frames (2.0f) != frames (1.0f) && frames (0.0f) != frames (1.0f), "the level shows");
        // Every level and clock, into a page with guard columns beside it and rows above and below.
        constexpr int stride = 333, rows = 218;
        std::vector<uint32_t> guarded ((rows + 2) * stride, 0x5a5a5a5au);
        Screen wild;
        auto f = frame();
        f.pixels = guarded.data() + stride, f.stride = stride;
        f.time_ms = 0, f.redraw_all = 1;
        paint (wild, f, 0, 1.0f);
        f.redraw_all = 0;
        const float levels[] = { nan, inf, -inf, -1.0f, 0.0f, 1e-30f, 0.5f, 1.0f, 1.4f, 2.0f, 1e30f };
        const uint64_t clocks[] = { 0, 1, 50, 49, 1000, 999, UINT64_MAX / 2, UINT64_MAX - 5, UINT64_MAX, 3, 4000 };
        uint32_t hits = 0;
        for (uint64_t start : clocks)
            for (float level : levels)
                for (int n = 0; n < 40; ++n)
                {
                    f.time_ms = start + (uint64_t) n * 7;
                    paint (wild, f, n % 9 == 0 ? ++hits : hits, level);
                }
        bool kept = true;
        for (int x = 0; x < stride; ++x) kept &= guarded[x] == 0x5a5a5a5au && guarded[(rows + 1) * stride + x] == 0x5a5a5a5au;
        for (int y = 1; y <= rows; ++y)
            for (int x = 320; x < stride; ++x) kept &= guarded[y * stride + x] == 0x5a5a5a5au;
        check (kept, "nothing is written outside the page");
        std::printf ("page check: %d failed\n", failures);
        return failures != 0;
    }
    if (argc >= 3 && std::strcmp (argv[1], "bench") == 0)
    {
        const int frames = std::atoi (argv[2]);
        const std::string mode = argc > 3 ? argv[3] : "hit";
        const int gap = mode == "roll" ? 100 : 400;
        const float level = mode == "roll" ? 2.0f : 1.0f;
        uint32_t hits = 0, sum = 0;
        for (int n = 0; n < frames; ++n)
        {
            c.time_ms = 2000 + (uint64_t) n * 16;
            if (mode != "rest") hits = (uint32_t) (n * 16 / gap) + 1;
            sum += paint (screen, c, hits, level) ? 1u : 0u;
        }
        std::printf ("%s: %u of %d frames drew, checksum %08x\n", mode.c_str(), sum, frames, pixels[60 * 320 + 40]);
        return 0;
    }
    std::fprintf (stderr, "usage: page render DIR LEVEL STEP_MS COUNT [GAP_MS HITS] | page bench FRAMES [hit|rest|roll]\n");
    return 2;
}
