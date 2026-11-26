// The Orbital Impact tile (tile 0) as the desktop panel paints it: whole, every 16 ms while the panel is open. A note
// through process() must set off the hit's animation there and end on the resting picture; a hit while the panel
// was shut (no paint for a quarter of a second) must not play when it opens again. Built with the inner plug-in's
// sources, as the bundle's inner object is.
#include "moonbass.cpp"

#include <cstdio>
#include <vector>

namespace
{
int failures = 0;
void check (bool ok, const char* what)
{
    if (! ok) std::fprintf (stderr, "FAIL: %s\n", what), ++failures;
}

const melodee_screen_host_t screenHost { [] (const void*, uint32_t) {} };
const void* hostExtension (const clap_host_t*, const char* id)
{
    return std::strcmp (id, MELODEE_SCREEN_CLAP_HOST) == 0 ? &screenHost : nullptr;
}
const clap_host_t host { CLAP_VERSION, nullptr, "tile_test", "", "", "1", hostExtension,
                         [] (const clap_host_t*) {}, [] (const clap_host_t*) {}, [] (const clap_host_t*) {} };

clap_event_note pending {};
bool notePending = false;
uint32_t inSize (const clap_input_events_t*) { return notePending ? 1u : 0u; }
const clap_event_header_t* inGet (const clap_input_events_t*, uint32_t) { return &pending.header; }
bool outPush (const clap_output_events_t*, const clap_event_header_t*) { return true; }

struct Tile
{
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_hdslave_tiles_t* tiles = nullptr;
    std::vector<uint32_t> pixels = std::vector<uint32_t> (320 * 218, 0xff0a0c10u);
    float left[256] {}, right[256] {};

    Tile()
    {
        auto* factory = static_cast<const clap_plugin_factory*> (clap_entry.get_factory (CLAP_PLUGIN_FACTORY_ID));
        plugin = factory->create_plugin (factory, &host, "com.hdslave.moonbass");
        plugin->init (plugin);
        plugin->activate (plugin, 48000.0, 1, 256);
        plugin->start_processing (plugin);
        tiles = static_cast<const clap_plugin_hdslave_tiles_t*> (plugin->get_extension (plugin, CLAP_EXT_HDSLAVE_TILES));
    }
    ~Tile()
    {
        plugin->stop_processing (plugin);
        plugin->deactivate (plugin);
        plugin->destroy (plugin);
    }
    void block (bool note)
    {
        float* channels[2] = { left, right };
        clap_audio_buffer_t out {};
        out.data32 = channels, out.channel_count = 2;
        const clap_input_events_t in { nullptr, inSize, inGet };
        const clap_output_events_t outEvents { nullptr, outPush };
        notePending = note;
        pending = { { sizeof (clap_event_note), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0 }, -1, 0, 0, 36, 1.0 };
        clap_process_t process {};
        process.frames_count = 256, process.audio_outputs = &out, process.audio_outputs_count = 1;
        process.in_events = &in, process.out_events = &outEvents, process.steady_time = -1;
        plugin->process (plugin, &process);
        notePending = false;
    }
    // The tile at `ms`, as the panel paints it; the Orbital Impact panel's pixels (0, 45, 164 x 122).
    std::vector<uint32_t> paint (uint64_t ms)
    {
        melodee_screen_frame_t frame {};
        frame.pixels = pixels.data(), frame.width = 320, frame.height = 218, frame.stride = 320;
        frame.area = MELODEE_SCREEN_PAGE, frame.redraw_all = 1, frame.time_ms = ms;
        for (int k = 0; k < 3; ++k) frame.pots[k] = -1;
        clap_id knobs[3];
        tiles->paint (plugin, 0, &frame, knobs);
        std::vector<uint32_t> globe;
        for (int y = 45; y < 167; ++y) globe.insert (globe.end(), pixels.begin() + y * 320, pixels.begin() + y * 320 + 164);
        return globe;
    }
};
} // namespace

int main()
{
    clap_entry.init ("");
    {
        Tile t;
        check (t.tiles != nullptr, "the tiles extension");
        if (! t.tiles) return 1;
        uint64_t ms = 1000;
        std::vector<uint32_t> rest;
        for (; ms < 2000; ms += 16) t.block (false), rest = t.paint (ms);
        t.block (true);
        int moved = 0;
        for (int n = 0; n < 10; ++n, ms += 16) moved += t.paint (ms) != rest;
        check (moved == 10, "a hit animates the Orbital Impact tile from its first frame");
        for (int n = 0; n < 40; ++n, ms += 16) t.paint (ms);
        check (t.paint (ms) == rest, "the hit's animation ends on the resting picture");

        // The panel shut for a second: a hit meanwhile is not played when it opens again.
        ms += 1000;
        t.block (true);
        check (t.paint (ms) == rest && t.paint (ms + 16) == rest && t.paint (ms + 48) == rest,
               "a hit while the panel was shut does not play when it opens");
    }
    clap_entry.deinit();
    std::printf ("tile_test: %d failed\n", failures);
    return failures != 0;
}
