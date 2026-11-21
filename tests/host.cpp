// Loads MoonBass.clap as a host does and checks it: description and ports, every parameter and its page, state,
// presets, hits landing on their frame, the same output whatever the block size, the sound's parts (Pitch, Tune,
// Decay, Compression, Harmonics, the attack and both metal noises, EQ), what a hit takes as it is at the hit, Split
// Mode and Pitch Mode, Dynamics, two hits on one frame, the LFOs (free, retriggered, one shot, in step with the song),
// the CV inputs, the MIDI controller map, Play, and bounded output at random and extreme settings. Writes demo renders
// as WAV files.
//   host MoonBass.clap OUT_FOLDER
#include "wav.h"
#include "../src/melodee_screen.h"

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++checks; const double a_ = (a), b_ = (b); if (! (std::fabs (a_ - b_) <= (tol))) { ++failures; \
    std::printf ("FAIL %s:%d  %s = %.6g, expected %.6g (+-%.3g)\n", __FILE__, __LINE__, #a, a_, b_, (double) (tol)); } } while (0)

const clap_plugin_entry* entry = nullptr;
const clap_plugin_factory* factory = nullptr;

// The host side: log, and the two a preset load tells (counted); the plug-in must cope without the rest.
int rescans = 0, presetsLoaded = 0, presetErrors = 0;
int paints = 0, processRequests = 0, flushes = 0;
const void* hostExtension (const clap_host*, const char* id)
{
    static const clap_host_log log { [] (const clap_host*, clap_log_severity s, const char* m) { std::printf ("  [plug-in log %d] %s\n", (int) s, m); } };
    static const clap_host_params params { [] (const clap_host*, clap_param_rescan_flags f) { rescans += (f & CLAP_PARAM_RESCAN_VALUES) != 0; },
                                           [] (const clap_host*, clap_id, clap_param_clear_flags) {}, [] (const clap_host*) { ++flushes; } };
    static const clap_host_preset_load presetLoad {
        [] (const clap_host*, uint32_t, const char*, const char*, int32_t, const char*) { ++presetErrors; },
        [] (const clap_host*, uint32_t, const char*, const char*) { ++presetsLoaded; },
    };
    static const clap_host_melodee_screen screen { [] (const clap_host*) { ++paints; } };
    if (std::strcmp (id, CLAP_EXT_MELODEE_SCREEN_HOST) == 0) return &screen;
    if (std::strcmp (id, CLAP_EXT_LOG) == 0) return &log;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
    if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0) return &presetLoad;
    return nullptr;
}

clap_host host {
    CLAP_VERSION, nullptr, "MoonBass test host", "tests", "", "1",
    hostExtension, [] (const clap_host*) {}, [] (const clap_host*) { ++processRequests; }, [] (const clap_host*) {},
};

// Input events for one process call, in time order.
struct Events
{
    struct Any { union { clap_event_header header; clap_event_param_value param; clap_event_note note; clap_event_midi midi; }; };
    std::vector<Any> list;
    clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->list.size(); },
                           [] (const clap_input_events* e, uint32_t i) { return &static_cast<const Events*> (e->ctx)->list[i].header; } };
    void param (uint32_t time, clap_id id, double value)
    {
        Any a {};
        a.param = { { sizeof (clap_event_param_value), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id, nullptr, -1, -1, -1, -1, value };
        list.push_back (a);
    }
    void note (uint32_t time, uint16_t type, int16_t key, double velocity)
    {
        Any a {};
        a.note = { { sizeof (clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 }, -1, 0, 0, key, velocity };
        list.push_back (a);
    }
    void midi (uint32_t time, uint8_t status, uint8_t a1, uint8_t a2)
    {
        Any a {};
        a.midi = { { sizeof (clap_event_midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 }, 0, { status, a1, a2 } };
        list.push_back (a);
    }
    void sort() { std::stable_sort (list.begin(), list.end(), [] (const Any& a, const Any& b) { return a.header.time < b.header.time; }); }
};

// What the plug-in sends back: parameter values it changed itself (Play returning to 0, a MIDI controller).
struct OutEvents
{
    std::vector<std::pair<clap_id, double>> values;
    clap_output_events out { this, [] (const clap_output_events* o, const clap_event_header* h) {
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* v = reinterpret_cast<const clap_event_param_value*> (h);
            static_cast<OutEvents*> (o->ctx)->values.push_back ({ v->param_id, v->value });
        }
        return true;
    } };
};

// The transport a block hears: Melodee's way, the place at the block's start counted on by its frames.
struct Transport
{
    bool playing = true;
    double bpm = 120.0, beats = 0.0;
};
Transport stopped (uint64_t) { return { false, 120.0, 0.0 }; }

// A hit: a note at a frame (key 36, C1, unless said).
struct Hit
{
    uint64_t frame;
    int key = 36;
    double velocity = 1.0;
};

struct Instance
{
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const clap_plugin_state* state = nullptr;
    const clap_plugin_preset_load* presets = nullptr;
    std::map<std::string, clap_id> ids;
    double rate = 48000.0;

    explicit Instance (double sampleRate = 48000.0, uint32_t maxFrames = 4096) : rate (sampleRate)
    {
        const auto* d = factory->get_plugin_descriptor (factory, 0);
        plugin = factory->create_plugin (factory, &host, d->id);
        CHECK (plugin != nullptr && plugin->init (plugin));
        params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        for (uint32_t i = 0; params && i < params->count (plugin); ++i)
        {
            clap_param_info info {};
            params->get_info (plugin, i, &info);
            ids[info.name] = info.id;
        }
        CHECK (plugin->activate (plugin, rate, 1, maxFrames));
        CHECK (plugin->start_processing (plugin));
    }
    ~Instance()
    {
        plugin->stop_processing (plugin);
        plugin->deactivate (plugin);
        plugin->destroy (plugin);
    }
    clap_id id (const std::string& name)
    {
        const auto it = ids.find (name);
        if (it == ids.end())
        {
            std::printf ("FAIL no parameter named \"%s\"\n", name.c_str());
            ++failures;
            return CLAP_INVALID_ID;
        }
        return it->second;
    }
    bool loadPreset (const char* key) { return presets != nullptr && presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key); }
    double value (const std::string& name)
    {
        double v = 0.0;
        params->get_value (plugin, id (name), &v);
        return v;
    }
    double valueOf (const std::string& name, const std::string& text)
    {
        double v = 0.0;
        CHECK (params->text_to_value (plugin, id (name), text.c_str(), &v));
        return v;
    }
    std::string text (const std::string& name, double v)
    {
        char t[64] = {};
        CHECK (params->value_to_text (plugin, id (name), v, t, sizeof (t)));
        return t;
    }

    // Renders `frames` in blocks of `block`, with `events(frame, frames, list)` adding events for the block starting
    // at `frame`; the transport is asked for each block.
    void render (uint32_t frames, uint32_t block, std::vector<float>& left, std::vector<float>& right,
                 const std::function<Transport (uint64_t)>& transport, const std::function<void (uint64_t, uint32_t, Events&)>& events = {},
                 OutEvents* out = nullptr)
    {
        left.assign (frames, 0.0f);
        right.assign (frames, 0.0f);
        OutEvents ignored;
        for (uint64_t at = 0; at < frames; at += block)
        {
            const uint32_t n = (uint32_t) std::min<uint64_t> (block, frames - at);
            const auto t = transport (at);
            clap_event_transport tr {};
            tr.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO | (t.beats >= 0.0 ? CLAP_TRANSPORT_HAS_BEATS_TIMELINE : 0) | (t.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            tr.tempo = t.bpm;
            tr.song_pos_beats = (clap_beattime) std::llround (t.beats * (double) CLAP_BEATTIME_FACTOR);
            Events e;
            if (events)
                events (at, n, e);
            e.sort();
            float* outs[2] = { left.data() + at, right.data() + at };
            clap_audio_buffer buffer {};
            buffer.data32 = outs;
            buffer.channel_count = 2;
            clap_process p {};
            p.steady_time = (int64_t) at;
            p.frames_count = n;
            p.transport = &tr;
            p.audio_outputs = &buffer;
            p.audio_outputs_count = 1;
            p.in_events = &e.in;
            p.out_events = out ? &out->out : &ignored.out;
            CHECK (plugin->process (plugin, &p) != CLAP_PROCESS_ERROR);
        }
    }
    // Parameter changes before anything plays: one silent, stopped block.
    void setup (const std::vector<std::pair<std::string, double>>& values)
    {
        std::vector<float> l, r;
        render (1, 1, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0)
                for (auto& [name, v] : values)
                    e.param (0, id (name), v);
        });
    }
    // The hits (and any other events) over `seconds`; the left channel (the right is the same).
    std::vector<float> play (const std::vector<Hit>& hits, double seconds, uint32_t block = 128,
                             const std::function<Transport (uint64_t)>& transport = stopped,
                             const std::function<void (uint64_t, uint32_t, Events&)>& more = {}, std::vector<float>* rightOut = nullptr)
    {
        std::vector<float> l, r;
        render ((uint32_t) (seconds * rate), block, l, r, transport, [&] (uint64_t at, uint32_t n, Events& e) {
            for (const auto& h : hits)
                if (h.frame >= at && h.frame < at + n)
                    e.note ((uint32_t) (h.frame - at), CLAP_EVENT_NOTE_ON, (int16_t) h.key, h.velocity);
            if (more)
                more (at, n, e);
        });
        if (rightOut)
            *rightOut = r;
        return l;
    }
};

using Settings = std::vector<std::pair<std::string, double>>;

// A plain sine kick for measuring: no pitch envelope, no harmonics, no attack, EQ open, quiet enough that the output's
// soft ceiling stays out of the way, the longest decay.
Settings clean (Settings more = {})
{
    Settings s { { "Tune", 0.0 }, { "Harmonics", 0.0 }, { "Pulse", 0.0 }, { "Noise", 0.0 }, { "Attack", 0.0 }, { "EQ", 0.0 },
                 { "Volume", 0.35 }, { "Gate Time", 0.0 }, { "Compression", 0.0 }, { "Decay", 1.0 } };
    s.insert (s.end(), more.begin(), more.end());
    return s;
}

std::vector<float> kick (const Settings& settings, const std::vector<Hit>& hits, double seconds, double rate = 48000.0, uint32_t block = 128)
{
    Instance a (rate);
    a.setup (settings);
    return a.play (hits, seconds, block);
}

// The frequency between two frames, from the rising zero crossings (interpolated).
double frequency (const std::vector<float>& x, double rate, size_t from, size_t to)
{
    double first = -1.0, last = -1.0;
    int n = 0;
    for (size_t i = std::max<size_t> (from, 1); i < std::min (to, x.size()); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = (double) (i - 1) + (double) x[i - 1] / ((double) x[i - 1] - (double) x[i]);
            if (first < 0.0)
                first = t;
            last = t, ++n;
        }
    return n > 1 ? (n - 1) * rate / (last - first) : 0.0;
}

float peak (const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        m = std::max (m, std::fabs (x[i]));
    return m;
}

double energy (const std::vector<float>& x, size_t from, size_t to)
{
    double e = 0.0;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        e += (double) x[i] * x[i];
    return e;
}

// The energy of the first difference: how much high-frequency content there is.
double brightness (const std::vector<float>& x, size_t from, size_t to)
{
    double e = 0.0;
    for (size_t i = std::max<size_t> (from, 1); i < std::min (to, x.size()); ++i)
        e += ((double) x[i] - x[i - 1]) * ((double) x[i] - x[i - 1]);
    return e;
}

// The amplitude at frequency f over [from, to) (Goertzel).
double amplitudeAt (const std::vector<float>& x, double rate, double f, size_t from, size_t to)
{
    const double w = 2.0 * M_PI * f / rate, c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        const double s = x[i] + c * s1 - s2;
        s2 = s1, s1 = s;
    }
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / (double) (to - from);
}

long firstSound (const std::vector<float>& x, long from = 0, float threshold = 1.0e-6f)
{
    for (long i = from; i < (long) x.size(); ++i)
        if (std::fabs (x[(size_t) i]) > threshold)
            return i;
    return -1;
}

bool same (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = SIZE_MAX)
{
    to = std::min ({ to, a.size(), b.size() });
    return a.size() == b.size() && std::memcmp (a.data() + from, b.data() + from, (to - from) * sizeof (float)) == 0;
}

bool finiteAndBounded (const std::vector<float>& x)
{
    for (float v : x)
        if (! std::isfinite (v) || std::fabs (v) > 1.0f)
            return false;
    return true;
}

// --- Description, parameters, state, presets ---------------------------------------------------------------------

void testDescription()
{
    CHECK (factory->get_plugin_count (factory) == 1);
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    CHECK (d != nullptr && std::string (d->name) == "hdslave MoonBass" && std::string (d->id) == "com.hdslave.moonbass");
    bool instrument = false, drum = false;
    for (const char* const* f = d->features; f && *f; ++f)
        instrument |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0, drum |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_DRUM) == 0;
    CHECK (instrument && drum);
    bool stereo = false, mono = false; // the audio port is stereo (the same both sides); "mono" is an audio feature in CLAP
    for (const char* const* f = d->features; f && *f; ++f)
        stereo |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_STEREO) == 0, mono |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_MONO) == 0;
    CHECK (stereo && ! mono);

    Instance a;
    const auto* audio = static_cast<const clap_plugin_audio_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_AUDIO_PORTS));
    const auto* notes = static_cast<const clap_plugin_note_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_NOTE_PORTS));
    CHECK (audio && audio->count (a.plugin, true) == 0 && audio->count (a.plugin, false) == 1);
    clap_audio_port_info port {};
    CHECK (audio && audio->get (a.plugin, 0, false, &port) && port.channel_count == 2);
    CHECK (notes && notes->count (a.plugin, true) == 1 && notes->count (a.plugin, false) == 0);
    clap_note_port_info np {};
    CHECK (notes && notes->get (a.plugin, 0, true, &np) && (np.supported_dialects & CLAP_NOTE_DIALECT_MIDI) && (np.supported_dialects & CLAP_NOTE_DIALECT_CLAP));
}

// Every parameter: its info, default, text both ways; the pages (three to a page, one module a page); the texts at the
// ends of the documented ranges.
void testParameters()
{
    Instance a;
    const uint32_t count = a.params->count (a.plugin);
    CHECK (count == 42);
    CHECK (count % 3 == 0);
    std::set<std::string> names;
    std::set<clap_id> ids;
    std::vector<std::string> modules;
    for (uint32_t i = 0; i < count; ++i)
    {
        clap_param_info info {};
        CHECK (a.params->get_info (a.plugin, i, &info));
        CHECK (names.insert (info.name).second);
        CHECK (ids.insert (info.id).second);
        CHECK (info.id == i); // ids are the table's order in 0.1 (they never move: new parameters take new ids)
        CHECK (std::strncmp (info.name, "MIDI Ch.", 8) != 0 && std::strncmp (info.name, "MIDI CC", 7) != 0); // Melodee hides those
        CHECK (std::strlen (info.name) <= 16); // fits the MPC's knob labels
        CHECK (info.min_value < info.max_value && info.default_value >= info.min_value && info.default_value <= info.max_value);
        CHECK ((info.flags & CLAP_PARAM_IS_AUTOMATABLE) && ! (info.flags & CLAP_PARAM_IS_HIDDEN));
        modules.push_back (info.module);
        double v = -1.0;
        CHECK (a.params->get_value (a.plugin, info.id, &v) && v == info.default_value);
        // Text both ways, at several values: the text read back gives the same text.
        for (double f : { 0.0, 0.13, 0.5, 0.77, 1.0 })
        {
            double x = info.min_value + f * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                x = std::round (x);
            char t1[64] = {}, t2[64] = {};
            CHECK (a.params->value_to_text (a.plugin, info.id, x, t1, sizeof (t1)) && t1[0] != 0);
            double back = 0.0;
            CHECK (a.params->text_to_value (a.plugin, info.id, t1, &back));
            CHECK (a.params->value_to_text (a.plugin, info.id, back, t2, sizeof (t2)) && std::string (t1) == t2);
            if (std::string (t1) != t2)
                std::printf ("  %s: %s -> %g -> %s\n", info.name, t1, back, t2);
        }
    }
    for (uint32_t page = 0; page < count / 3; ++page)
        CHECK (modules[3 * page] == modules[3 * page + 1] && modules[3 * page] == modules[3 * page + 2]);
    // The ends of the ranges.
    CHECK (a.text ("Pitch", 0.0) == "10.0 Hz" && a.text ("Pitch", 1.0) == "175.0 Hz" && a.text ("Pitch", 0.5623) == "50.0 Hz");
    CHECK (a.text ("Decay", 0.0) == "30 ms" && a.text ("Decay", 1.0) == "2.00 s");
    CHECK (a.text ("Gate Time", 0.0) == "0.10 ms" && a.text ("Gate Time", 1.0) == "16.0 ms");
    CHECK (a.text ("EQ", 0.0) == "20.00 kHz" && a.text ("EQ", 1.0) == "150.0 Hz");
    CHECK (a.text ("Tune", 1.0) == "+4.00 oct");
    CHECK (a.text ("LFO 1 Speed", 0.0) == "0.10 Hz" && a.text ("LFO 1 Speed", 1.0) == "40.0 Hz");
    CHECK (a.text ("Metal Noise 1", 0) == "Off" && a.text ("Metal Noise 1", 1) == "Noise" && a.text ("Metal Noise 1", 499) == "499");
    CHECK (a.text ("Metal Noise 2", 0) == "Off" && a.text ("Metal Noise 2", 1) == "1");
    CHECK (a.text ("Noise 2 Mask", 1) == "M01" && a.text ("Noise 2 Mask", 16) == "M16");
    CHECK (a.text ("CV 1 Amt", -128) == "-128" && a.text ("CV 1 Amt", 127) == "+127" && a.text ("CV 4 In", 5.0) == "5.00 V");
    CHECK (a.text ("LFO 2 Wave", 15) == "Pulse 12" && a.text ("LFO 1 Clock", 4) == "1/4" && a.text ("CV 2 Dest", 16) == "Accent");
    CHECK (a.text ("Split Mode", 6) == "C1-F3" && a.text ("Pitch Mode", 0) == "Linear");
    CHECK (a.valueOf ("Metal Noise 1", "Noise") == 1.0 && a.valueOf ("Noise 2 Mask", "M07") == 7.0 && a.valueOf ("LFO 1 Mode", "One Shot") == 2.0);
    CHECK_NEAR (a.valueOf ("Pitch", "175 Hz"), 1.0, 1e-5);
    CHECK_NEAR (a.valueOf ("Decay", "2 s"), 1.0, 1e-5);
    CHECK_NEAR (a.valueOf ("EQ", "150 Hz"), 1.0, 1e-5);
    double v = 0.0;
    CHECK (! a.params->text_to_value (a.plugin, a.id ("Pitch"), "loud", &v));
    // Out-of-range and NaN values are held to the range.
    std::vector<float> l, r;
    a.render (1, 1, l, r, stopped, [&] (uint64_t, uint32_t, Events& e) {
        e.param (0, a.id ("Pitch"), 7.0);
        e.param (0, a.id ("CV 1 Amt"), -1000.0);
        e.param (0, a.id ("Decay"), std::nan (""));
    });
    CHECK (a.value ("Pitch") == 1.0 && a.value ("CV 1 Amt") == -128.0 && a.value ("Decay") == 0.0);
}

void testState()
{
    const Settings settings { { "Pitch", 0.3 }, { "Metal Noise 1", 222 }, { "LFO 2 Mode", 2 }, { "CV 3 Amt", -77 }, { "CV 3 In", 2.5 }, { "Split Mode", 3 } };
    std::vector<uint8_t> saved;
    {
        Instance a;
        a.setup (settings);
        clap_ostream os { &saved, [] (const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
            auto* v = static_cast<std::vector<uint8_t>*> (s->ctx);
            v->insert (v->end(), static_cast<const uint8_t*> (d), static_cast<const uint8_t*> (d) + n);
            return (int64_t) n;
        } };
        CHECK (a.state->save (a.plugin, &os));
    }
    struct Reader { const std::vector<uint8_t>* data; size_t at; };
    auto load = [] (Instance& b, const std::vector<uint8_t>& data) {
        Reader reader { &data, 0 };
        clap_istream is { &reader, [] (const clap_istream* s, void* d, uint64_t n) -> int64_t {
            auto* r = static_cast<Reader*> (s->ctx);
            const uint64_t k = std::min<uint64_t> (n, r->data->size() - r->at);
            std::memcpy (d, r->data->data() + r->at, k);
            r->at += k;
            return (int64_t) k;
        } };
        return b.state->load (b.plugin, &is);
    };
    Instance b;
    CHECK (load (b, saved));
    for (auto& [name, v] : settings)
        CHECK_NEAR (b.value (name), v, 1e-6);
    CHECK (b.value ("Tune") == 0.5); // the rest as saved: defaults

    // Another version's state: an unknown id is skipped, a missing one keeps its default.
    std::vector<uint8_t> other = { 'M', 'O', 'O', 'N', 1, 0, 0, 0, 2, 0, 0, 0 };
    auto pair = [&] (uint32_t id, float value) {
        const uint8_t* p = reinterpret_cast<const uint8_t*> (&id);
        other.insert (other.end(), p, p + 4);
        p = reinterpret_cast<const uint8_t*> (&value);
        other.insert (other.end(), p, p + 4);
    };
    pair (9999, 1.0f);
    pair (b.id ("Harmonics"), 0.9f);
    Instance c;
    CHECK (load (c, other));
    CHECK_NEAR (c.value ("Harmonics"), 0.9, 1e-6);
    CHECK (c.value ("Pitch") == 0.5623f);
    std::vector<uint8_t> wrong = { 'A', 'N', 'V', 'L', 1, 0, 0, 0, 0, 0, 0, 0 };
    CHECK (! load (c, wrong));
}

// The factory presets: what the demo renders play. Each preset sets these, the rest at their defaults.
struct Demo
{
    const char* key;
    Settings settings;
    std::vector<Hit> hits; // empty: on every beat at 120 BPM, key 36
};
const std::vector<Demo>& demos()
{
    static const std::vector<Demo> list {
        { "default", {}, {} },
        { "gut-punch", { { "Tune", 0.7 }, { "Pitch", 0.576 }, { "Decay", 0.62 }, { "Harmonics", 0.2 }, { "Pulse", 0.7 }, { "Noise", 0.25 },
                         { "Attack", 0.7 }, { "EQ", 0.25 }, { "Gate Time", 0.25 }, { "Compression", 0.1 } }, {} },
        { "soft-boom", { { "Tune", 0.15 }, { "Pitch", 0.5255 }, { "Decay", 0.85 }, { "Harmonics", 0.0 }, { "Pulse", 0.2 }, { "Noise", 0.05 },
                         { "Attack", 0.3 }, { "EQ", 0.8 }, { "Gate Time", 0.2 } }, {} },
        { "sub-drop", { { "Tune", 0.4 }, { "Pitch", 0.3838 }, { "Decay", 0.95 }, { "Harmonics", 0.05 }, { "Pulse", 0.3 }, { "Noise", 0.0 },
                        { "Attack", 0.2 }, { "EQ", 0.6 } }, {} },
        { "clap-kick", { { "Tune", 0.45 }, { "Decay", 0.5 }, { "Pulse", 0.5 }, { "Noise", 0.8 }, { "Attack", 0.85 }, { "EQ", 0.05 }, { "Gate Time", 0.95 } }, {} },
        { "metal-kick", { { "Tune", 0.6 }, { "Decay", 0.55 }, { "Harmonics", 0.4 }, { "Noise", 0.7 }, { "Attack", 0.7 }, { "EQ", 0.1 }, { "Gate Time", 0.6 },
                          { "Metal Noise 1", 337 }, { "Metal Noise 2", 77 }, { "Noise 2 Mask", 12 } }, {} },
        { "wobble-tail", { { "Tune", 0.35 }, { "Pitch", 0.5 }, { "Decay", 0.9 }, { "LFO 1 Wave", 2 }, { "LFO 1 Int", 0.25 }, { "LFO 1 Mode", 0 }, { "LFO 1 Clock", 8 } }, {} },
        { "zap", { { "Tune", 0.3 }, { "Decay", 0.55 }, { "Harmonics", 0.3 }, { "LFO 1 Wave", 1 }, { "LFO 1 Speed", 0.75 }, { "LFO 1 Int", 0.6 }, { "LFO 1 Mode", 2 } }, {} },
        { "bass-line", { { "Split Mode", 6 }, { "Pitch Mode", 1 }, { "Pitch", 0.4139 }, { "Tune", 0.1 }, { "Decay", 0.8 }, { "Harmonics", 0.5 },
                         { "Pulse", 0.15 }, { "Noise", 0.0 }, { "Attack", 0.2 }, { "EQ", 0.35 } },
          { { 0, 36 }, { 6000, 36, 0.6 }, { 12000, 48 }, { 18000, 43, 0.7 }, { 24000, 39 }, { 36000, 46, 0.8 }, { 42000, 36 }, { 48000, 51, 0.6 },
            { 54000, 48 }, { 60000, 41 }, { 72000, 36 }, { 84000, 43 }, { 90000, 55, 0.7 } } },
        { "pad-tunings", { { "Split Mode", 2 }, { "Pitch Mode", 0 }, { "Pitch", 0.45 }, { "Tune", 0.55 }, { "Decay", 0.6 }, { "Pulse", 0.6 }, { "Attack", 0.6 } },
          { { 0, 36 }, { 12000, 37 }, { 24000, 38 }, { 36000, 39 }, { 48000, 40 }, { 60000, 41 }, { 72000, 42 }, { 84000, 43 } } },
    };
    return list;
}
std::vector<Hit> beats (double seconds, double rate = 48000.0)
{
    std::vector<Hit> h;
    for (double t = 0.0; t < seconds; t += 0.5)
        h.push_back ({ (uint64_t) std::llround (t * rate) });
    return h;
}

void testPresets (const std::string& outFolder)
{
    // As preset discovery lists them.
    const auto* discovery = static_cast<const clap_preset_discovery_factory*> (entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
    CHECK (discovery != nullptr && discovery->count (discovery) == 1);
    struct Found { std::vector<std::pair<std::string, std::string>> presets; int locations = 0; } found;
    clap_preset_discovery_indexer indexer { CLAP_VERSION, "tests", "", "", "1", &found,
        [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_filetype*) { return true; },
        [] (const clap_preset_discovery_indexer* i, const clap_preset_discovery_location* l) {
            static_cast<Found*> (i->indexer_data)->locations += l->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
            return true;
        },
        [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_soundpack*) { return true; },
        [] (const clap_preset_discovery_indexer*, const char*) -> const void* { return nullptr; } };
    const auto* provider = discovery->create (discovery, &indexer, discovery->get_descriptor (discovery, 0)->id);
    CHECK (provider != nullptr && provider->init (provider) && found.locations == 1);
    clap_preset_discovery_metadata_receiver receiver { &found, [] (const clap_preset_discovery_metadata_receiver*, int32_t, const char*) {},
        [] (const clap_preset_discovery_metadata_receiver* r, const char* name, const char* key) {
            static_cast<Found*> (r->receiver_data)->presets.push_back ({ name, key });
            return true;
        },
        [] (const clap_preset_discovery_metadata_receiver*, const clap_universal_plugin_id*) {},
        [] (const clap_preset_discovery_metadata_receiver*, const char*) {},
        [] (const clap_preset_discovery_metadata_receiver*, uint32_t) {},
        [] (const clap_preset_discovery_metadata_receiver*, const char*) {},
        [] (const clap_preset_discovery_metadata_receiver*, const char*) {},
        [] (const clap_preset_discovery_metadata_receiver*, clap_timestamp, clap_timestamp) {},
        [] (const clap_preset_discovery_metadata_receiver*, const char*) {},
        [] (const clap_preset_discovery_metadata_receiver*, const char*, const char*) {} };
    CHECK (provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver));
    provider->destroy (provider);
    CHECK (found.presets.size() == demos().size());
    for (size_t i = 0; i < std::min (found.presets.size(), demos().size()); ++i)
        CHECK (found.presets[i].second == demos()[i].key);

    for (const auto& demo : demos())
    {
        const auto hits = demo.hits.empty() ? beats (4.0) : demo.hits;
        // By hand on the defaults...
        Instance a;
        a.setup (demo.settings);
        std::vector<float> right;
        const auto byHand = a.play (hits, 4.0, 128, stopped, {}, &right);
        CHECK (same (byHand, right));
        CHECK (finiteAndBounded (byHand));
        CHECK (peak (byHand, 0, byHand.size()) > 0.05f);
        // ...and loaded as the preset, over another preset first: the same values and the same sound.
        Instance b;
        const int rescansBefore = rescans, loadedBefore = presetsLoaded;
        CHECK (b.loadPreset ("metal-kick") && b.loadPreset (demo.key));
        CHECK (rescans == rescansBefore + 2 && presetsLoaded == loadedBefore + 2);
        for (const auto& [name, id] : a.ids)
            CHECK (a.value (name) == b.value (name));
        b.setup ({});
        CHECK (same (byHand, b.play (hits, 4.0)));
        writeWav ((outFolder + "/demo-" + demo.key + ".wav").c_str(), byHand, byHand, 48000);
    }
    Instance c;
    const int errorsBefore = presetErrors;
    CHECK (! c.loadPreset ("no-such-preset") && presetErrors == errorsBefore + 1);
}

// --- Timing and block sizes --------------------------------------------------------------------------------------

void testTiming()
{
    // A hit sounds from its own frame (silence before), at each rate and block size.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (uint32_t block : { 1u, 37u, 128u, 4096u })
        {
            const std::vector<Hit> hits { { 1000 }, { 7777 }, { 30001 } };
            const auto x = kick ({}, hits, 1.0, rate, block);
            CHECK (firstSound (x) == 1000);
            CHECK (peak (x, 0, 1000) == 0.0f);
            CHECK (std::fabs (x[7777]) > 0.0f && std::fabs (x[30001]) > 0.0f);
        }
    // The host's reset starts everything again, the noise generators too: the same events then play as from a fresh
    // instance.
    {
        const Settings noisy { { "Noise", 1.0 }, { "Attack", 1.0 }, { "Gate Time", 1.0 }, { "Metal Noise 1", 300 }, { "Metal Noise 2", 41 },
                               { "LFO 1 Int", 0.3 }, { "LFO 1 Mode", 0 }, { "LFO 2 Int", 0.2 }, { "LFO 2 Clock", 6 } };
        const std::vector<Hit> hits { { 100 }, { 5000 }, { 9000, 44 } };
        const auto playing = [] (uint64_t at) { return Transport { true, 132.0, 1.0 + (double) at * 2.2 / 48000.0 }; };
        Instance fresh;
        fresh.setup (noisy);
        const auto first = fresh.play (hits, 0.5, 128, playing);
        Instance used;
        used.setup (noisy);
        used.play ({ { 0 }, { 333, 50 }, { 7000 } }, 0.4, 128, playing);
        used.plugin->reset (used.plugin);
        used.setup ({});
        CHECK (same (first, used.play (hits, 0.5, 128, playing)));
    }
    // The kick dies away to exact silence, and plays again after it.
    {
        const auto x = kick ({ { "Decay", 0.0 } }, { { 0 }, { 40000 } }, 1.0);
        CHECK (peak (x, 20000, 40000) == 0.0f);
        CHECK (firstSound (x, 20000) == 40000);
    }
    // The same output whatever the block size: hits, knobs moving, MIDI controllers, both LFOs (one in step with the
    // song), CVs, Play, with the transport playing.
    Settings busy { { "LFO 1 Int", 0.3 }, { "LFO 1 Mode", 0 }, { "LFO 1 Clock", 6 }, { "LFO 1 Wave", 4 }, { "LFO 2 Int", 0.2 },
                    { "LFO 2 Dest", 1 }, { "LFO 2 Speed", 0.8 }, { "Metal Noise 2", 99 }, { "Noise", 0.6 }, { "Gate Time", 0.8 },
                    { "CV 1 Dest", 2 }, { "CV 1 Amt", 50 }, { "CV 2 Dest", 16 }, { "CV 2 Amt", -60 }, { "CV 2 In", 2.0 } };
    std::vector<float> reference;
    for (uint32_t block : { 1u, 37u, 128u, 4096u })
    {
        Instance a;
        a.setup (busy);
        const auto x = a.play ({ { 0 }, { 9000, 40, 0.5 }, { 30000 }, { 30100 }, { 61111, 36, 0.8 } }, 2.0, block,
                               [&] (uint64_t at) { return Transport { true, 120.0, (double) at * 2.0 / 48000.0 }; },
                               [&] (uint64_t at, uint32_t n, Events& e) {
                                   auto due = [&] (uint64_t f) { return f >= at && f < at + n; };
                                   if (due (5003)) e.param ((uint32_t) (5003 - at), a.id ("Pitch"), 0.8);
                                   if (due (12000)) e.midi ((uint32_t) (12000 - at), 0xb0, 107, 90);
                                   if (due (20000)) e.param ((uint32_t) (20000 - at), a.id ("CV 1 In"), 4.0);
                                   if (due (45000)) e.param ((uint32_t) (45000 - at), a.id ("Play"), 1.0);
                                   if (due (50001)) e.param ((uint32_t) (50001 - at), a.id ("LFO 1 Clock"), 8);
                                   if (due (70000)) e.midi ((uint32_t) (70000 - at), 0x90, 36, 100);
                               });
        CHECK (finiteAndBounded (x));
        if (reference.empty())
            reference = x;
        else
            CHECK (same (reference, x));
    }
}

// --- The sound's parts ----------------------------------------------------------------------------------------------

void testPitchAndTune()
{
    // Pitch: 10 Hz, 50 Hz and 175 Hz at the ends and middle of its range, at two rates.
    for (double rate : { 44100.0, 96000.0 })
        for (auto [knob, hz] : { std::pair { 0.0, 10.0 }, std::pair { 0.5623, 50.0 }, std::pair { 1.0, 175.0 } })
        {
            const auto x = kick (clean ({ { "Pitch", knob } }), { { 0 } }, 1.5, rate);
            CHECK_NEAR (frequency (x, rate, (size_t) (0.1 * rate), (size_t) (1.4 * rate)), hz, hz * 0.003);
        }
    // Tune: the pitch starts higher and falls back to Pitch; at 0 it starts where it ends.
    const auto flat = kick (clean ({ { "Pitch", 0.5623 } }), { { 0 } }, 0.5);
    const auto tuned = kick (clean ({ { "Pitch", 0.5623 }, { "Tune", 0.5 } }), { { 0 } }, 0.5);
    CHECK_NEAR (frequency (flat, 48000, 0, 2400), 50.0, 1.0);
    CHECK (frequency (tuned, 48000, 0, 1920) > 75.0); // the first 40 ms: from two octaves up, falling
    CHECK_NEAR (frequency (tuned, 48000, 9600, 24000), 50.0, 0.5);
}

void testDecayAndCompression()
{
    // Decay: the time to fall 60 dB, from the ratio of two peaks a known time apart.
    for (auto [knob, seconds] : { std::pair { 0.3, 0.03 * std::pow (2.0 / 0.03, 0.3) }, std::pair { 0.7, 0.03 * std::pow (2.0 / 0.03, 0.7) }, std::pair { 1.0, 2.0 } })
    {
        const auto x = kick (clean ({ { "Pitch", 1.0 }, { "Decay", knob } }), { { 0 } }, 2.5);
        const double t1 = 0.02, t2 = std::min (0.02 + seconds * 0.5, 1.5);
        const double a1 = peak (x, (size_t) (t1 * 48000), (size_t) (t1 * 48000) + 300), a2 = peak (x, (size_t) (t2 * 48000), (size_t) (t2 * 48000) + 300);
        const double measured = -6.90775528 * (t2 - t1) / std::log (a2 / a1);
        CHECK_NEAR (measured, seconds, seconds * 0.03);
    }
    // Compression: the more, the sooner it is over; the start stays as loud.
    long last = 1 << 30;
    float firstPeak = 0.0f;
    for (double c : { 0.0, 0.3, 0.6, 0.9 })
    {
        const auto x = kick (clean ({ { "Pitch", 1.0 }, { "Decay", 0.8 }, { "Compression", c } }), { { 0 } }, 2.0);
        long end = (long) x.size();
        while (end > 0 && x[(size_t) end - 1] == 0.0f)
            --end;
        CHECK (end < last);
        last = end;
        if (c == 0.0)
            firstPeak = peak (x, 0, 300);
        else
            CHECK (peak (x, 0, 300) > 0.5f * firstPeak);
    }
    CHECK (last < 48000 / 10); // at 90 %, a 0.8 s decay is over in under 100 ms
}

void testHarmonicsAndEq()
{
    // Harmonics: from the sine toward the cusped wave: its second and third harmonics come up, the fundamental stays.
    const auto sine = kick (clean ({ { "Pitch", 0.5623 } }), { { 0 } }, 1.0);
    const auto bent = kick (clean ({ { "Pitch", 0.5623 }, { "Harmonics", 1.0 } }), { { 0 } }, 1.0);
    const size_t from = 4800, to = 4800 + 9600; // 200 ms: ten cycles
    CHECK (amplitudeAt (bent, 48000, 100.0, from, to) > 20.0 * amplitudeAt (sine, 48000, 100.0, from, to));
    CHECK (amplitudeAt (bent, 48000, 150.0, from, to) > 0.05 * amplitudeAt (bent, 48000, 50.0, from, to));
    CHECK_NEAR (amplitudeAt (bent, 48000, 50.0, from, to) / amplitudeAt (sine, 48000, 50.0, from, to), 1.0, 0.05);
    // EQ: closed, the click and the noise are gone; the fundamental gets through.
    const Settings bright { { "Pulse", 1.0 }, { "Noise", 1.0 }, { "Attack", 1.0 }, { "Gate Time", 0.5 }, { "Volume", 0.4 } };
    Settings dark = bright;
    dark.push_back ({ "EQ", 1.0 });
    Settings open = bright;
    open.push_back ({ "EQ", 0.0 });
    const auto o = kick (open, { { 0 } }, 0.5), d = kick (dark, { { 0 } }, 0.5);
    CHECK (brightness (d, 0, 960) < 0.01 * brightness (o, 0, 960));
    CHECK (peak (d, 4800, 24000) > 0.3f * peak (o, 4800, 24000));
}

void testAttack()
{
    const Settings body { { "Volume", 0.4 }, { "EQ", 0.0 }, { "Gate Time", 0.7 } };
    auto with = [&] (Settings more) { Settings s = body; s.insert (s.end(), more.begin(), more.end()); return kick (s, { { 0 } }, 0.3); };
    const auto none = with ({ { "Attack", 0.0 }, { "Pulse", 1.0 }, { "Noise", 1.0 } });
    const auto silentParts = with ({ { "Attack", 1.0 }, { "Pulse", 0.0 }, { "Noise", 0.0 } });
    CHECK (same (none, silentParts)); // Attack 0, or both parts at 0: the body alone
    const auto pulse = with ({ { "Attack", 1.0 }, { "Pulse", 1.0 }, { "Noise", 0.0 } });
    const auto noise = with ({ { "Attack", 1.0 }, { "Pulse", 0.0 }, { "Noise", 1.0 } });
    CHECK (! same (none, pulse) && ! same (none, noise) && ! same (pulse, noise));
    // Either part alone plays (unlike the MBase 11's, neither needs the other left up).
    CHECK (brightness (pulse, 0, 960) > 2.0 * brightness (none, 0, 960));
    CHECK (brightness (noise, 0, 960) > 2.0 * brightness (none, 0, 960));
    // The noise sounds for the gate: Gate Time 0.7 is 3.5 ms; its last trace is gone 1 ms after.
    const double gate = 0.1 * std::pow (160.0, 0.7) / 1000.0;
    std::vector<float> diff (noise.size());
    for (size_t i = 0; i < noise.size(); ++i)
        diff[i] = noise[i] - none[i];
    const size_t end = (size_t) ((gate + 0.001) * 48000);
    CHECK (energy (diff, end, diff.size()) < 1e-6 * energy (diff, 0, end));
    CHECK (energy (diff, (size_t) (0.5 * gate * 48000), (size_t) (gate * 48000)) > 0.2 * energy (diff, 0, (size_t) (0.5 * gate * 48000)));
    // The gate is open for Gate Time to the frame (the VCA is held as long): the click's fall starts on the first
    // frame after it (before, the pulse only sinks slowly through its coupling high pass).
    for (auto [rate, knob] : { std::pair { 48000.0, 0.0 }, std::pair { 48000.0, 0.3 }, std::pair { 44100.0, 0.5 }, std::pair { 96000.0, 0.2 } })
    {
        const Settings click { { "Attack", 1.0 }, { "Pulse", 1.0 }, { "Noise", 0.0 }, { "EQ", 0.0 }, { "Pitch", 0.0 }, { "Tune", 0.0 },
                               { "Volume", 0.3 }, { "Gate Time", knob } };
        const auto x = kick (click, { { 0 } }, 0.05, rate);
        const size_t open = (size_t) std::lrint (0.1 * std::pow (160.0, knob) * 0.001 * rate);
        CHECK (x[open - 2] - x[open - 1] < 0.1f * (x[open - 1] - x[open]));
    }
    // A short gate puts less into the envelope: quieter, as on the hardware.
    const auto shortGate = kick (clean ({ { "Pitch", 1.0 }, { "Gate Time", 0.0 } }), { { 0 } }, 0.3);
    const auto longGate = kick (clean ({ { "Pitch", 1.0 }, { "Gate Time", 1.0 } }), { { 0 } }, 0.3);
    CHECK (peak (shortGate, 0, 2000) < 0.75f * peak (longGate, 0, 2000));
}

void testMetalNoise()
{
    const Settings attackOnly { { "Attack", 1.0 }, { "Pulse", 0.0 }, { "Noise", 1.0 }, { "EQ", 0.0 }, { "Gate Time", 1.0 }, { "Volume", 0.4 } };
    auto with = [&] (Settings more) { Settings s = attackOnly; s.insert (s.end(), more.begin(), more.end()); return kick (s, { { 0 } }, 0.1); };
    const auto quiet = with ({ { "Noise", 0.0 } });
    // Both generators off: Noise has nothing to play.
    CHECK (same (quiet, with ({ { "Metal Noise 1", 0 }, { "Metal Noise 2", 0 } })));
    // Metal Noise 2 with no bit inside the mask is silent; numbers that differ only above the mask sound the same.
    CHECK (same (quiet, with ({ { "Metal Noise 1", 0 }, { "Metal Noise 2", 256 }, { "Noise 2 Mask", 8 } })));
    CHECK (same (with ({ { "Metal Noise 1", 0 }, { "Metal Noise 2", 5 }, { "Noise 2 Mask", 8 } }),
                 with ({ { "Metal Noise 1", 0 }, { "Metal Noise 2", 5 + 256 }, { "Noise 2 Mask", 8 } })));
    CHECK (! same (with ({ { "Metal Noise 1", 0 }, { "Metal Noise 2", 5 }, { "Noise 2 Mask", 8 } }),
                   with ({ { "Metal Noise 1", 0 }, { "Metal Noise 2", 5 + 256 }, { "Noise 2 Mask", 9 } })));
    // From M09 up every mask is its own sound, and the wider the mask the slower the pattern (fewer edges).
    {
        std::vector<std::vector<float>> masks;
        for (int m = 9; m <= 16; ++m)
            masks.push_back (with ({ { "Metal Noise 1", 0 }, { "Metal Noise 2", 77 }, { "Noise 2 Mask", (double) m } }));
        for (size_t i = 0; i < masks.size(); ++i)
            for (size_t j = i + 1; j < masks.size(); ++j)
                CHECK (! same (masks[i], masks[j]));
        auto edges = [&] (const std::vector<float>& x) { std::vector<float> d (x.size()); for (size_t i = 0; i < x.size(); ++i) d[i] = x[i] - quiet[i]; return brightness (d, 0, 760); };
        CHECK (edges (masks.back()) < 0.5 * edges (masks.front()));
    }
    // M01 with an odd number: the counter's lowest bit, a 10 kHz square.
    const auto m01 = with ({ { "Metal Noise 1", 0 }, { "Metal Noise 2", 1 }, { "Noise 2 Mask", 1 }, { "Tune", 0.0 }, { "Pitch", 0.0 } });
    CHECK (amplitudeAt (m01, 48000, 10000.0, 48, 720) > 10.0 * amplitudeAt (m01, 48000, 7000.0, 48, 720));
    // Every Metal Noise 1 setting is its own sound (a few, against each other), and white noise is not a pattern.
    std::vector<std::vector<float>> sounds;
    for (int n : { 1, 2, 3, 100, 337, 499 })
        sounds.push_back (with ({ { "Metal Noise 1", n } }));
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        CHECK (! same (sounds[i], quiet));
        for (size_t j = i + 1; j < sounds.size(); ++j)
            CHECK (! same (sounds[i], sounds[j]));
    }
    // Both on: they add up (louder than either).
    const double one = energy (with ({ { "Metal Noise 1", 337 }, { "Metal Noise 2", 0 } }), 0, 800);
    const double both = energy (with ({ { "Metal Noise 1", 337 }, { "Metal Noise 2", 77 }, { "Noise 2 Mask", 12 } }), 0, 800);
    CHECK (both > one);
}

// Tune, Decay, Pulse, Noise, Attack and both metal noises are taken at the hit; Pitch, Harmonics, EQ, Compression and
// Volume move while it sounds.
void testTakenAtTheHit()
{
    const Settings base = clean ({ { "Pitch", 1.0 }, { "Decay", 0.7 }, { "Attack", 0.5 }, { "Pulse", 0.5 }, { "Noise", 0.5 } });
    const auto plain = kick (base, { { 0 }, { 30000 } }, 1.0);
    for (auto [name, v] : { std::pair { "Tune", 0.9 }, std::pair { "Decay", 0.1 }, std::pair { "Pulse", 1.0 }, std::pair { "Noise", 0.0 },
                            std::pair { "Attack", 0.0 }, std::pair { "Metal Noise 1", 300.0 }, std::pair { "Metal Noise 2", 50.0 } })
    {
        Instance a;
        a.setup (base);
        const auto x = a.play ({ { 0 }, { 30000 } }, 1.0, 128, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            if (10000 >= at && 10000 < at + n)
                e.param ((uint32_t) (10000 - at), a.id (name), v);
        });
        CHECK (same (plain, x, 0, 30000));      // the hit under way goes on as it began
        CHECK (! same (plain, x, 30000, 48000)); // the next takes it
    }
    for (auto [name, v] : { std::pair { "Pitch", 0.2 }, std::pair { "Harmonics", 0.8 }, std::pair { "EQ", 0.9 }, std::pair { "Compression", 0.5 },
                            std::pair { "Volume", 0.1 } })
    {
        Instance a;
        a.setup (base);
        const auto x = a.play ({ { 0 }, { 30000 } }, 1.0, 128, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            if (10000 >= at && 10000 < at + n)
                e.param ((uint32_t) (10000 - at), a.id (name), v);
        });
        CHECK (same (plain, x, 0, 10000) && ! same (plain, x, 10000, 20000));
    }
}

// --- Playing it -----------------------------------------------------------------------------------------------------

void testSplitAndPitchMode()
{
    const Settings base = clean ({ { "Pitch", 0.5623 } });
    auto with = [&] (Settings more, int key) { Settings s = base; s.insert (s.end(), more.begin(), more.end()); return kick (s, { { 0, key } }, 1.0); };
    // Any Note: every key plays the kick as set.
    const auto c1 = with ({}, 36);
    CHECK (same (c1, with ({}, 0)) && same (c1, with ({}, 100)));
    // C1: only C1.
    CHECK (same (c1, with ({ { "Split Mode", 1 } }, 36)));
    CHECK (peak (with ({ { "Split Mode", 1 } }, 37), 0, 48000) == 0.0f);
    // C1-C2 in semitones: C2 an octave up; outside the range, nothing.
    CHECK_NEAR (frequency (with ({ { "Split Mode", 2 } }, 48), 48000, 4800, 40000), 100.0, 0.3);
    CHECK_NEAR (frequency (with ({ { "Split Mode", 2 } }, 43), 48000, 4800, 40000), 50.0 * std::pow (2.0, 7.0 / 12.0), 0.3);
    CHECK (peak (with ({ { "Split Mode", 2 } }, 49), 0, 48000) == 0.0f && peak (with ({ { "Split Mode", 2 } }, 35), 0, 48000) == 0.0f);
    // The other ranges: their lowest key plays Pitch, their ends play, past them nothing.
    for (auto [mode, low, high] : { std::tuple { 3, 48, 60 }, std::tuple { 4, 60, 72 }, std::tuple { 5, 36, 96 }, std::tuple { 6, 36, 65 } })
    {
        CHECK (same (c1, with ({ { "Split Mode", mode } }, low)));
        CHECK (peak (with ({ { "Split Mode", mode } }, high), 0, 4800) > 0.0f);
        CHECK (peak (with ({ { "Split Mode", mode } }, high + 1), 0, 48000) == 0.0f);
        CHECK (peak (with ({ { "Split Mode", mode } }, low - 1), 0, 48000) == 0.0f);
    }
    // Linear: equal steps in Hz, a semitone's at the bottom.
    CHECK_NEAR (frequency (with ({ { "Split Mode", 2 }, { "Pitch Mode", 0 } }, 48), 48000, 4800, 40000), 50.0 * (1.0 + 12.0 * (std::pow (2.0, 1.0 / 12.0) - 1.0)), 0.3);
    CHECK_NEAR (frequency (with ({ { "Split Mode", 2 }, { "Pitch Mode", 0 } }, 37), 48000, 4800, 40000), 50.0 * std::pow (2.0, 1.0 / 12.0), 0.2);
}

void testDynamicsAndPlay()
{
    const Settings base = clean ({ { "Pitch", 1.0 } });
    auto at = [&] (Settings more, double velocity) { Settings s = base; s.insert (s.end(), more.begin(), more.end()); return kick (s, { { 0, 36, velocity } }, 0.2); };
    // Dynamics 100 %: velocity is the level; 0: every hit at full level.
    CHECK_NEAR (peak (at ({}, 0.5), 0, 9600) / peak (at ({}, 1.0), 0, 9600), 0.5, 0.01);
    CHECK (same (at ({ { "Dynamics", 0.0 } }, 0.3), at ({ { "Dynamics", 0.0 } }, 1.0)));
    CHECK_NEAR (peak (at ({ { "Dynamics", 0.5 } }, 0.2), 0, 9600) / peak (at ({}, 1.0), 0, 9600), 0.6, 0.01);
    // MIDI notes as CLAP notes do, velocity over 127.
    {
        Instance a;
        a.setup (base);
        const auto viaMidi = a.play ({}, 0.2, 128, stopped, [] (uint64_t t, uint32_t, Events& e) { if (t == 0) e.midi (0, 0x90, 36, 127); });
        CHECK (same (viaMidi, at ({}, 1.0)));
    }
    // Play: a hit at full velocity, the pitch as set; it reports itself released.
    Instance a;
    a.setup (base);
    OutEvents out;
    std::vector<float> l, r;
    a.render (9600, 128, l, r, stopped, [&] (uint64_t t, uint32_t, Events& e) { if (t == 0) e.param (0, a.id ("Play"), 1.0); }, &out);
    CHECK (same (l, at ({}, 1.0)));
    CHECK (out.values.size() == 1 && out.values[0].first == a.id ("Play") && out.values[0].second == 0.0);
    CHECK (a.value ("Play") == 0.0);
}

// Two hits on one frame (two pads struck together, a pad over the sequence: Melodee gives all of a block's MIDI its
// first frame) are one hit: the louder plays as if it had come alone, its key, pitch and level with it, whichever came
// first (equal, the later). A frame apart, both play.
void testSameFrame()
{
    // A step on a frame, in order: a note (velocity > 0), a choke (velocity 0) or a parameter's value.
    struct Step { uint64_t frame; int key; double velocity; const char* param = nullptr; double value = 0.0; };
    auto hit = [] (uint64_t f, double velocity, int key = 36) { return Step { f, key, velocity }; };
    auto choke = [] (uint64_t f, int key) { return Step { f, key, 0.0 }; };
    auto set = [] (uint64_t f, const char* name, double v) { return Step { f, 0, 0.0, name, v }; };
    auto run = [] (const Settings& s, const std::vector<Step>& steps, uint32_t block) {
        Instance a;
        a.setup (s);
        std::vector<float> l, r;
        a.render (9600, block, l, r, [] (uint64_t at) { return Transport { true, 120.0, (double) at * 2.0 / 48000.0 }; },
                  [&] (uint64_t at, uint32_t n, Events& e) {
                      for (const auto& st : steps)
                          if (st.frame >= at && st.frame < at + n)
                          {
                              const uint32_t t = (uint32_t) (st.frame - at);
                              if (st.param)
                                  e.param (t, a.id (st.param), st.value);
                              else
                                  e.note (t, st.velocity > 0.0 ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_CHOKE, (int16_t) st.key, st.velocity);
                          }
                  });
        return l;
    };
    auto with = [] (std::vector<Step> a, const std::vector<Step>& b) { a.insert (a.end(), b.begin(), b.end()); return a; };

    // A busy patch: the attack and a metal noise, LFO 1 retriggered on Pitch, LFO 2 a One Shot on the metal noise, the
    // Accent CV. The pair lands on frame 2560 (a block's first at 128), from silence and over a kick still sounding.
    const Settings busy { { "Noise", 0.6 }, { "Attack", 0.7 }, { "Gate Time", 0.6 }, { "Metal Noise 1", 300 }, { "LFO 1 Int", 0.3 },
                          { "LFO 2 Int", 0.2 }, { "LFO 2 Mode", 2 }, { "LFO 2 Dest", 1 }, { "CV 1 Dest", 16 }, { "CV 1 Amt", 60 },
                          { "CV 1 In", 2.5 } };
    Settings split = busy;
    split.push_back ({ "Split Mode", 2 }); // C1-C2 in semitones: C2 an octave up
    constexpr uint64_t f = 2560;
    for (const std::vector<Step>& before : { std::vector<Step> {}, std::vector<Step> { hit (0, 0.8) } })
    {
        const auto loud = run (busy, with (before, { hit (f, 1.0) }), 128);
        // The ghost after the accent, or before it: the accent alone, whatever the block size (1: a block a frame).
        for (uint32_t block : { 1u, 37u, 128u, 4096u })
        {
            CHECK (same (loud, run (busy, with (before, { hit (f, 1.0), hit (f, 0.3) }), block)));
            CHECK (same (loud, run (busy, with (before, { hit (f, 0.3), hit (f, 1.0) }), block)));
        }
        for (uint32_t block : { 37u, 128u, 4096u })
        {
            auto play = [&] (const Settings& s, const std::vector<Step>& steps) { return run (s, with (before, steps), block); };
            CHECK (same (loud, play (busy, { hit (f, 0.3), set (f, "Play", 1.0), hit (f, 0.5) }))); // Play: full velocity
            // The ghost's key: neither its pitch nor a choke of it.
            const auto low = play (split, { hit (f, 1.0, 36) });
            CHECK (same (low, play (split, { hit (f, 1.0, 36), hit (f, 0.3, 48) })));
            CHECK (same (low, play (split, { hit (f, 0.3, 48), hit (f, 1.0, 36) })));
            CHECK (same (play (split, { hit (f, 1.0, 48) }), play (split, { hit (f, 1.0, 36), hit (f, 1.0, 48) }))); // equal: the later
            CHECK (same (loud, play (busy, { hit (f, 1.0, 40), hit (f, 0.3, 41), choke (f + 2000, 41) })));
            CHECK (same (loud, play (busy, { hit (f, 0.3, 41), hit (f, 1.0, 40), choke (f + 2000, 41) })));
            // A knob moved between the two: the louder takes the knobs as they are at it, as if the softer had not come
            // (nothing of the softer stays: not its LFO retrigger); a hit the Accent CV left softer is the one left out.
            CHECK (same (play (busy, { set (f, "Pitch", 0.9), hit (f, 1.0) }), play (busy, { hit (f, 0.3), set (f, "Pitch", 0.9), hit (f, 1.0) })));
            CHECK (same (play (busy, { set (f, "LFO 1 Mode", 0), hit (f, 1.0) }), play (busy, { hit (f, 0.3), set (f, "LFO 1 Mode", 0), hit (f, 1.0) })));
            CHECK (same (play (busy, { hit (f, 1.0), set (f, "CV 1 In", 0.0) }), play (busy, { hit (f, 1.0), set (f, "CV 1 In", 0.0), hit (f, 1.0) })));
            // A choke between them: the hit after it plays.
            CHECK (same (play (busy, { choke (f, -1), hit (f, 0.3) }), play (busy, { hit (f, 1.0), choke (f, -1), hit (f, 0.3) })));
        }
    }
    // A frame apart (across a block's edge at 128), both play: the second restarts the kick at its own level.
    const Settings plain = clean ({ { "Pitch", 1.0 } });
    const auto one = run (plain, { hit (f, 1.0) }, 128);
    const auto loudThenGhost = run (plain, { hit (f - 1, 1.0), hit (f, 0.3) }, 128);
    const auto ghostThenLoud = run (plain, { hit (f - 1, 0.3), hit (f, 1.0) }, 128);
    CHECK (loudThenGhost[f - 1] != 0.0f && ghostThenLoud[f - 1] != 0.0f);
    CHECK_NEAR (peak (loudThenGhost, f, 9600) / peak (one, f, 9600), 0.3, 0.01);
    CHECK_NEAR (peak (ghostThenLoud, f, 9600) / peak (one, f, 9600), 1.0, 0.01);
    for (uint32_t block : { 1u, 37u, 4096u })
        CHECK (same (loudThenGhost, run (plain, { hit (f - 1, 1.0), hit (f, 0.3) }, block))); // whatever the block size
}

// A choke (Melodee's all sound off, a cut group) stops the hit its key played, on its frame; another key's does not.
void testChoke()
{
    const Settings base = clean ({ { "Pitch", 1.0 } });
    auto run = [&] (int playKey, int chokeKey, bool play = false) {
        Instance a;
        a.setup (base);
        std::vector<float> l, r;
        a.render (24000, 128, l, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            if (at == 0)
            {
                if (play) e.param (0, a.id ("Play"), 1.0);
                else e.note (0, CLAP_EVENT_NOTE_ON, (int16_t) playKey, 1.0);
            }
            if (4803 >= at && 4803 < at + n)
                e.note ((uint32_t) (4803 - at), CLAP_EVENT_NOTE_CHOKE, (int16_t) chokeKey, 0.0);
        });
        return l;
    };
    const auto plain = run (40, 99);
    CHECK (peak (plain, 4803, 24000) > 0.0f);
    for (auto [key, choke, play] : { std::tuple { 40, 40, false }, std::tuple { 40, -1, false }, std::tuple { 0, -1, true } })
    {
        const auto x = run (key, choke, play);
        CHECK (same (plain, x, 0, 4803) && peak (x, 4803, 24000) == 0.0f);
    }
    CHECK (same (plain, run (40, 36)));   // another key's choke
    CHECK (same (plain, run (0, 0, true))); // Play's hit has no key: only a choke of any key (-1) stops it
}

void testLfos()
{
    const Settings base = clean ({ { "Pitch", 1.0 } });
    auto with = [&] (Settings more, const std::vector<Hit>& hits, double seconds, const std::function<Transport (uint64_t)>& transport = stopped) {
        Settings s = base;
        s.insert (s.end(), more.begin(), more.end());
        Instance a;
        a.setup (s);
        return a.play (hits, seconds, 128, transport);
    };
    const auto plain = with ({}, { { 0 } }, 1.0);
    // Int 0: no LFO, whatever it is set to.
    CHECK (same (plain, with ({ { "LFO 1 Wave", 6 }, { "LFO 1 Speed", 1.0 }, { "LFO 2 Wave", 0 }, { "LFO 2 Mode", 2 } }, { { 0 } }, 1.0)));
    // A square on the pitch, retriggered at 2 Hz: a quarter second up by Int x 2 octaves, then a quarter down.
    const Settings square { { "LFO 1 Wave", 6 }, { "LFO 1 Speed", std::log (20.0) / std::log (400.0) }, { "LFO 1 Int", 0.25 } };
    const auto sq = with (square, { { 0 } }, 0.6);
    CHECK_NEAR (frequency (sq, 48000, 480, 11500), 175.0 * std::sqrt (2.0), 1.0);
    CHECK_NEAR (frequency (sq, 48000, 12500, 23500), 175.0 / std::sqrt (2.0), 1.0);
    // Retrigger: every hit starts it again, so two hits sound alike (the first gone before the second); Free runs on,
    // so they differ.
    Settings shortSquare = square;
    shortSquare.push_back ({ "Decay", 0.3 });
    const auto re = with (shortSquare, { { 0 }, { 20000 } }, 0.8);
    CHECK (same (std::vector<float> (re.begin(), re.begin() + 10000), std::vector<float> (re.begin() + 20000, re.begin() + 30000)));
    Settings free = shortSquare;
    free.push_back ({ "LFO 1 Mode", 0 });
    const auto fr = with (free, { { 0 }, { 20000 } }, 0.8);
    CHECK (! same (std::vector<float> (fr.begin(), fr.begin() + 10000), std::vector<float> (fr.begin() + 20000, fr.begin() + 30000)));
    // One Shot: a Saw Up at 2 Hz runs once and holds its top: half a second after the hit, +Int x 2 octaves.
    const auto once = with ({ { "LFO 1 Wave", 0 }, { "LFO 1 Speed", std::log (20.0) / std::log (400.0) }, { "LFO 1 Int", 0.5 }, { "LFO 1 Mode", 2 } }, { { 0 } }, 1.2);
    CHECK_NEAR (frequency (once, 48000, 26000, 50000), 350.0, 1.0);
    // In step with the song: a square at 1/4 (a cycle a beat, 120 BPM) running Free changes from up to down on each
    // half beat, wherever the hit falls.
    const auto playing = [] (uint64_t at) { return Transport { true, 120.0, (double) at * 2.0 / 48000.0 }; };
    const auto synced = with ({ { "LFO 1 Wave", 6 }, { "LFO 1 Int", 0.25 }, { "LFO 1 Mode", 0 }, { "LFO 1 Clock", 4 } }, { { 3000 } }, 0.9, playing);
    CHECK_NEAR (frequency (synced, 48000, 3300, 11500), 175.0 * std::sqrt (2.0), 1.0);   // up to beat 0.5 (frame 12000)
    CHECK_NEAR (frequency (synced, 48000, 12500, 23500), 175.0 / std::sqrt (2.0), 1.0);  // down to beat 1 (frame 24000)
    CHECK_NEAR (frequency (synced, 48000, 24500, 35500), 175.0 * std::sqrt (2.0), 1.0);
    // A new tempo while it plays keeps it in step with the song from that frame: the same as starting there at that
    // tempo.
    {
        const Settings lfo { { "LFO 1 Wave", 4 }, { "LFO 1 Int", 0.25 }, { "LFO 1 Mode", 0 }, { "LFO 1 Clock", 8 } };
        const double at12800 = 12800.0 * 2.0 / 48000.0;
        const auto faster = with (lfo, { { 25600 } }, 0.8, [&] (uint64_t at) {
            return at < 12800 ? Transport { true, 120.0, (double) at * 2.0 / 48000.0 } : Transport { true, 150.0, at12800 + (double) (at - 12800) * 2.5 / 48000.0 };
        });
        const auto startedFast = with (lfo, { { 25600 } }, 0.8, [&] (uint64_t at) {
            return Transport { at >= 12800, 150.0, at < 12800 ? 0.0 : at12800 + (double) (at - 12800) * 2.5 / 48000.0 };
        });
        CHECK (same (faster, startedFast, 25600));
    }
    // A jump puts it where the song now is: the same as starting there.
    {
        const auto jumped = with ({ { "LFO 1 Wave", 4 }, { "LFO 1 Int", 0.25 }, { "LFO 1 Mode", 0 }, { "LFO 1 Clock", 8 } }, { { 25600 } }, 0.8,
                                  [] (uint64_t at) { return Transport { true, 120.0, at < 12800 ? (double) at * 2.0 / 48000.0 : 7.3 + (double) (at - 12800) * 2.0 / 48000.0 }; });
        const auto there = with ({ { "LFO 1 Wave", 4 }, { "LFO 1 Int", 0.25 }, { "LFO 1 Mode", 0 }, { "LFO 1 Clock", 8 } }, { { 25600 } }, 0.8,
                                 [] (uint64_t at) { return Transport { at >= 12800, 120.0, at < 12800 ? 0.0 : 7.3 + (double) (at - 12800) * 2.0 / 48000.0 }; });
        CHECK (same (jumped, there, 25600));
    }
    // LFO 2 too, and on Metal Noise 1 it moves the noise's pattern (only the attack changes).
    const Settings noisy { { "Attack", 1.0 }, { "Noise", 1.0 }, { "Gate Time", 1.0 }, { "Metal Noise 1", 200 } };
    Settings lfoOnNoise = noisy;
    lfoOnNoise.insert (lfoOnNoise.end(), { { "LFO 2 Dest", 1 }, { "LFO 2 Int", 1.0 }, { "LFO 2 Wave", 0 }, { "LFO 2 Speed", 1.0 } });
    const auto still = with (noisy, { { 0 } }, 0.2), moved = with (lfoOnNoise, { { 0 } }, 0.2);
    CHECK (! same (still, moved, 0, 960) && same (still, moved, 2400, 9600));
    CHECK (! same (plain, with ({ { "LFO 2 Int", 0.3 } }, { { 0 } }, 1.0)));
}

void testCvs()
{
    const Settings base = clean ({ { "Pitch", 0.25 } });
    auto with = [&] (Settings more, double seconds = 0.5) { Settings s = base; s.insert (s.end(), more.begin(), more.end()); return kick (s, { { 0 } }, seconds); };
    // CV 1 at 5 V on Pitch with +64: as turning Pitch from 25 % to 75 %; -64 from 75 % back to 25 %.
    const auto knob75 = with ({ { "Pitch", 0.75 } });
    CHECK (same (knob75, with ({ { "CV 1 Dest", 2 }, { "CV 1 Amt", 64 }, { "CV 1 In", 5.0 } })));
    CHECK (same (with ({}), with ({ { "Pitch", 0.75 }, { "CV 3 Dest", 2 }, { "CV 3 Amt", -64 }, { "CV 3 In", 5.0 } })));
    // Two CVs on one destination add; 0 V, an amount of 0 or Off do nothing.
    CHECK (same (knob75, with ({ { "CV 2 Dest", 2 }, { "CV 2 Amt", 64 }, { "CV 2 In", 2.5 }, { "CV 4 Dest", 2 }, { "CV 4 Amt", 32 }, { "CV 4 In", 5.0 } })));
    CHECK (same (with ({}), with ({ { "CV 1 Dest", 2 }, { "CV 1 Amt", 127 }, { "CV 1 In", 0.0 }, { "CV 2 Dest", 2 }, { "CV 2 In", 5.0 },
                                    { "CV 3 Dest", 0 }, { "CV 3 Amt", 127 }, { "CV 3 In", 5.0 } })));
    // Accent: louder by up to about twice; a negative amount softer.
    const float level = peak (with ({}), 0, 9600);
    CHECK_NEAR (peak (with ({ { "CV 1 Dest", 16 }, { "CV 1 Amt", 64 }, { "CV 1 In", 5.0 } }), 0, 9600) / level, 1.5, 0.02);
    CHECK_NEAR (peak (with ({ { "CV 2 Dest", 16 }, { "CV 2 Amt", -64 }, { "CV 2 In", 5.0 } }), 0, 9600) / level, 0.5, 0.02);
    // The other destinations do what their knob does.
    for (auto [dest, name] : { std::pair { 1, "Tune" }, std::pair { 3, "Decay" }, std::pair { 4, "Harmonics" }, std::pair { 8, "EQ" }, std::pair { 9, "Compression" } })
    {
        Settings knob { { name, 0.75 } }, viaCv { { name, 0.25 }, { "CV 1 Dest", dest }, { "CV 1 Amt", 64 }, { "CV 1 In", 5.0 } };
        CHECK (same (with (knob, 1.0), with (viaCv, 1.0)));
    }
    // A CV on a destination the hit takes at its start waits for the next hit; on Pitch it moves the sound at once.
    for (auto [dest, realtime] : { std::pair { 3, false }, std::pair { 2, true } })
    {
        Instance a;
        a.setup (clean ({ { "Pitch", 0.5 }, { "Decay", 0.5 }, { "CV 1 Dest", dest }, { "CV 1 Amt", 100 } }));
        const auto x = a.play ({ { 0 } }, 0.3, 128, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            if (2000 >= at && 2000 < at + n)
                e.param ((uint32_t) (2000 - at), a.id ("CV 1 In"), 5.0);
        });
        Instance b;
        b.setup (clean ({ { "Pitch", 0.5 }, { "Decay", 0.5 }, { "CV 1 Dest", dest }, { "CV 1 Amt", 100 } }));
        const auto y = b.play ({ { 0 } }, 0.3);
        CHECK (same (x, y) != realtime);
    }
}

void testMidiControllers()
{
    // The hardware's controller map: each sets its parameter, as turning it would, and tells the host.
    struct Cc { int number, value; const char* name; double expected; };
    for (const Cc& cc : { Cc { 100, 127, "Tune", 1.0 }, Cc { 101, 0, "Pitch", 0.0 }, Cc { 102, 64, "Decay", 64.0 / 127.0 }, Cc { 103, 127, "Harmonics", 1.0 },
                          Cc { 104, 10, "Pulse", 10.0 / 127.0 }, Cc { 105, 127, "Noise", 1.0 }, Cc { 106, 0, "Attack", 0.0 }, Cc { 107, 127, "EQ", 1.0 },
                          Cc { 113, 127, "Compression", 1.0 }, Cc { 114, 0, "Gate Time", 0.0 }, Cc { 115, 127, "Metal Noise 1", 499 },
                          Cc { 115, 1, "Metal Noise 1", 1 }, Cc { 115, 2, "Metal Noise 1", 2 }, Cc { 115, 0, "Metal Noise 1", 0 },
                          Cc { 116, 64, "Metal Noise 2", 250 }, Cc { 116, 1, "Metal Noise 2", 1 }, Cc { 116, 0, "Metal Noise 2", 0 },
                          Cc { 120, 5, "Noise 2 Mask", 6 }, Cc { 121, 1, "LFO 1 Int", 1.0 / 127.0 }, Cc { 123, 15, "LFO 2 Wave", 15 }, Cc { 117, 127, "Volume", 1.0 }, Cc { 119, 100, "LFO 1 Wave", 15 },
                          Cc { 120, 1, "Noise 2 Mask", 2 }, Cc { 121, 127, "LFO 1 Int", 1.0 }, Cc { 122, 0, "LFO 1 Speed", 0.0 },
                          Cc { 123, 3, "LFO 2 Wave", 3 }, Cc { 124, 127, "LFO 2 Int", 1.0 }, Cc { 125, 127, "LFO 2 Speed", 1.0 } })
    {
        Instance a;
        OutEvents out;
        std::vector<float> l, r;
        a.render (64, 64, l, r, stopped, [&] (uint64_t, uint32_t, Events& e) { e.midi (10, 0xb3, (uint8_t) cc.number, (uint8_t) cc.value); }, &out);
        CHECK_NEAR (a.value (cc.name), cc.expected, 1e-6);
        CHECK (out.values.size() == 1 && out.values[0].first == a.id (cc.name));
    }
    // All Sound Off, Reset All Controllers and All Notes Off (120, 121, 123 at 0, as Melodee sends them on every
    // channel when it silences a track) move no parameter; All Sound Off stops the kick on its frame.
    {
        Instance a;
        a.setup ({ { "Noise 2 Mask", 12 }, { "LFO 1 Int", 0.4 }, { "LFO 2 Wave", 7 } });
        OutEvents out;
        std::vector<float> l, r;
        a.render (24000, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0)
                e.note (0, CLAP_EVENT_NOTE_ON, 36, 1.0);
            if (at == 9600)
                for (uint8_t ch = 0; ch < 16; ++ch)
                    e.midi (7, (uint8_t) (0xb0 | ch), 120, 0), e.midi (7, (uint8_t) (0xb0 | ch), 123, 0), e.midi (7, (uint8_t) (0xb0 | ch), 121, 0);
        }, &out);
        CHECK (out.values.empty());
        CHECK (a.value ("Noise 2 Mask") == 12 && a.value ("LFO 1 Int") == 0.4f && a.value ("LFO 2 Wave") == 7);
        CHECK (std::fabs (l[9606]) > 0.0f && peak (l, 9607, l.size()) == 0.0f);
    }
    // Another controller does nothing; a controller sounds the same as the knob.
    {
        Instance a;
        OutEvents out;
        std::vector<float> l, r;
        a.render (64, 64, l, r, stopped, [&] (uint64_t, uint32_t, Events& e) { e.midi (0, 0xb0, 7, 100); e.midi (0, 0xb0, 1, 100); }, &out);
        CHECK (out.values.empty());
    }
    Instance a;
    a.setup ({});
    const auto viaCc = a.play ({ { 100 } }, 0.5, 128, stopped, [] (uint64_t t, uint32_t, Events& e) { if (t == 0) e.midi (5, 0xb0, 101, 100); });
    Instance b;
    b.setup ({ { "Pitch", 100.0 / 127.0 } });
    CHECK (same (viaCc, b.play ({ { 100 } }, 0.5)));
}

// A tempo that is infinite, NaN or 0 is no tempo the LFOs can follow (held to 1-1000 BPM, or 120): the output stays a
// number, the kick plays, and it ends in exact silence.
void testBadTempo()
{
    for (double bpm : { (double) INFINITY, -(double) INFINITY, std::nan (""), 0.0, 1.0e9 })
    {
        Instance a;
        a.setup ({ { "LFO 1 Clock", 6 }, { "LFO 1 Int", 0.5 }, { "LFO 1 Mode", 0 }, { "LFO 2 Clock", 3 }, { "LFO 2 Int", 0.5 }, { "Decay", 0.2 } });
        const auto x = a.play ({ { 0 }, { 30000 } }, 1.5, 128, [&] (uint64_t at) { return Transport { true, at < 20000 ? bpm : 120.0, (double) at * 2.0 / 48000.0 }; });
        CHECK (finiteAndBounded (x));
        CHECK (peak (x, 0, 4800) > 0.05f && peak (x, 30000, 34800) > 0.05f);
        CHECK (peak (x, 66000, x.size()) == 0.0f);
    }
}

// Finite, bounded output at random and extreme settings, hit fast and slow, with knobs and controllers moving.
void testBounded()
{
    Instance info;
    std::vector<clap_param_info> all;
    for (uint32_t i = 0; i < info.params->count (info.plugin); ++i)
    {
        clap_param_info p {};
        info.params->get_info (info.plugin, i, &p);
        all.push_back (p);
    }
    std::mt19937 random (7);
    for (int trial = 0; trial < 64; ++trial)
    {
        Settings s;
        for (const auto& p : all)
        {
            if (std::string (p.name) == "Play")
                continue;
            double v = trial == 0 ? p.max_value : trial == 1 ? p.min_value : std::uniform_real_distribution<double> (p.min_value, p.max_value) (random);
            if (p.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round (v);
            s.push_back ({ p.name, v });
        }
        if (trial < 2)
            s.push_back ({ "Split Mode", 0 }); // so the hits play
        std::vector<Hit> hits;
        const int spacing = trial % 3 == 0 ? 211 : 9000;
        for (uint64_t f = 0; f < 48000; f += (uint64_t) spacing)
            hits.push_back ({ f, (int) (36 + (f / (uint64_t) spacing) % 40), 0.2 + 0.8 * ((f / 7) % 5) / 4.0 });
        Instance a;
        a.setup (s);
        const auto x = a.play (hits, 1.2, 128, [] (uint64_t at) { return Transport { true, 174.0, (double) at * 174.0 / 60.0 / 48000.0 }; },
                               [&] (uint64_t at, uint32_t, Events& e) {
                                   if (at % 4096 == 0)
                                       e.midi (0, 0xb0, (uint8_t) (100 + (at / 4096) % 26), (uint8_t) ((at / 128) % 128));
                               });
        CHECK (finiteAndBounded (x));
        if (! finiteAndBounded (x))
            for (size_t i = 0; i < x.size(); ++i)
                if (! std::isfinite (x[i]) || std::fabs (x[i]) > 1.0f)
                {
                    std::printf ("  out of range: trial %d, frame %zu, %.9g, at:\n", trial, i, (double) x[i]);
                    for (auto& [name, v] : s) std::printf ("    %s %g\n", name.c_str(), v);
                    break;
                }
    }
}
// Exercise the extension through the built CLAP, including a host that skips silent blocks.
// A load drops a page press still waiting for the audio thread: Push alone sounds the kick, Push then a preset load
// before the next block stays silent.
void testPushThenLoad()
{
    auto peakAfterPush = [] (bool load) {
        Instance a;
        const auto* page = static_cast<const clap_plugin_melodee_screen*> (a.plugin->get_extension (a.plugin, CLAP_EXT_MELODEE_SCREEN));
        std::vector<float> l, r;
        a.render (128 * 4, 128, l, r, stopped);
        const melodee_screen_input_t push { MELODEE_SCREEN_PUSH, 0, false };
        CHECK (page != nullptr && page->input (a.plugin, &push));
        if (load) CHECK (a.loadPreset ("default"));
        a.render (12000, 128, l, r, stopped);
        float peak = 0.0f;
        for (float v : l) peak = std::max (peak, std::fabs (v));
        return peak;
    };
    CHECK (peakAfterPush (false) > 0.1f);
    CHECK (peakAfterPush (true) == 0.0f);
}

void testScreen()
{
    Instance a;
    const auto* page = static_cast<const clap_plugin_melodee_screen*> (a.plugin->get_extension (a.plugin, CLAP_EXT_MELODEE_SCREEN));
    CHECK (page != nullptr);
    if (!page) return;
    constexpr int stride = 327;
    constexpr uint32_t guard = 0x13472568;
    std::vector<uint32_t> memory (stride * 218, guard);
    melodee_screen_canvas_t c {};
    c.pixels = memory.data();
    c.width = 320; c.height = 218; c.stride = stride; c.now_ms = 1000;
    for (auto& k : c.knobs) k = { -1, false };
    auto draw = [&](bool whole = false) { c.whole = whole; return page->paint (a.plugin, &c); };
    auto input = [&](uint32_t kind, int delta, bool shift = false) {
        melodee_screen_input_t e { kind, delta, shift };
        return page->input (a.plugin, &e);
    };
    auto region = [&](int x, int y, int w, int h) {
        std::vector<uint32_t> pixels;
        for (int j = y; j < y + h; ++j)
            pixels.insert (pixels.end(), memory.begin() + j * stride + x, memory.begin() + j * stride + x + w);
        return pixels;
    };
    CHECK (draw (true) && !draw());
    CHECK (std::count (memory.begin(), memory.end(), guard) == 7 * 218);
    const auto rest = memory;
    const auto globe = region (0, 45, 164, 122);
    const auto pitchCurve = region (164, 45, 156, 61);
    const auto ampCurve = region (164, 106, 156, 61);
    // Every page binds its original triplet. The Play button uses the encoder press.
    for (int n = 0; n < 14; ++n)
    {
        clap_id ids[3];
        page->knobs (a.plugin, ids);
        for (int k = 0; k < 3; ++k)
        {
            clap_param_info info {};
            CHECK (a.params->get_info (a.plugin, n * 3 + k, &info));
            CHECK (ids[k] == (info.id == a.id ("Play") ? CLAP_INVALID_ID : info.id));
        }
        if (n < 13) { CHECK (input (MELODEE_SCREEN_NUDGE, 1)); CHECK (draw()); }
    }
    CHECK (input (MELODEE_SCREEN_NUDGE, 1) && !draw());
    // The last page is above bit 31 in the pending edit mask. A full output list retries.
    input (MELODEE_SCREEN_WHEEL, 1);
    Events empty;
    clap_output_events full { nullptr, [] (const clap_output_events*, const clap_event_header*) { return false; } };
    a.params->flush (a.plugin, &empty.in, &full);
    OutEvents retry;
    a.params->flush (a.plugin, &empty.in, &retry.out);
    CHECK ((retry.values == std::vector<std::pair<clap_id, double>> { { a.id ("CV 4 Dest"), 1.0 } }));
    CHECK (a.loadPreset ("default"));
    for (int i = 0; i < 13; ++i) input (MELODEE_SCREEN_NUDGE, -1);
    CHECK (draw() && memory == rest);
    c.knobs[0] = { 105, false };
    CHECK (draw() && region (0, 45, 164, 122) == globe);
    c.knobs[0].engaged = true;
    CHECK (draw() && memory == rest);
    c.shift = true; CHECK (draw()); c.shift = false; CHECK (draw() && memory == rest);

    const int rescanBefore = rescans, flushBefore = flushes;
    input (MELODEE_SCREEN_WHEEL, 16);
    CHECK (rescans == rescanBefore + 1 && flushes == flushBefore + 1);
    CHECK_NEAR (a.value ("Tune"), 0.625, 1e-6);
    CHECK (draw() && region (164, 45, 156, 61) != pitchCurve && region (164, 106, 156, 61) == ampCurve);
    OutEvents out;
    Events none;
    a.params->flush (a.plugin, &none.in, &out.out);
    CHECK ((out.values == std::vector<std::pair<clap_id,double>> { { a.id ("Tune"), 0.625 } }));
    input (MELODEE_SCREEN_WHEEL, -1, true);
    CHECK_NEAR (a.value ("Tune"), 0.625 - 1.0 / 1024, 1e-6);
    const int paintBefore = paints;
    a.setup ({ { "Gate Time", 1.0 }, { "Compression", 0.5 } });
    CHECK (paints > paintBefore && draw() && region (164, 106, 156, 61) != ampCurve);
    CHECK (a.loadPreset ("default") && draw() && memory == rest);

    auto sound = [&] { return a.play ({}, 0.02); };
    const int wakeBefore = processRequests;
    input (MELODEE_SCREEN_PUSH, 1);
    CHECK (processRequests > wakeBefore);
    std::this_thread::sleep_for (std::chrono::milliseconds (550));
    auto quiet = sound();
    CHECK (peak (quiet, 0, quiet.size()) == 0 && !draw());
    input (MELODEE_SCREEN_PUSH, 1);
    a.params->flush (a.plugin, &none.in, &out.out); // only process consumes an audition
    auto kick = sound();
    CHECK (peak (kick, 0, kick.size()) > 0.1f);
    const int beforeAnimation = paints;
    CHECK (draw() && region (0, 45, 164, 122) != globe && paints > beforeAnimation);
    c.now_ms += 60;
    CHECK (draw()); // two frames into the pulse, without another audio block
    c.now_ms += 200;
    CHECK (draw() && memory == rest);
    const int resting = paints;
    CHECK (!draw() && paints == resting);
    a.setup ({ { "Split Mode", 1 } }); // C1 only
    a.play ({ { 0, 37 } }, 0.01);
    draw();
    CHECK (region (0, 45, 164, 122) == globe);
    a.play ({ { 0, 36 } }, 0.01);
    CHECK (draw() && region (0, 45, 164, 122) != globe);

    c.stride = 319; CHECK (!draw (true)); c.stride = stride;
    CHECK (!page->paint (a.plugin, nullptr) && !page->input (a.plugin, nullptr));
    CHECK (!input (99, 1));
    for (int j = 0; j < 218; ++j)
        for (int x = 320; x < stride; ++x) CHECK (memory[j * stride + x] == guard);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: host PLUGIN.clap OUT_FOLDER\n");
        return 2;
    }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    entry = library ? static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry")) : nullptr;
    if (entry == nullptr || ! entry->init (argv[1]))
    {
        std::printf ("FAIL cannot load %s: %s\n", argv[1], dlerror());
        return 1;
    }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    testDescription();
    testParameters();
    testScreen();
    testPushThenLoad();
    testState();
    testPresets (argv[2]);
    testTiming();
    testPitchAndTune();
    testDecayAndCompression();
    testHarmonicsAndEq();
    testAttack();
    testMetalNoise();
    testTakenAtTheHit();
    testSplitAndPitchMode();
    testDynamicsAndPlay();
    testSameFrame();
    testChoke();
    testLfos();
    testCvs();
    testMidiControllers();
    testBadTempo();
    testBounded();
    entry->deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
