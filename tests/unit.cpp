// Engine-level checks, under the shipped OPT flags: today, the metal noise generator by itself — its
// silence, its mask arithmetic and that a restart gives the same frames again. The sound it makes inside a
// kick is covered through the CLAP API in host.cpp and to the bit in fingerprint.cpp.
#include "../src/engine.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace moonbass;

void testMetalNoise()
{
    MetalNoise m;
    m.prepare (48000.0f);
    // Both generators off: exactly nothing, frame after frame.
    for (int i = 0; i < 1000; ++i)
        CHECK (m.next (0, 0) == 0.0f);
    // The counter bits Noise 2 reads: a number with no bit inside the mask selects nothing (silent), the
    // mask cuts a nine-bit number up to M09, and from M10 the number moves into the mask's top bits.
    CHECK (MetalNoise::selection (256, 8) == 0u);
    CHECK (MetalNoise::selection (499, 8) == (499u & 0xffu));
    CHECK (MetalNoise::selection (1, 16) == (1u << 7));
    CHECK (MetalNoise::selection (499, 16) == (499u << 7));
    // A restart replays the same frames, to the bit, and every frame is a finite, bounded number.
    auto run = [&] (int n1, uint32_t sel) {
        m.restart();
        std::vector<float> x;
        for (int i = 0; i < 8192; ++i)
            x.push_back (m.next (n1, sel));
        return x;
    };
    for (auto [n1, sel] : { std::pair { 1, 0u }, std::pair { 2, 0u }, std::pair { 337, MetalNoise::selection (77, 12) }, std::pair { 499, MetalNoise::selection (499, 16) } })
    {
        const auto a = run (n1, sel), b = run (n1, sel);
        CHECK (a == b);
        bool varied = false;
        for (float v : a)
        {
            CHECK (std::isfinite (v) && std::fabs (v) <= 4.0f);
            varied |= v != a[0];
        }
        CHECK (varied);
    }
}
} // namespace

int main()
{
    testMetalNoise();
    std::printf ("%d unit checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
