// MoonBass's cost: renders a pattern through the CLAP entry for SECONDS at 48 kHz in 128-frame blocks, as Melodee
// plays it, and prints the time a block takes here. tools/bench.sh runs it under callgrind for instruction counts
// (exact and repeatable, unlike timings on a computer that is not the MPC).
//   bench [worst|typical|stopped] [SECONDS]
#include "../src/moonbass.cpp"

#include <chrono>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "worst";
    const double seconds = argc > 2 ? std::atof (argv[2]) : 10.0;
    constexpr double rate = 48000.0;
    constexpr uint32_t block = 128;

    static const clap_host host { CLAP_VERSION, nullptr, "bench", "", "", "1", [] (const clap_host*, const char*) -> const void* { return nullptr; },
                                  [] (const clap_host*) {}, [] (const clap_host*) {}, [] (const clap_host*) {} };
    clap_entry.init ("");
    const auto* factory = static_cast<const clap_plugin_factory*> (clap_entry.get_factory (CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin* plugin = factory->create_plugin (factory, &host, "com.hdslave.moonbass");
    plugin->init (plugin);
    plugin->activate (plugin, rate, 1, block);
    plugin->start_processing (plugin);

    // The patch: worst keeps every part busy all the time (a hit every 5 blocks, 13 ms, with the longest gate, so the
    // attack section never rests: both metal noises, both LFOs, a long decay); typical is the default kick on every
    // beat at 120 BPM; stopped renders with nothing sounding.
    std::vector<std::pair<uint32_t, double>> values;
    uint32_t every = 0; // blocks between hits (0: none)
    using namespace moonbass::ParamId;
    if (mode == "worst")
    {
        values = { { gateTime, 1.0 }, { metalNoise1, 499 }, { metalNoise2, 333 }, { noise2Mask, 16 }, { noise, 0.8 }, { decay, 1.0 },
                   { harmonics, 0.5 }, { compression, 0.2 }, { lfo1Int, 0.3 }, { lfo1Dest, 1 }, { lfo1Mode, 0 }, { lfo2Int, 0.2 },
                   { lfo2Clock, 8 }, { lfo2Wave, 9 } };
        every = 5;
    }
    else if (mode == "typical")
        every = 188; // 500 ms

    struct Events
    {
        std::vector<clap_event_param_value> params;
        clap_event_note note {};
        std::vector<const clap_event_header*> order;
        clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->order.size(); },
                               [] (const clap_input_events* e, uint32_t i) { return static_cast<const Events*> (e->ctx)->order[i]; } };
    } first, hit, none;
    for (auto& [id, v] : values)
    {
        clap_event_param_value e {};
        e.header = { sizeof (e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = id;
        e.note_id = e.port_index = e.channel = e.key = -1;
        e.value = v;
        first.params.push_back (e);
    }
    for (auto* list : { &first, &hit })
    {
        list->note.header = { sizeof (clap_event_note), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0 };
        list->note.note_id = -1, list->note.port_index = 0, list->note.channel = 0, list->note.key = 36, list->note.velocity = 0.9;
        for (auto& p : list->params)
            list->order.push_back (&p.header);
        if (every > 0)
            list->order.push_back (&list->note.header);
    }
    clap_output_events out { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };

    std::vector<float> left (block), right (block);
    float* outs[2] = { left.data(), right.data() };
    clap_audio_buffer buffer {};
    buffer.data32 = outs;
    buffer.channel_count = 2;
    const uint64_t blocks = (uint64_t) (seconds * rate / block);
    double sum = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t b = 0; b < blocks; ++b)
    {
        clap_event_transport t {};
        t.header = { sizeof (t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (mode != "stopped" ? CLAP_TRANSPORT_IS_PLAYING : 0);
        t.tempo = 120.0;
        t.song_pos_beats = (clap_beattime) std::llround ((double) (b * block) * 2.0 / rate * (double) CLAP_BEATTIME_FACTOR);
        clap_process p {};
        p.frames_count = block;
        p.transport = &t;
        p.audio_outputs = &buffer;
        p.audio_outputs_count = 1;
        p.in_events = b == 0 ? &first.in : (every > 0 && b % every == 0 ? &hit.in : &none.in);
        p.out_events = &out;
        plugin->process (plugin, &p);
        sum += left[0];
    }
    const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("%s: %llu blocks, %.2f us a block here (checksum %.6f)\n", mode.c_str(), (unsigned long long) blocks, us / blocks, sum);
    plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    clap_entry.deinit();
    return 0;
}
