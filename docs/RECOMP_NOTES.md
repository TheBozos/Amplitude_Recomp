# Recompilation notes: Amplitude (NTSC-U, SCUS-97258)

How the port is brought up, every fix and how it was found, and what remains open. Built on
[BozoEngine](https://github.com/TheBozos/BozoEngine) (submodule `BozoEngine/`), following the layout of
Onimusha_Recomp and Shinobido_WOTN_Recomp.

## The disc (SCUS-97258, version 1.00)

- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SCUS_972.58;1`, `VER = 1.00`, `VMODE = NTSC`.

  | File | SHA-1 |
  |---|---|
  | `SCUS_972.58` | `3a2e05f454ae36abc85da31225bc6968d8fb5548` |
  | `GEN/MAIN.ARK` | `f8be4267055d71010d159fe0cf7b928354626d1d` |
  | `IOP/IOPRP260.IMG` | `86ecd5726b7d3548c3472e5ebf74152bd8697aba` |
  | `IOP/SYNTH_S.IRX` | `d4840a8c6b1298cc35f7b297a94a306b18c56f0e` |

- **The ELF is stripped**: no `.symtab`, no DWARF. A GCC link (`.mdebug.eabi64`, `.gcc_except_table`, C++ with
  exceptions; file names `StreamBuffer.cpp`, `LargeInt.c`, `RSA.c`, `libezctl.c` remain in assertion strings).
  Library names come from a symbol map (BozoEngine's `tools/symbolmap/bozo_symbolmap.py`), not from the ELF.
- Sony SDK **2.6.0** libraries: `libgraph`, `libdma`, `libkernl`, `libcdvd`, `libipu`, `libmc`, `libmrpc`, `libpad`,
  `libscf`, `libsdr`, `libusbkb` (all `PsIIlib* 2600`), plus `libmtap`.
- One program header: `0x100000` .. `0x449898` in memory (file `0x335014` bytes), entry `0x100010`.

  | section | address | size |
  |---|---|---|
  | `.text` | `0x100000` | `0x2ab970` |
  | `.vutext` | `0x3ab970` | `0x2aa0` (VU microcode; 13 `.DVP.overlay` sections) |
  | `.data` | `0x3ae480` | `0x28c90` |
  | `.rodata` | `0x3d7180` | `0x5ddd8` |
  | `.sbss` / `.bss` | `0x435080` / `0x435c00` | `0xb50` / `0x113c98` |

- **Code inside `.rodata`**: `0x42df00`..`0x42e860` holds four MMI routines (an IDCT: the constants `0x5a82`,
  `0x539f`, `0x4546` are cosines in Q15, their coefficient tables just before) that the program calls with `jal`
  (12 call sites, to `0x42df00`, `0x42e090`, `0x42e190`, `0x42e490`). `tools/amp_prepare.py` adds a code section for
  the block, so bozo-recomp translates it like `.text`.
- **No code overlays** in the main program. The network setup GUI is separate (`NETGUI/NTGUICD.ELF`,
  `NTGUIDVD.ELF`), only for online play.
- Data: `GEN/MAIN.ARK` (564 MB), a Harmonix ARK archive, version 2, 4,526 entries, its table of contents at the
  start of the file. Songs: `AUDIO/*.STR`, 26 multitrack streams (one per song), likely streamed through `CDVDSTM.IRX`.
- IOP modules on the disc (which ones the game loads is still to be traced): `IOPRP260.IMG`, SIO2MAN, PADMAN, MTAPMAN (multitap: four-player mode), MCMAN, MCSERV, LIBSD, SDRDRV,
  CDVDSTM, and Harmonix's own sound modules `SYNTH_S.IRX` and `SOFTFX_S.IRX`. The network stack (DEV9, SMAP,
  INET, PPP, PPPOE, AN986, NETCNF, EZNCTL/EZNCNF) and USB keyboard (USBD, USBKB) serve online play and chat.

## First recompile (BozoEngine `b7298d5`)

`bozo-recomp game/SCUS_972.58 build/analysis/gen --report build/analysis/report.txt`, symbols discovered from the
code alone:

- **13,651 functions**, 14,687 table entries, 699,998 instructions, 86 files (92 MB of C++), in 1.5 s.
- **0 untranslated instructions**; `--census` decodes every word of the code ranges.
- 3 `jr` the analysis could not resolve, `0x102c18`, `0x1035f0`, `0x1037d8`: libgcc's setjmp/longjmp exception
  paths (`__sjthrow`, `__throw`, `__rethrow`) jumping to a landing pad. They go through the dispatcher, which is
  what they need; a landing pad that is not an entry yet will show up as an unknown-target fault.
- 12 calls to the 4 `.rodata` code targets above. With the prepared ELF (the added section) and the symbol map:
  13,686 functions, no warning.

## Symbol map

`scripts/symbols.sh <reference.elf>...` (Shinobido's recipe): BozoEngine's matcher names the stripped ELF's library
functions from executables that keep their symbols, here Dark Cloud's `SCUS_971.11`, Twisted Metal: Black's
`SCUS_971.01` and Onimusha's `SLUS_200.18` (the player's own discs, never committed). Output: `recomp/amp_symbols.txt`
(names and addresses only), the report in `build/bozo/amp_symbols_report.txt`.

- First pass, no reviewed names: **444 functions** named (exact 319, layout 85, calls 37, drift 2, rpc 1);
  **135 of the engine's 272** natively answered names.
- Found: the kernel syscall stubs, `InitThread`, `sceCdInit`, `scePadInit`/`scePadInit2`/`scePadPortOpen`.
- Missing or ambiguous: most of libmc (their neighbours are laid out differently in this link), `sceSifInitRpc`,
  `sceSifLoadModule` (same-shape candidates), the fileio client, libcdvd's stream functions.
- **Reviewed** (`recomp/amp_symbols_reviewed.txt`, 39 names, each with its evidence): `tools/amp_rpc_names.py` pairs
  every `sceSifCallRpc` caller with its server and function number and looks the pair up in the references (libmc
  `0x80000400`, libcdvd `0x80000592`/`0x80000595`/`0x80000597`, fileio `0x80000001`); the rest by reading the code
  (`tools/amp_disasm.py`, `tools/amp_callers.py`): `InitSys` (called by crt0 before `main` `0x100b50`),
  `sceSifInitRpc`, `sceSifSyncIop`, `sceSifRebootIop`, `sceSifLoadModule`, `sceMcSync`, `sceMcMkdir`, `sceCdMmode`,
  SDK 2.6's interrupt-safe `SyncDCache`/`InvalidDCache`/`EnableIntc`/`DisableIntc`/`EnableDmac`/`DisableDmac` (the
  raw syscalls are `_EnableIntc` etc. in this SDK).
- **libcdvd streaming is linked**: the client `0x315878` (N-command `0x80000595`, function 9: `sceCdStream`) is called
  with mode 5, 1, 3 and 2 by `sceCdStInit`, `sceCdStStart`, `sceCdStStop` and `sceCdStRead`, very likely the songs'
  multitrack streaming.
- With them and the boot's names below: **493 functions**, **178 of the 272** engine names. libmpeg is linked too (`_PES_packet`, `_sliceA0`
  near `0x308b18`/`0x30b758`): the movies.

## Build

`scripts/build_runtime.sh` (under `scripts/bench_run.sh --alone`): `tools/amp_prepare.py` adds the `.rodata` code
section, `bozo-externals` turns the symbol map's library names into stubs (179) and the port's registered addresses into
overrides, `bozo-recomp` writes 32 files (13,686 functions), then `amp-headless` and the `amplitude` window link
against BozoEngine (`b7298d5`). `scripts/run.sh` runs the headless runner; set `PS2X_PAD_NO_HOST=1` in scripted runs.

## Boot

Each headless run stops at the next thing that is not native yet; each stop below was answered and the run moved on.

| Stop | What it was | Answer |
|---|---|---|
| wait in the fileio client (`sceIoctl` `0x31b0b8`) | the game opens files with `O_NOWAIT` (0x8001) and polls `sceIoctl(fd, 1, &status)` until status is no longer 1 (busy): the file layer `0x1af6a0`, the reads `0x28f528`/`0x28f598`/`0x28f6b0` | port override: status 0 (the engine's file calls are done when they return). Candidate for BozoEngine's fileio |
| bind loop in `0x310618` | libusbkb's `sceUsbKbInit`: it binds USBKB.IRX's server `0x80000211` until it appears | the game's keyboard init `0x28b148`, end `0x28b1c8` and per-frame poll `0x28b1f0` do nothing: the keyboard is for online chat only |
| IPU_CMD write in `0x30efb0` | `sceIpuInit` (the IPU reset), from libmpeg's `sceMpegInit` and from the game's "Movie" class set-up `0x2163c0` | libmpeg named (`sceMpegInit` `0x307f28`, `sceMpegCreate`, `sceMpegDelete`, `sceMpegGetPicture`, `sceMpegReset`: native); `sceIpuInit` does nothing |
| VIF1_FBRST write in `0x30ea90` | libgraph's `sceGsResetPath`, and `sceGsSyncPath` `0x30eb30` beside it | named: native |
| GS CSR write in `0x30f1e8` | libgraph's `sceGsResetGraph`, `sceGsGetGParam` `0x30f378`, `sceGsSetDefDispEnv` `0x30f438` | named: native |

Further stops on the way to the title, each answered the same way:

| Stop | What it was | Answer |
|---|---|---|
| the display set-up `0x21b248` | an inlined `sceGsPutDispEnv` of one of two display environments (`+0x00`/`+0x40`): the renderer's flip | the flip (below) |
| DMA register accesses in the renderer | Harmonix's DMA channels and the MFIFO ring (below) | native sites on a port register file |
| bind loop on `0x75433178` (`0x24d360`) | the sound set-up waits for SYNTH_S's RPC server | the sound modules compiled and run (below) |
| syscalls 120 (`sceSifSetDChain`) and 122 (`sceSifGetReg`, from `sceSifInitCmd`) | SIF set-up the engine has nothing to do for | `sceSifSetDChain`, `isceSifSetDChain`, `sceSifInitCmd` do nothing |
| bind loop in `_lf_bind` (`0x253438`) | the network configuration's clients: `sceSifSearchModuleByName("eznetcnf")`, then EZNCNF_S's `0x75499128` and EZNCTL_S's `0x75488909` | online play is out of scope: `0x26c648` and `0x26c8f8` do nothing (their results are ignored) |
| a null command after the intro (`0x282910`) | the task manager's list header (`0x549ea0`, the first heap block) overwritten from frame 467: native allocations on the game's heap | the engine's heap moved (below) |
| IPU register writes in `0x216a18` | the Movie class: the animated textures, decoded on the IPU | native IPU pictures (below) |

## Drawing (bring-up path)

Harmonix's renderer builds its packets in the scratchpad and sends them to RAM with the from-scratchpad channel (8)
into a 512 KB ring, in **MFIFO** mode (the init `0x210bb8`: `D_RBOR`, `D_RBSR = 0x7fff0`, `D_CTRL.MFD = 2`); VIF1
(channel 1, chain mode) drains the ring up to channel 8's `MADR`, started each frame at the ring's write position by
`0x211410`. Each channel is a small object (`+4` base, `+8` cursor, `+0x14` channel) programmed through tables of
register addresses (`0x3b06a0` CHCR, `0x3b06c8` MADR, `0x3b06f0` QWC, `0x3b0718` TADR, `0x3b0740` SADR) by
`0x212328` / `0x212500`; `0x2122e8` waits for STR to clear; `0x211128` waits for room in the ring. The VU0 and VU1
microcode is uploaded at init by VIF0 / VIF1 chains (TTE) from `.vutext`.

`runtime/amp_render.cpp` answers every one of those register accesses as a **native site** (28 of them) on a register
file of the port's: a transfer runs when its CHCR is started, at once. VIF0 / VIF1 codes go to the VU interpreters,
GIF packets to the GS translator, scratchpad copies are copies (into the ring, wrapping, in MFIFO mode), and VIF1 in
MFIFO mode follows the ring's tags up to channel 8's `MADR`. The VIF1 reader, the VU1 interpreter and the GS translator
(GIF packets to native draws, the GS local memory kept for the textures) are Shinobido_WOTN_Recomp's
(`runtime/amp_gs*.cpp`, `amp_vu1.cpp`, `amp_vu.h`); no VU1 program is compiled yet (`runtime/amp_vu1_images.cpp`: the
interpreter runs them all).

- The flip `0x21b248` and the movie player's `sceGsPutDispEnv` publish the frame shown (`gs::flip` with its `DISPFB`
  / `DISPLAY`); the frame buffers are 16-bit, at FBP 0 and 0x46 (640 x 448).
- `sceGsPutDrawEnv` (`0x30fb60`, native in the engine, which drops its packet) is answered by the port: its GIF packet
  (`FRAME`, `ZBUF`, `XYOFFSET`, `SCISSOR`, ...) goes to the translator. Without it every draw went to FBP 0 and half
  the frames were black.
- A picture written into the shown frame buffer by IMAGE transfers (the movie player uploads each decoded picture
  straight into it) becomes the frame's backdrop (`Renderer::setBackdrop`) while its pages were written within the last
  8 flips.

## Movies

- The intro (`AMP.PSS`, after "Sony Computer Entertainment America presents" and the Harmonix logo) plays through the
  engine's native libmpeg (FFmpeg's MPEG-2), the movie player `0x1aec48` uploading each picture. **The engine's
  default movie options are Twisted Metal: Black's reference layout**: they write TMB's fixed addresses and take
  blocks from the engine's heap. The runner sets `referenceLayout = false` (the SDK's layout).
- The engine's heap (native allocations: libmpeg's 32-byte stream callback entries) defaulted to the region after
  `.bss`, where the game's own allocator works: both handed out the same memory, the task manager's list header
  (`0x549ea0`) was overwritten from the movie's first frame, and the task run after the intro had a null command. The
  runner gives the engine 32 KB inside `.text` (`0x101000-0x109000`: recompiled code, never read as data), and
  `_calloc_r` stays the game's own code (`recomp/amp_guest_names.txt`: its malloc and free could not be named).
  libmpeg's SDK demux (`sdkDemux`) stalls this movie (its sound callback is not Shinobido's): left off.
- **The movie's sound is noise** (a WAV of the intro: flat spectrum, left and right uncorrelated): the PCM the movie
  carries is read in the wrong format somewhere. Open.

## IPU pictures: the animated textures

The menus' monitors, the arenas' and the songs' screens are `ipum` streams in MAIN.ARK (`metagame/image/*.ipu`,
`metagame/arena/textures/movieN.ipu`, `songs/<song>/movieN.ipu`: 128 x 128, tens of frames; `tools/amp_ark.py` lists
and extracts the archive), played by the game's "Movie" class on the IPU, several streams taking turns: `0x216a18`
starts a picture (the to-IPU chain `0x21b520` the first time, else the stream's DMA state restored by `0x30ed90`; the
from-IPU destination is the texture's buffer; `0x21b560` reads the picture's flag byte; IDEC with colour conversion to
RGB16), and channel 3's interrupt (`0x216340` -> `0x216990`) hands the picture to the texture, saves the stream's DMA
state (`0x30eca8`) and starts the next waiting stream's picture.

`runtime/amp_ipu.cpp` reads each to-IPU chain into a byte stream once (its `ipum` header gives the size), follows the
current stream through the save / restore calls, answers the flag read, and decodes each IDEC's picture (flag byte to
end code) with FFmpeg's IPU decoder, converted with the IPU's colour conversion to RGB16 in macroblock order at
`D3_MADR`, as the IPU writes it; then it sets `D_STAT`'s channel 3 bit and raises the interrupt (the game masks the
channel while it queues a picture: the kernel delivers it at `EnableDmac` only with that bit set).

## Sound

The game loads Sony's LIBSD, SDRDRV and SCRTCHPD and Harmonix's SYNTH_S (the synthesizer, RPC server `0x75433178`)
and SOFTFX_S ("softFX", `0x75433776`). `tools/amp_bozo_audio.py` relocates and translates all five to C++ with
BozoEngine's native IRX tools (Shinobido's recipe; SCRTCHPD because SOFTFX_S allocates from it), and
`runtime/amp_audio.cpp` runs them on the engine's native IRX host and mixer; the other modules are answered at their EE
APIs (pad, multitap, memory card, CD) or are online-only. Neither Harmonix module reads the disc: the EE streams song
data to them over SIF.

- SOFTFX_S did not compile: a function whose last instruction pair is a call (`0x761c0`) listed the call's return
  address (the next function's start) as its own resume point without its label. Fixed in BozoEngine's
  `tools/native_irx/recompile.py` (branch `amp/irx-call-ends-function`, not merged yet).
- The title screen plays real stereo sound (a WAV of it: correlated channels, a music-like spectrum); not compared with
  the console yet.
- **The streams never refilled** (the tutorial's narration and the songs stopped after their first buffers; the game
  then waited for the stream for ever). SOFTFX_S times its stream player with the SPU2's IRQ: it points a core's IRQ
  address at the output write-back areas (IRQA `0x29f0` / `0x2ff0`, halfword `0x14f8` / `0x17f8`: core 0 and 1's
  output areas, 512 samples each) and counts each time the output reaches it. The engine's mixer only raised the IRQ
  on voice reads. Fixed in BozoEngine's `runtime/audio/mixer.cpp` (`Mixer::reachOutputIrq`: the IRQ is raised when the
  output position, one sample per output frame, reaches an IRQ address inside `0x800..0x4000`).

## Gameplay

The data the songs need come from `GEN/MAIN.ARK` (version 2: five-word entries, offset, name index, directory index
(`0xffffffff` for the root), size, 0; then the string table and its index) and the song streams `AUDIO/*.STR`, which
the EE reads with `sceCdRead` and sends to SOFTFX_S over SIF (opcode `0x69` data, `0x6f` volume); SYNTH_S plays the
MIDI parts (RPC batch command `0x70`, opcode `0x6e`).

- The path: main menu, Solo, a FreQ (the FreQmaker's prefab, a name typed on the on-screen keyboard, saved to the
  memory card), Game, the difficulty, the arena, the song. The arenas are locked by the game's progress
  (`freq2_config`: Training and its first two programs always open; beating Basic Concepts Training opens Neotropolis
  and Advanced Training). The game data object is `*0x440d44` (difficulty `+0x48`, mode `+0x54`, the unlock-all flag
  `+0x8c`, the songs' data array `+0xa8`).
- **Basic Concepts Training** plays with its streamed narration; the lanes' buttons (L1, R1, R2) hit the gems.
- **"Boom" (P.O.D. vs. The Crystal Method, Neotropolis, Normal) plays from start to end**: the track, the gems, the
  HUD (energy, score, multiplier), the arena's animated screens, the song's music from its stream, the "Energy low"
  warning and Game Over without input, the end of the song and the PLAYER STATS screen. With the game's own autopilot
  (below) the score climbs as the gems are played (30 at the end, 17 % of the tracks turned on).
- The energy meter is a float at `+0x4bc` of the song's player object `*0x3af838` (15 full, Game Over at 0); the HUD
  keeps an integer copy at `0x43b74c`.

Play-test switches of the runner (`runtime/amp_main.cpp`), for unattended runs:

| Variable | What |
|---|---|
| `AMP_UNLOCK_ALL=1` | the game's unlock-all flag: every arena and song open |
| `AMP_INFINITE_ENERGY=1` | the energy meter held full: a whole song without input |
| `AMP_AUTOPILOT=1` | the game's autopilot cheat (`+0x100` of `*0x435f14`, its debug toggle `0x110390`): the game plays the gems |
| `AMP_UNPACED=1` | the window runs as fast as it can (captures) |
| `AMP_DUMP_RAM_AT=<frame>,...` | RAM written to `build/ram_<frame>.bin` |
| `AMP_WATCH=<address>,...` | a RAM word printed whenever it changes |
| `AMP_TRACE_FILES=1`, `AMP_TRACE_RPC=1`, `AMP_TRACE_RPC_DUMP=<frame>` | file system calls, the drivers' RPC calls |

A pad script that reaches "Boom" from boot (with `AMP_UNLOCK_ALL=1`; the arena list opens on Blastlands):
`6470:START/6,6650:CROSS/6,6900:CROSS/6,7000:CROSS/6,7100:CROSS/6,7300:CROSS/6,7500:CROSS/6,7800:CROSS/6,9000:DOWN/6,9150:DOWN/6,9400:CROSS/6,9800:CROSS/6,10200:CROSS/6`.
The song plays from about frame 10,800 to 22,800.

## Where it stands

The game boots through the logos and the intro movie to the title screen, the menus, the FreQmaker, the arena and song
selection, and **a whole song, scored, to the results screen**. About 52 frames a second in the window with every VU1
program interpreted. Seen so far: the track's surface drawn as coloured noise (a texture or CLUT case of the
translator), white blotches on the title's scene and over highlighted menu labels, the intro's noisy sound.

## Next steps

1. The track's surface texture; the white blotches; the intro movie's sound.
2. Timing: input and sound against the beat, compared with the console.
3. Speed: compile the VU1 programs (Shinobido's `tools/shino_vu1_recomp.py`), then answer the renderer's draws at its
   own functions instead of through VIF1/VU1/GIF.
4. Upstream to BozoEngine: the IRX translator fix, the mixer's output IRQ; a native `sceIoctl` status poll and
   `sceSifInitCmd`; the libmpeg defaults (TMB's reference layout, the heap after `.bss`) that break other games.
