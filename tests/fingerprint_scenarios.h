// MoonBass's fingerprint scenarios (tests/fingerprint.cpp).
#pragma once

namespace
{
// The parameters of MoonBass 0.1, in its table's order: the random scenarios draw for these. When parameters are
// added, keep this list and add another for scenarios that use them, so these hashes still hold.
const std::vector<std::string> order01 { "Tune", "Pitch", "Decay", "Harmonics", "Pulse", "Noise", "Attack", "EQ", "Volume", "Gate Time",
    "Compression", "Play", "Metal Noise 1", "Metal Noise 2", "Noise 2 Mask", "LFO 1 Wave", "LFO 1 Speed", "LFO 1 Int", "LFO 1 Dest",
    "LFO 1 Mode", "LFO 1 Clock", "LFO 2 Wave", "LFO 2 Speed", "LFO 2 Int", "LFO 2 Dest", "LFO 2 Mode", "LFO 2 Clock", "Split Mode",
    "Pitch Mode", "Dynamics", "CV 1 Dest", "CV 1 Amt", "CV 1 In", "CV 2 Dest", "CV 2 Amt", "CV 2 In", "CV 3 Dest", "CV 3 Amt",
    "CV 3 In", "CV 4 Dest", "CV 4 Amt", "CV 4 In" };

// A note every `step` seconds over `seconds`, at `rate`.
std::vector<Event> hitsEvery (double seconds, double step, double rate, int key = 36, double velocity = 0.9)
{
    std::vector<Event> out;
    for (double t = 0.0; t < seconds; t += step)
        out.push_back ({ (uint64_t) std::llround (t * rate), Event::noteOn, "", (double) key, velocity });
    return out;
}

Event note (uint64_t frame, int key, double velocity = 0.9) { return { frame, Event::noteOn, "", (double) key, velocity }; }
Event cc (uint64_t frame, int number, int value) { return { frame, Event::midi, "", (double) 0xb0, (double) (number * 256 + value) }; }

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    for (const char* key : { "default", "gut-punch", "soft-boom", "sub-drop", "clap-kick", "metal-kick", "wobble-tail", "zap", "bass-line", "pad-tunings" })
    {
        Scenario s;
        s.name = std::string ("preset-") + key;
        s.preset = key;
        s.seconds = 4.0;
        if (s.preset == "bass-line")
            for (int i = 0; i < 16; ++i)
                s.events.push_back (note ((uint64_t) i * 12000, 36 + (i * 7) % 20, 0.5 + 0.5 * (i % 3) / 2.0));
        else if (s.preset == "pad-tunings")
            for (int i = 0; i < 16; ++i)
                s.events.push_back (note ((uint64_t) i * 12000, 36 + i % 8));
        else
            s.events = hitsEvery (4.0, 0.5, 48000.0);
        list.push_back (s);
    }
    // The bench's patches.
    Scenario worst;
    worst.name = "bench-worst";
    worst.values = { { "Gate Time", 1.0 }, { "Metal Noise 1", 499 }, { "Metal Noise 2", 333 }, { "Noise 2 Mask", 16 }, { "Noise", 0.8 }, { "Decay", 1.0 },
                     { "Harmonics", 0.5 }, { "Compression", 0.2 }, { "LFO 1 Int", 0.3 }, { "LFO 1 Dest", 1 }, { "LFO 1 Mode", 0 }, { "LFO 2 Int", 0.2 },
                     { "LFO 2 Clock", 8 }, { "LFO 2 Wave", 9 } };
    worst.seconds = 3.0;
    for (uint64_t f = 0; f < 3 * 48000; f += 640)
        worst.events.push_back (note (f, 36));
    list.push_back (worst);
    Scenario typical;
    typical.name = "bench-typical";
    typical.seconds = 4.0;
    for (uint64_t f = 0; f < 4 * 48000; f += 188 * 128) // as the bench hits: every 188 blocks
        typical.events.push_back (note (f, 36));
    list.push_back (typical);
    // Random settings with notes across the keyboard, Play, MIDI controllers and knobs moving while it plays.
    for (uint32_t r = 0; r < 24; ++r)
    {
        Scenario s;
        s.name = "random-" + std::to_string (r);
        s.values = randomValues (2000 + r, { "Play" }, order01);
        s.seconds = 3.0, s.bpm = 140.0;
        for (int i = 0; i < 12; ++i)
            s.events.push_back (note ((uint64_t) i * 12000 + (uint64_t) (r * 37), 30 + (i * 11 + (int) r) % 60, 0.3 + 0.7 * ((i + r) % 4) / 3.0));
        s.events.push_back ({ 52800, Event::param, "Play", 1.0 });
        s.events.push_back (cc (62403, 101, 40));
        s.events.push_back (cc (70001, 115, 77));
        s.events.push_back ({ 81000, Event::param, "Decay", 0.4 });
        s.events.push_back ({ 96005, Event::param, "Split Mode", 0 });
        s.events.push_back ({ 100007, Event::param, "EQ", 0.25 });
        s.events.push_back (note (110000, 50));
        s.events.push_back ({ 115201, Event::param, "LFO 1 Clock", 6 });
        s.events.push_back (note (120000, 60));
        list.push_back (s);
    }
    // Other block sizes (the same sound as random-0) and other rates.
    for (uint32_t block : { 1u, 37u, 4096u })
    {
        Scenario s = list[12];
        s.name = "random-0-block-" + std::to_string (block);
        s.block = block;
        list.push_back (s);
    }
    for (double rate : { 44100.0, 96000.0 })
    {
        Scenario s;
        s.name = "rate-" + std::to_string ((int) rate);
        s.preset = "metal-kick";
        s.rate = rate;
        s.seconds = 3.0;
        s.events = hitsEvery (3.0, 0.375, rate);
        list.push_back (s);
    }
    // The LFOs against the song: synced and free, stopped, started again, jumped, a loop's wrap.
    for (int clock : { 4, 6, 9 })
    {
        Scenario s;
        s.name = "lfo-song-" + std::to_string (clock);
        s.values = { { "LFO 1 Wave", 6 }, { "LFO 1 Int", 0.2 }, { "LFO 1 Mode", 0 }, { "LFO 1 Clock", (double) clock }, { "LFO 2 Wave", 4 },
                     { "LFO 2 Int", 0.5 }, { "LFO 2 Dest", 1 }, { "LFO 2 Clock", 10 }, { "Decay", 0.9 }, { "Metal Noise 1", 180 } };
        s.seconds = 6.0;
        s.events = hitsEvery (6.0, 0.4, 48000.0);
        s.transport = [] (uint64_t at, double bpm, double rate) -> std::pair<bool, double> {
            const double f = (double) at;
            if (at < 48000) return { true, f * bpm / 60.0 / rate };
            if (at < 72000) return { false, 2.0 };
            if (at < 144000) return { true, (f - 72000) * bpm / 60.0 / rate };
            if (at < 200000) return { true, 3.3 + (f - 144000) * bpm / 60.0 / rate };
            return { true, 100.0 / 24000.0 + (f - 200000) * bpm / 60.0 / rate };
        };
        list.push_back (s);
    }
    // Every LFO wave and mode, retriggered by fast hits.
    for (int mode : { 0, 1, 2 })
    {
        Scenario s;
        s.name = "lfo-waves-" + std::to_string (mode);
        s.seconds = 8.0;
        s.values = { { "LFO 1 Int", 0.4 }, { "LFO 1 Mode", (double) mode }, { "LFO 1 Speed", 0.7 }, { "LFO 2 Int", 0.3 }, { "LFO 2 Mode", (double) (2 - mode) },
                     { "LFO 2 Speed", 0.55 }, { "Decay", 0.7 } };
        for (int w = 0; w < 16; ++w)
        {
            s.events.push_back ({ (uint64_t) w * 24000, Event::param, "LFO 1 Wave", (double) w });
            s.events.push_back ({ (uint64_t) w * 24000 + 5, Event::param, "LFO 2 Wave", (double) (15 - w) });
            s.events.push_back (note ((uint64_t) w * 24000 + 100, 36));
            s.events.push_back (note ((uint64_t) w * 24000 + 12100, 36, 0.5));
        }
        list.push_back (s);
    }
    // The CVs: on each destination, moving.
    for (int dest = 1; dest <= 16; ++dest)
    {
        Scenario s;
        s.name = "cv-" + std::to_string (dest);
        s.seconds = 2.0;
        s.values = { { "CV 1 Dest", (double) dest }, { "CV 1 Amt", 90 }, { "CV 2 Dest", (double) (17 - dest) }, { "CV 2 Amt", -70 }, { "CV 2 In", 3.0 },
                     { "LFO 1 Int", 0.1 }, { "LFO 2 Int", 0.1 }, { "Metal Noise 2", 44 } };
        s.events = hitsEvery (2.0, 0.25, 48000.0);
        for (int i = 0; i < 8; ++i)
            s.events.push_back ({ (uint64_t) i * 12000 + 3000, Event::param, "CV 1 In", 0.6 * i });
        list.push_back (s);
    }
    // MIDI: notes and the controller map, every controller once.
    Scenario midi;
    midi.name = "midi";
    midi.seconds = 4.0;
    midi.values = { { "Split Mode", 5 } };
    {
        const int numbers[] = { 100, 101, 102, 103, 104, 105, 106, 107, 113, 114, 115, 116, 117, 119, 120, 121, 122, 123, 124, 125 };
        uint64_t f = 0;
        for (int n : numbers)
        {
            midi.events.push_back (cc (f, n, (n * 13) % 128));
            midi.events.push_back ({ f + 10, Event::midi, "", (double) 0x90, (double) ((36 + n % 40) * 256 + 100) });
            f += 9000;
        }
    }
    list.push_back (midi);
    return list;
}
} // namespace
