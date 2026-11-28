# Notes for coding agents

**hdslave MoonBass** is a ModBase 09 / MBase 11 style kick drum with every parameter on Edit pages: a CLAP plug-in for the Akai MPC Sample. It is made to run in hdslaveOS, the hdslave fork of Kerem Kiliç's Melodee. Its CLAP id is `com.hdslave.moonbass`, and it builds to `build/MoonBass.clap`.

## Read the workspace guide first

On the author's machine, this repository sits in one folder beside the other hdslave projects. That folder's `../AGENTS.md` and `../Workflow/` hold the shared workflow:
- building in the Linux VM;
- what hdslaveOS does to plug-ins;
- screen pages;
- reviews;
- publishing, the hub repository and staging for the SD card.

Read `../AGENTS.md`, then `../Workflow/plugins.md` and `../Workflow/host-facts.md`, before you change anything. The rules below hold either way.

## Build and test

- `./build.sh` builds the plug-in in a Linux arm64 container (image `melodee-builder`, Colima VM `melodee`). It runs `make all test`: the host tests in `tests/host.cpp` and the sound's fingerprint. Pass make targets to run others, for example `./build.sh sanitize` (ASan and UBSan over the unit and host tests).
- `tools/bench.sh` counts the instructions of a 128-frame block with callgrind. A block is about 4.8 M cycles on one of the MPC's Cortex-A55 cores.
- Check the build's own exit code: `./build.sh | grep …` hides a failure.

## Rules

- **Fixed for saved projects.** Parameter ids, preset keys and choice orders never change once released, because saved projects and presets refer to them.
  - Add new ones at the end.
  - Ranges stay as they are too: the host stores bindings normalized to the range.
- **The sound is held to the bit** by `tests/fingerprint.txt`.
  - A refactor or speed-up must leave every hash unchanged. With `-ffp-contract=fast`, even reshaping a loop can change the last bit, so check after every edit.
  - Only a change meant to change the sound runs `make fingerprint-update`, and its commit says which hashes moved and why.
- **Real-time and determinism.** The output is the same at every block size. No allocation, locks or file I/O on the audio thread.
- **Notes** must survive how hdslaveOS sends them:
  - every MIDI event of a block arrives at frame 0;
  - a re-pressed key arrives as on, on, off;
  - CC 120 and 123 arrive at 0 on every channel when the host silences.
- **Commits** are made as `hdslave <111106843+hdslave@users.noreply.github.com>`, with Conventional Commits. Never push, publish or stage for the card without the owner's word.
- **Other agents' work.** Others may be working here. Check `git status` and `git worktree list`, and never edit, commit or reset files you did not change.
- **Names and art.** No trademarked names in the plug-in's names, presets, UI text or art. Art is original; fonts are OFL, with their licences in `licenses/`.
- **Read the README** before you change the sound. It lists the sources, the inferred values and the deliberate deviations. Keep it current: version, costs, and "not yet run on the MPC" until it has been.

## This plug-in

- **Latched at the hit on purpose:** Tune, Decay, Pulse, Noise, Attack, the metal noises, the mask and Gate Time, as on the hardware.
- **MIDI CC map.** CC 120, 121 and 123 at value 0 are channel mode, never parameter changes. `NOTE_CHOKE` stops the hit its key played.
- **Pads and banks.** `padMapJson` in `src/moonbass.cpp` is the drum map hdslaveOS reads from the library (pads A01-A16 = keys 36-51, "Kick 1"-"Kick 16"), so − / + move the bank. `Engine::note` keeps every key's 0.1.8 pitch inside the Key Range; a key outside it (silent up to 0.1.8) plays the same-named key an octave in (C1: as C1). There is no bank-as-octave remap: MoonBass has no step pads. The desktop copy in `mac-clap/src` keeps the old range filter.
- **Same-frame triggers.** Two triggers on one frame keep the louder one; on a tie, the later one.
- The parameter count stays a multiple of three (a `static_assert`).
- **The page's shockwave** (`src/ui/shockwave.h`) is drawn from each hit's age alone, so a repeated paint touches nothing and a whole repaint equals a partial one (`testShockwave`). It draws only with the page's inks (`inks` in `src/screen.h`: the art's palette, the page's colours and mixes of two of them). `build/page` (`make page`) renders its frames outside a host; `tools/bench.sh` counts a frame's instructions.

## Release packaging

Every release ZIP includes the full repository `LICENSE.txt` beside the third-party notices, outside signed plug-in bundles. Preserve both when packaging the plug-in for another project.
