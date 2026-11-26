#pragma once

#include "hdslave_tiles.h"
#include "screen.h"

namespace moonbass
{

struct TilesAccess
{
    // 6 tiles (3x2 grid) covering Globe + Pitch/Amp Envelopes (Tile 0) and all 14 pages (42 parameters, Tiles 1..5),
    // with the K1-K3 footer panel removed from Tile 0 (content_h = 167).
    static constexpr int kTileCount = 6;
    static constexpr int kTileBanks[kTileCount][3] = {
        { -1, -1, -1 }, // Tile 0: Orbital Moon Globe + Pitch & Amp Envelopes (no K1-K3 footer)
        {  0,  1,  2 }, // Tile 1: Pages 1..3 (Tune/Pitch/Decay, Harmonics/Pulse/Noise, Attack/EQ/Volume)
        {  3,  4,  5 }, // Tile 2: Pages 4..6 (Gate/Compress/Play, Metal 1/2/Mask, LFO 1 Wave/Speed/Int)
        {  6,  7,  8 }, // Tile 3: Pages 7..9 (LFO 1 Dest/Mode/Clock, LFO 2 Wave/Speed/Int, LFO 2 Dest/Mode/Clock)
        {  9, 10, 11 }, // Tile 4: Pages 10..12 (Split/Pitch/Dynamics, CV 1, CV 2)
        { 12, 13, -1 }  // Tile 5: Pages 13..14 (CV 3, CV 4) + Sub-Bass Trigger Pad
    };

    static uint32_t CLAP_ABI count (const clap_plugin_t*, int* out_cols, int* out_rows)
    {
        if (out_cols) *out_cols = 3;
        if (out_rows) *out_rows = 2;
        return kTileCount;
    }

    static const char* CLAP_ABI title (const clap_plugin_t*, uint32_t tile)
    {
        static constexpr const char* kTitles[kTileCount] = {
            "01 // ORBITAL IMPACT (GLOBE & ENVS)",
            "02 // KICK CORE / VOICE / EQ",
            "03 // GATE / METAL NOISE / LFO 1",
            "04 // LFO 1 ROUTING & LFO 2",
            "05 // KEY / VELOCITY / CV 1 & 2",
            "06 // CV 3 & 4 / KICK TRIGGER"
        };
        return tile < kTileCount ? kTitles[tile] : "";
    }

    static void drawDeck (Screen& s, const melodee_screen_frame_t& c, int bankPage, int y0, int h)
    {
        hdui::Raster r { c.pixels + y0 * c.stride, 320, h };
        r.box (0, 0, 320, h, Screen::panel);
        r.box (0, 0, 320, 1, 0xff66507d);
        if (bankPage < 0)
        {
            // Interactive Kick Trigger Pad & Sub-Bass X/Y Monitor in the bottom slot of Tile 6
            r.frame (8, 5, 144, h - 10, Screen::orange);
            r.text (22, (h - 8) / 2, "TRIGGER KICK (C1)", Screen::paper);
            r.frame (160, 5, 152, h - 10, Screen::lilac);
            r.text (170, (h - 8) / 2, "X/Y: PITCH / DECAY", Screen::muted);
            return;
        }
        for (int k = 0; k < 3; ++k)
        {
            const int x = 8 + k * 106;
            const auto& p = paramTable[bankPage * 3 + k];
            if (k) r.box (x - 7, 4, 1, h - 8, 0xff55415d);
            const char* name = p.name;
            const size_t prefix = std::strlen (p.module);
            if (std::strncmp (name, p.module, prefix) == 0 && name[prefix] == ' ') name += prefix + 1;
            if (p.id == ParamId::metalNoise1) name = "Metal 1";
            if (p.id == ParamId::metalNoise2) name = "Metal 2";
            if (p.id == ParamId::noise2Mask) name = "Mask 2";
            if (p.id == ParamId::compression) name = "Compress";
            char label[32], text[32];
            Screen::upper (name, label);
            r.text (x, 3, label, Screen::paper);
            if (p.kind == Kind::button)
            {
                r.frame (x, 14, 92, 22, Screen::orange);
                r.text (x + 4, 20, "PUSH TO PLAY", Screen::paper);
                continue;
            }
            const float v = s.values[p.id];
            formatParam (p, v, text, sizeof (text));
            // Compact readout in 41px deck height
            const char* glyphs = "+-.0123456789";
            const size_t n = std::strspn (text, glyphs);
            if (n == 0 || (text[n] && text[n] != ' '))
            {
                r.text (x, 20, text, Screen::paper);
            }
            else
            {
                int dx = x;
                for (size_t i = 0; i < n; ++i)
                {
                    const int index = (int) (std::strchr (glyphs, text[i]) - glyphs);
                    Screen::sprite (r, art::digits, 208, 16 * index, 16, 24, dx, 13, 14, 21);
                    dx += text[i] == '.' ? 5 : (text[i] == '-' ? 7 : 11);
                }
                if (text[n]) r.text (dx + 2, 24, text + n + 1, Screen::muted);
            }
            const int at = (int) std::lround ((v - p.min) / (p.max - p.min) * 87.0f);
            r.box (x, h - 4, 88, 2, 0xff49364f);
            r.box (x, h - 4, at + 1, 2, k == 2 ? Screen::lilac : Screen::orange);
        }
    }

    static bool CLAP_ABI paint (const clap_plugin_t* plugin, uint32_t tile, const melodee_screen_frame_t* frame, clap_id knobs_out[3])
    {
        if (tile >= kTileCount || !frame || !frame->pixels) return false;
        auto& p = of (plugin);

        if (tile == 0)
        {
            const int savedPage = p.screen.page;
            p.screen.page = 0;
            p.screen.lastPage = -1;
            melodee_screen_frame_t c = *frame;
            c.redraw_all = 1;
            const uint32_t hits = p.hits.load (std::memory_order_acquire);
            const bool drew = p.screen.paint (c, p.values, hits, p.hitStrength.load (std::memory_order_relaxed));
            if (knobs_out)
                p.screen.knobs (knobs_out);
            p.screen.page = savedPage;
            return drew;
        }

        // Sync live values
        for (uint32_t i = 0; i < paramCount; ++i)
            p.screen.values[paramTable[i].id] = p.values[i].load (std::memory_order_relaxed);

        // Draw MoonBass header (0..43) + 3 stacked MoonBass big-digit decks in y = 44..166 (41px each)
        hdui::Raster topR { frame->pixels, 320, 44 };
        topR.box (0, 0, 320, 44, 0xff161122);
        const int savedPage = p.screen.page;
        p.screen.page = kTileBanks[tile][0];
        p.screen.header (topR);
        p.screen.page = savedPage;

        for (int b = 0; b < 3; ++b)
        {
            const int y0 = 44 + b * 41;
            const int h = 41;
            drawDeck (p.screen, *frame, kTileBanks[tile][b], y0, h);
        }

        if (knobs_out)
        {
            const int bank = std::max (0, kTileBanks[tile][0]);
            for (int k = 0; k < 3; ++k)
            {
                const auto& par = paramTable[bank * 3 + k];
                knobs_out[k] = par.kind == Kind::button ? CLAP_INVALID_ID : par.id;
            }
        }
        return true;
    }

    static bool CLAP_ABI hit_test (const clap_plugin_t*, uint32_t tile, int x, int y, hdslave_tile_hotspot_t* out_spot)
    {
        if (!out_spot || tile >= kTileCount) return false;
        *out_spot = {};
        out_spot->param_id = CLAP_INVALID_ID;
        out_spot->param_id_y = CLAP_INVALID_ID;

        if (tile == 0)
        {
            if (y >= 45 && y < 167)
            {
                if (x < 164)
                {
                    // Moon Globe: click plays kick!
                    out_spot->kind = HDSLAVE_HOTSPOT_BUTTON;
                    out_spot->param_id = ParamId::play;
                    out_spot->x = 4;
                    out_spot->y = 45;
                    out_spot->w = 158;
                    out_spot->h = 120;
                    return true;
                }
                if (y < 106)
                {
                    // Pitch envelope graph: 2D Pitch & Tune
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::pitch;
                    out_spot->param_id_y = ParamId::tune;
                    out_spot->x = 164;
                    out_spot->y = 45;
                    out_spot->w = 154;
                    out_spot->h = 60;
                    out_spot->invert_y = true;
                    return true;
                }
                // Amp envelope graph: 2D Decay & Compression
                out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = ParamId::decay;
                out_spot->param_id_y = ParamId::compression;
                out_spot->x = 164;
                out_spot->y = 106;
                out_spot->w = 154;
                out_spot->h = 60;
                out_spot->invert_y = true;
                return true;
            }
            return false;
        }

        // Tiles 1..5: 3 stacked decks of 3 parameters each (y: 44..166, 41px each)
        if (y >= 44 && y < 167)
        {
            const int b = std::clamp ((y - 44) / 41, 0, 2);
            const int bankPage = kTileBanks[tile][b];
            if (bankPage < 0)
            {
                if (x < 156)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_BUTTON;
                    out_spot->param_id = ParamId::play;
                    out_spot->x = 8; out_spot->y = (int16_t) (44 + b * 41 + 4);
                    out_spot->w = 144; out_spot->h = 33;
                    return true;
                }
                out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = ParamId::pitch;
                out_spot->param_id_y = ParamId::decay;
                out_spot->x = 160; out_spot->y = (int16_t) (44 + b * 41 + 4);
                out_spot->w = 152; out_spot->h = 33;
                out_spot->invert_y = true;
                return true;
            }
            const int k = std::clamp (x / 107, 0, 2);
            const auto& par = paramTable[bankPage * 3 + k];
            out_spot->kind = par.kind == Kind::button ? HDSLAVE_HOTSPOT_BUTTON : HDSLAVE_HOTSPOT_PARAM;
            out_spot->param_id = par.id;
            out_spot->x = (int16_t) (k * 107);
            out_spot->y = (int16_t) (44 + b * 41);
            out_spot->w = 106;
            out_spot->h = 40;
            return true;
        }
        return false;
    }

    static void CLAP_ABI focus (const clap_plugin_t* plugin, uint32_t tile, int, int y, bool)
    {
        if (tile >= kTileCount) return;
        auto& p = of (plugin);
        if (tile == 0)
            p.screen.page = 0;
        else if (y >= 44 && y < 167)
        {
            const int bp = kTileBanks[tile][std::clamp ((y - 44) / 41, 0, 2)];
            if (bp >= 0) p.screen.page = bp;
        }
    }

    static void CLAP_ABI action (const clap_plugin_t*, uint32_t, int32_t, int32_t, bool) {}
};

const clap_plugin_hdslave_tiles_t tilesExtension {
    TilesAccess::count,
    TilesAccess::title,
    TilesAccess::paint,
    TilesAccess::hit_test,
    TilesAccess::focus,
    TilesAccess::action,
    167,
};

} // namespace moonbass
