<div align="center">

<img src="docs/branding/amp_banner.png" alt="Amplitude, native PC port by The Bozos" width="100%">

# Amplitude_Recomp

**A native PC port of *Amplitude* (NTSC-U, `SCUS-97258`), made by statically recompiling the game's PlayStation 2 code to C++. Windows first; Linux later.**<br>
Built on [BozoEngine](https://github.com/TheBozos/BozoEngine), The Bozos' own toolchain: the game's EE code recompiled to C++, every library and kernel call it makes answered by native code. No PS2 hardware model at any level: no GS, VU, VIF/GIF, DMA, IOP or SPU2.

![Platform](https://img.shields.io/badge/platform-Windows%20(Linux%20later)-1b8cff)
![Compiler](https://img.shields.io/badge/compiler-GCC%20%7C%20Clang-1b8cff)
![Based on](https://img.shields.io/badge/based%20on-BozoEngine-17a35b)
![Status](https://img.shields.io/badge/status-songs%20play-e0a100)

</div>

> [!IMPORTANT]
> No game files are included: no ELF, IRX modules, archives, audio streams, or code generated from them.
> You need **your own copy** of the North American release, PS2 ID **`SCUS-97258`** (disc version 1.00). The port is
> built against that disc's executable:
>
> | File | SHA-1 |
> |---|---|
> | `SCUS_972.58` | `3a2e05f454ae36abc85da31225bc6968d8fb5548` |

## Status

**A song plays from start to end.** The game boots through the logos and the intro movie to the title screen, the
menus, the FreQmaker, the arena and song selection, and plays a whole song ("Boom", P.O.D. vs. The Crystal Method, in
Neotropolis) with its music streamed from the disc: the gems are hit, the tracks turn on, the score climbs and the
results screen follows. It runs at full speed (60 frames a second, the VU1 programs compiled to C++), the intro movie
with its sound, the track's surface and the HUD's monitor drawn by render-to-texture.

| Milestone | |
|---|:---:|
| Disc layout, executable sections and IOP modules mapped | ✅ |
| Whole program recompiled by bozo-recomp: 13,686 functions, no untranslated instruction | ✅ |
| Library symbol map for the stripped executable (493 names, 178 of the engine's natively answered calls) | ✅ |
| Port build (`amp-headless`, `amplitude`) linked against BozoEngine | ✅ |
| Boot, memory-card check, logos | ✅ |
| Intro movie (native libmpeg) | ✅ |
| Title screen: the 3D city fly-over | ✅ |
| Main menu and Solo menu, with the animated IPU monitors | ✅ |
| Harmonix's sound modules (SYNTH_S, SOFTFX_S) compiled and running on the native mixer | ✅ |
| FreQmaker, arena and song selection, the training with its narration | ✅ |
| A complete song: streamed music, gems, scoring, energy, results screen | ✅ |
| Render-to-texture (the track's surface, the FreQ's monitor, HUD messages) | ✅ |
| Intro movie sound, the song select's preview loops | ✅ |
| FreQs drawn whole (the game's VU0 microprograms) | ✅ |
| Full speed: the VU1 programs compiled to C++ (60 frames a second with room to spare) | ✅ |
| The title's and menus' glows and highlighted labels (were white blotches) | ✅ |
| Input and sound timing checked against the console | ⏳ |
| Save states (in the window: save and load slots) | ✅ |
| Memory-card saves (the FreQs and the progress, in `%APPDATA%\Amplitude_Recomp`) | ✅ |
| Every arena's songs, two players (Multiplayer, Game) | ✅ |
| Four players (multitap), Remix mode | ✅ |
| Native draws (the renderer answered at its own functions, not through VIF1 / GS packets) | ⏳ |
| Linux build | ⏳ |

✅ works · 🔧 in progress · ⏳ not reached yet

Online play (the network GUI and stack on the disc) is out of scope: its servers are long gone.

[docs/RECOMP_NOTES.md](docs/RECOMP_NOTES.md) records the disc, the executable and every step of the bring-up.

## Screenshots

Captured from the current build at 960x720.

| | | |
|---|---|---|
| ![The memory-card check](docs/screenshots/memory-card-check.jpg) | ![Sony Computer Entertainment America presents](docs/screenshots/scea-presents.jpg) | ![The Harmonix logo](docs/screenshots/harmonix-logo.jpg) |
| ![The intro movie](docs/screenshots/intro-movie.jpg) | ![The title screen's fly-over](docs/screenshots/title-flyover.jpg) | ![The title screen](docs/screenshots/title.jpg) |
| ![The main menu](docs/screenshots/main-menu.jpg) | ![Solo: select your FreQ](docs/screenshots/select-freq.jpg) | ![The FreQmaker](docs/screenshots/freqmaker.jpg) |
| ![Select an arena](docs/screenshots/select-arena.jpg) | | |
| ![Select a song](docs/screenshots/select-song.jpg) | ![The warp into the song](docs/screenshots/song-intro.jpg) | ![Boom, in Neotropolis](docs/screenshots/gameplay.jpg) |
| ![Tracks turned on, the score climbing](docs/screenshots/gameplay-score.jpg) | ![Player stats after the song](docs/screenshots/player-stats.jpg) | ![Elektro Kore: Cool Baby](docs/screenshots/gameplay-elektro-kore.jpg) |
| ![Two players](docs/screenshots/gameplay-2p.jpg) | ![Four players (multitap)](docs/screenshots/gameplay-4p.jpg) | ![Remix mode](docs/screenshots/remix.jpg) |

## Goal

A fully playable *Amplitude* as a native PC program: every song, mode and remix, drawing and playing sound directly
from the game's data, with tight audio/input timing first. Widescreen, higher resolutions and other presentation
options follow.

## How it works

```
disc files (game/) ──► scripts/symbols.sh ──► recomp/amp_symbols.txt (library names for the stripped ELF)
                                   │
          BozoEngine's bozo-recomp ──► native C++ in build/ (never committed)
                                   │
            CMakeLists.txt ──► BozoEngine + generated code + Amplitude's own adapters ──► amplitude.exe
```

- `SCUS_972.58` is a stripped GCC build using Sony SDK 2.6 libraries. BozoEngine discovers the functions from the code
  itself; `bozo_symbolmap.py` names the SDK functions by matching them against executables that keep their symbols.
- Game data lives in `GEN/MAIN.ARK`, a Harmonix ARK archive; each song is a multitrack stream in `AUDIO/*.STR`.
- Drawing (bring-up path): Harmonix's renderer sends its packets through a DMA ring (MFIFO) to VIF1 and the VU1. The
  port answers every DMA register access of the renderer natively, runs the VIF1 codes and the VU1 programs (compiled
  to C++ from the executable at build time by `tools/amp_vu1_recomp.py`), and draws the resulting GS packets with
  BozoEngine's renderer, off-screen buffers as render targets (the GS translator and the VU1 compiler come from
  Shinobido_WOTN_Recomp).
- The intro movie is decoded by BozoEngine's native libmpeg (FFmpeg); the menus' animated textures (`ipum` streams the
  game plays on the IPU) by FFmpeg's IPU decoder.
- Sound: Sony's LIBSD, SDRDRV, SCRTCHPD and Harmonix's SYNTH_S and SOFTFX_S are compiled offline from the disc to C++
  and run on BozoEngine's native IRX host and mixer.
- Game-specific addresses, formats and hooks stay in this repository; engine changes go to BozoEngine and stay generic.

## Requirements

- Windows 10/11 with [MSYS2](https://www.msys2.org/) (UCRT64); Linux is not qualified yet
- CMake 3.21+, Ninja, GCC with C++20 support
- FFmpeg development libraries (`libavcodec` with the MPEG-2 and IPU decoders, `libavutil`) through `pkg-config`
- `bsdtar` (libarchive) to extract the disc, Python 3
- Your own copy of `SCUS-97258` (check the hash above)

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,ffmpeg,libarchive,python} git
```

## Build and run

A song plays from start to end. This is the developer workflow as it stands.

```bash
git clone --recurse-submodules https://github.com/TheBozos/Amplitude_Recomp.git
cd Amplitude_Recomp
scripts/extract_iso.sh "/path/to/Amplitude (USA).iso"
scripts/symbols.sh <reference ELFs with symbols>...   # only when the symbol map changes
scripts/bench_run.sh --alone 30 "build" scripts/build_runtime.sh
PS2X_PAD_NO_HOST=1 scripts/bench_run.sh 10 "boot" scripts/run.sh --frames 600
```

`scripts/run.sh` runs `amp-headless`; `AMP_WINDOW_RUN=1` runs the `amplitude` window instead (`--hidden`, `--mute`
for unattended runs; `AMP_CAPTURE_FRAMES=first:last:step` with `PS2X_CAPTURE_WINDOW_DIR` saves pictures). The intro
lasts about 6,200 frames; `--pad-script` takes `frame:BUTTON/frames` entries (e.g. `6500:START/8` at the title). The
runner prints where the game stops and what is not native yet. Play-test switches (`AMP_UNLOCK_ALL`,
`AMP_INFINITE_ENERGY`, `AMP_AUTOPILOT`, `--pad-script2`..`4` for players 2-4) and a pad script that reaches a song are in
[docs/RECOMP_NOTES.md](docs/RECOMP_NOTES.md#gameplay).

Builds and timed runs take the shared benchmark lock alone (`scripts/bench_run.sh --alone`); game runs take one slot.

## Disc facts

- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SCUS_972.58;1`, `VER = 1.00`, `VMODE = NTSC`.
- `SCUS_972.58`: one program header `0x100000`..`0x449898`, entry `0x100010`, stripped (no `.symtab`, no DWARF),
  GCC with C++ exceptions, Sony SDK 2.6 libraries. A few routines live inside `.rodata`.
- `GEN/MAIN.ARK` (564 MB, ARK version 2, 4,526 entries), 26 song streams in `AUDIO/`, IOP modules in `IOP/`
  (including Harmonix's `SYNTH_S.IRX` and `SOFTFX_S.IRX`), the online setup GUI in `NETGUI/`.

## Repository layout

| Path | What |
|---|---|
| `BozoEngine/` | The engine (submodule): recompiler, runtime, native libraries, tools |
| `recomp/` | The symbol map, the names read by hand, the library functions kept as the game's own code |
| `runtime/` | The runner (`amp_main.cpp`), the port's overrides (`amp_port.cpp`), the renderer's DMA and flip (`amp_render.cpp`), the GS translator and VU interpreters (`amp_gs*.cpp`, `amp_vu1.cpp`), the IPU pictures (`amp_ipu.cpp`), the sound modules (`amp_audio.cpp`) |
| `tools/` | `amp_prepare.py` (the ELF as bozo-recomp reads it), `amp_bozo_audio.py` (the sound modules), `amp_vu1_recomp.py` (the VU1 programs), `amp_ark.py` (MAIN.ARK), the naming aids (`amp_disasm.py`, `amp_callers.py`, `amp_rpc_names.py`) |
| `runtime/app/` | The window build's icon, manifest and Windows resources |
| `scripts/` | Disc extraction, symbol map, build, run, shared bench lock |
| `docs/` | [RECOMP_NOTES.md](docs/RECOMP_NOTES.md), the branding (`docs/branding/make_logo.py`) |

## Credits

- **The Bozos**: boubou666 and Arzaroth, who make this port and BozoEngine
  ([RecompWatcher](https://thebozos.github.io/RecompWatcher/) shows our projects).
- [BozoEngine](https://github.com/TheBozos/BozoEngine), The Bozos' MIT recompiler and native runtime.
- The team's other recompilations (*Dark Cloud*, *Twisted Metal: Black*, *Onimusha: Warlords*, *Shinobido*, *The Lord
  of the Rings: The Third Age*, *Valkyrie Profile 2: Silmeria*, *Jade Cocoon 2*), which share native development work.
- *Amplitude* © Sony Computer Entertainment America, developed by Harmonix Music Systems. This is an unofficial fan
  project, not affiliated with or endorsed by them.

## Licence

MIT, see [LICENSE](LICENSE). Game data is never part of this repository: the port builds from the player's own disc.
