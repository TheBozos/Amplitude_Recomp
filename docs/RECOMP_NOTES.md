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
- **Render-to-texture.** The game draws into frame buffers it never displays and maps them as textures: in play the
  track's surface (FBP `0x130`, 64 x 64 CT16, and `0x131`, 128 x 256 CT32, drawn each frame), the FreQ's monitor
  (`0x170`), the HUD's messages ("Your energy is dropping"). Only the displayed buffer was drawn, so those textures were
  the local memory's leftovers: the track was coloured noise. `runtime/amp_gs_state.cpp` (`Offscreen`) gives each such
  buffer one of the renderer's auxiliary images (`Material::target`), sized to the buffer (FBW x 64 by what its draws
  reach, powers of two, at least what is sampled), draws its batches first at the flip with a matrix spreading the
  buffer over the screen's coordinates, and a texture whose TBP is the buffer's samples the image (`fromFrame`,
  `source`; FST U / V and STQ scaled to the image). An image is only used for a texture of its own colour depth, and is
  dropped once an image transfer writes into its buffer after it was drawn: the menus draw into buffers whose memory
  later holds uploaded textures (FBP `0xf3` becomes the "DRUMS" label's T8 texture in play).
- **VU0 microprograms.** The game calls two (VCALLMS at pairs 0x156 and 0x1ba, the microcode VIF0 loads from
  `0x3ae240` at boot): the FreQs' bones. The port had not registered them, so the engine's built-in table answered
  with Twisted Metal: Black's VU0 programs: the FreQs were drawn without arms and legs. `runtime/amp_render.cpp`
  registers both with `runtime/amp_vu1.cpp`'s interpreter (as Shinobido does).
- **Render images are freed.** A buffer counts as a frame only when displayed within the last 30 flips (the four last
  displayed kept a loading screen's buffer a frame for ever), and when the 14 images are taken the one drawn least
  lately is given up: after a few menus and arenas the track was noise again.
- **The VU1 programs are compiled.** All of Amplitude's VU1 code is uploaded once at boot, eleven MPGs from the
  executable's data (`0x3ab980`-`0x3ae188`, found with `AMP_VU_SURVEY=1`). `tools/amp_vu1_recomp.py` (Shinobido's
  `shino_vu1_recomp.py` with that list in place of its packet scan) compiles them at build time into 9 images, 1,289
  pairs, none left to the interpreter; `runtime/amp_vu1.cpp` runs an image while its code is what is loaded. In play
  the VU1 went from 12-14 ms a frame to 1.6-2 ms: 115-175 frames a second unpaced, a steady 60 in the window.

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
- **The movie's sound was white noise** (logos, intro, then a noise loop on the title). The player reads the stream 8
  sectors at a time without waiting (`sceCdStRead` mode 0, `0x1afa38`) and gives them to `sceMpegDemuxPss` in as many
  calls as the demux takes, the next one at `buffer + 0x4000 - bytes left` (`0x1af580`): it takes for granted that a read
  brought all 8 sectors (a console's drive fills the stream ring a bank at a time). The engine's ring fills a sector at a
  time and a read brings what is there: after a 7-sector read the player skipped a sector, the sound packet in it was
  lost, and the PCM packets being 4,073 bytes (odd), every sample after it was read a byte off. The port's `sceCdStRead`
  (`runtime/amp_port.cpp`) brings a read without waiting all its sectors or none. The intro now plays its music.
  Found by following the bytes: the IOP buffer (`AMP_TRACE_SIF`), the player's ring at `0x436d88`, the demux's packets.
- **The song select's preview loops were silent.** softFX (SOFTFX_S) mixes the preview in software and writes it into
  core 0's sound data input (bytes `0x4000`-`0x47ff`) by ordinary voice transfers, double-buffered by halves on the
  output IRQ, with no block transfer. The SPU2 plays that area at the output position whatever writes it; the engine's
  mixer played the input only for a block transfer. BozoEngine's `Mixer::setMemoryInput` (branch
  `amp/spu2-memory-input`, opt-in) reads it from sound memory; `runtime/amp_audio.cpp` turns it on.
- **GS colour and fog.** An untextured primitive's colour is now used as the GS uses it (0-255, `Material::directColour`;
  the renderer doubled it as it does a texture's modulation), and PRIM.FGE fogs it toward FOGCOL by the vertices' F
  (`Material::fog`; the translator had left fog out).
- **The white blotches.** The title's and the menus' glows (a beacon's flare, light shapes) were solid white and the
  highlighted menu labels blank. Bisecting the untextured draws of one flip (a capture is deterministic) found one
  call whose batch, drawn even opaque, came out white: the material took TEX0's TFX for an untextured primitive, whose
  TEX0 is whatever texture was set last, here DECAL, and the renderer's DECAL outputs the texel: 1.0 without a texture.
  The texture function is now TEX0's for textured primitives only. Left: a black wedge top left of the main menu,
  large triangles that by the GS's rules are opaque black (a white-to-grey palette, all alpha 0x80, black vertices);
  probably the scene's own dark structure.
- **Texture alpha doubled.** The decoder rescaled texel alpha to 255 (0x80 = opaque) and the renderer's shader scales
  it by 255 / 128 again, as the engine's textures carry the GS's alpha: every partly transparent texel drew at twice
  its strength (the fonts' faint cell outlines, alpha 9 of 128, boxed every glyph and space; the main menu's light
  beams were solid). The decoder now keeps the GS's alpha (DC_Recomp's dc_native.cpp found the same).
- **Memory cards.** The FreQmaker's save writes `BASCUS-97258freqs` (the FreQ `player 1.frq`, its icon) and a new
  boot loads it (SELECT / EDIT Player 1). A played window game keeps its cards in `%APPDATA%\Amplitude_Recomp` (mc0,
  mc1); headless and scripted runs keep fresh cards in memory unless given `--card-dir` (`AMP_MEMORY_CARDS=1`: in
  memory for a played game too). Before, every session started on fresh cards: the FreQ and the progress were lost.
- **Render images followed their buffers.** A buffer drawn in another format than its image had (FBP `0xe8`, a menu's
  16-bit buffer, the training track's 32-bit layer later) kept its 16-bit image, its 32-bit textures were refused it
  and read the local memory: the training track's surface was noise. The image follows the buffer's format and width
  now. And the material cache keys on the set of images (`g_offscreenEpoch`): a material names its image, and one
  reused across an image's change drew into the old one.
- **Every arena and two players.** The first song of Neotropolis, Beat Factory, Metaclouds, Elektro Kore and
  Blastlands plays and scores (autopilot), and Multiplayer > Game with two pads (`--pad-script2`) goes through the
  FreQs of both players, difficulty, power-ups and arena to a two-player song; four players (the multitap, players
  3 and 4's scripts) play too. Remix mode goes through its menus and the remix editor (sections, effects); Duel (two FreQs, the
  AMP meters, catch phrases), Practice (from the song select's Square), Freestyle (after a song),
  Options (game settings) and Credits work. Online goes as far as its FreQ select and backs out to the main menu
  cleanly (the network itself is out of scope).
- **Save states.** A state held the engine's parts and the sound modules but not the port's: its DMA register file
  and MFIFO ring (set once at boot), VIF and the VUs (the microcode is sent once at boot), the GS registers and local
  memory (textures uploaded once), the IPU streams. A loaded state ran on into a renderer that never drained its ring:
  the game waited for ever. They are now nested in the `amp` section (`serializeRender`, `serializeVu`, `serializeGs`,
  `serializeIpu`); what is derived from them (decoded textures, render-to-texture images, the frame's batches) starts
  again after a load. Checked in a song and in the FreQmaker: the game runs on, the picture and the sound as in an
  unbroken run. A state written before this has none of it and is refused, with the reason on the screen and in the
  log ("written by an older build"); the game is put back as it was. Loaded anyway, it stopped a few frames on, and
  rebuilding the ring's registers from the game's set-up (`0x210bb8`, the renderer at `0x43ba00`) was not enough: the
  VIF, VU1 and GS state are missing too. Pad scripts after a load count the game's frames, as the state left them.

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
| `AMP_TRACE_SIF=<most>` | the SIF DMA transfers into the IOP's memory (address, size, first bytes) |
| `AMP_TRACE_TARGETS=1` | the render-to-texture images and each flip's draws by frame buffer |

A pad script that reaches "Boom" from boot (with `AMP_UNLOCK_ALL=1`; the arena list opens on Blastlands):
`6470:START/6,6650:CROSS/6,6900:CROSS/6,7000:CROSS/6,7100:CROSS/6,7300:CROSS/6,7500:CROSS/6,7800:CROSS/6,9000:DOWN/6,9150:DOWN/6,9400:CROSS/6,9800:CROSS/6,10200:CROSS/6`.
The song plays from about frame 10,800 to 22,800.

## Native renderer (in progress)

Goal: Harmonix's draws answered at their own functions, their meshes, materials and matrices drawn through
BozoEngine's 3D path instead of the VIF1 / VU1 / GIF packets (`runtime/amp_native.cpp`). Mapped so far:

- Classes (RTTI names in the executable): RndMesh, RndMultiMesh, RndMat, RndTex, RndCam, RndEnviron, RndLight,
  RndTransformable, RndDrawable, RndParticleSys, RndText / RndFont, RndLine, RndMovie, RndView, RndBlur, RndArena.
- Time in a song (AMP_SAMPLE): VU1 image 8 25 %, VIF unpacks and GS packets ~20 %; the renderer's EE code is
  `0x210000`-`0x216000`. VU1 programs: `0x3a78` (2 M runs in the survey), `0x31f0` (0.8 M), `0x2ff0`, `0x2d40`, `0x2890`,
  `0x1ee0`.
- VU1 image 8 (`0x31f0`): a batch header (xtop) into vf19 (the clip guard's scale), vf20-23 (the 4 x 4 matrix to clip
  space), vf13 (colour scale), vf14 / vf15 (the viewport's scale and offset, times w), vf16-18 (the texture transform),
  a GIF tag and a jump target (vi13); then 4 quadwords a vertex (position, then the outputs XYZ / RGBA / STQ written
  over it: colour in `+2`, UV in `+3`), `xgkick`, a clipping path at `0x3a68`.
- The PS2 mesh's draw `0x215f30` (vtable `0x3d2a70` slot 5; `this` is the mesh): `0x232b30` (visible?),
  `0x215d18` (skinning: the bones at `+0x148` blend the bind pose `+0x160`, 32 bytes a vertex, into the vertices at
  `+0xe8`, 64 bytes a vertex, with VU0 program `0x156`; returns the world transform), then for each of the material's
  passes (the material at `+0x118`: `0x213698`, the default `0x213570`) the header and packet `0x2155b0`. The header's
  matrix is the global `0x43c200` (the camera's view-projection) times the world transform (`0x2933b0`, a 4 x 4
  multiply); more header words from `0x43c180`-`0x43c340` and `0x3b07e0`.
- A mesh object (166 drawn in a song frame; `AMP_TRACE_MESHES=<flip>` prints them): vtable pointers at `+0x14`,
  `+0xc8`, `+0xdc`, `+0x18c`, `+0x1a4` (multiple inheritance); the local transform `+0x30` and the world one `+0x70`
  (4 x 4); the material `+0x118`; the geometry owner `+0x140` (itself for a plain mesh); `+0x164` a buffer built once by
  `0x214150` and its size in quadwords `+0x16c`. Static meshes keep no CPU-side vertices (`+0xe8` empty): the
  geometry is in what `0x214150` built.
- `0x214150`'s packet (`+0x154` clear; set, it points at the vertices with REF tags instead): VIF1 codes in batches of
  up to 64 vertices, each an UNPACK V3-16 of the faces (each vertex's quadword offset, after the vertices), a V4-32 of
  the counts at `0x15` (vertices, faces), the vertices from `0x16` (STCYCL 4 / 2 V3-32 for the position and the
  normal, then 4 / 1 V4-32 for the colour, floats 0-1, and V2-32 for u v), then MSCAL `0x63e` (the first batch) or
  MSCNT. The mesh's own faces are at `+0xf8` (three u16 each), its vertices at `+0xe8` (64 bytes: the same four).
- A pass (`0x2155b0`) adds to the scratchpad's packet (its position at `0x70000008`): NOPs and an UNPACK V4-32 of the
  21-quadword header to TOPS, then a REF of the mesh's packet. The header: vf19, vf20-23, vf13 (128), vf14 / vf15
  (screen = vf14 * clip / w + vf15; fog = vf14.w * w + vf15.w, 0-255), vf16-18, the GIF tag (PRE: PRIM `0x7b`;
  ST RGBAQ XYZF2), the clipped fans' tag, vi13 / vi14 / vi15, then vf25-31 (left as they were for unlit passes).
- vi13 is image 8's colour sub-program, run before the transform over the batch's vertices: `0x260` three directional
  lights (vf26-28 the directions as columns, vf29-31 their colours, vf25 the ambient; vi15 multiplies the lights by
  the vertex colour, vi14 the ambient; clamped to 1), `0x373` the ambient alone (or times the vertex colour, vi14),
  `0x3d7` none, `0x6c` one point light at (vf26.x, vf27.x, vf28.x): vf29 times (L - P) . N times (vf26.w + 1 / d)
  within d^2 <= vf27.w, with the same vi14 / vi15 choices. The colour is then times vf13, `ftoi0`.
- A mesh whose packet points at its vertices (`+0x154`: the skinned ones, the FreQs) frees its faces too (`+0xf8`
  empty): its packet, copied into the scratchpad by channel 9 (`0x212500`) and sent at once by the flush `0x211128`,
  is a DMA chain of CNTs (the VIF codes) and REFs (each vertex's 4 quadwords from `+0xe8`, as the skinning left them).

`AMP_NATIVE=1` draws image 8's passes natively (`runtime/amp_native.cpp`): the hook on `0x2155b0` reads the header
from the scratchpad, reads the packet as VU1 would take it, lights and colours the vertices, cuts the triangles to the
clip space's depth (-w <= z <= w, as VU1's clipper does; past it the GS Z wraps), then puts a mark (a VIF NOP, NUM
`0xa5`) before the header's UNPACK and empties the packet's REF. Where the VIF reaches the mark, the GS translator
draws the triangles with the pass's matrix (to the window, XYOFFSET taken off) and the GS state the material set
(`gs::nativeTriangles`), as batches of their own. A pointer packet is taken at its flush instead: its chain read for
the triangles, then made one CNT of NOPs with the mark first. Against the GS path, the same frames but for triangle
edges: the arena selection 0.14 % of the pixels apart by more than 32, the training song 0.11 %. Left to VU1: about
one pass in 4000, whose header is not where expected. Not checked against VU1 yet: `0x6c`'s point light (no scene
compared uses it).

PsMultiMesh (RndMultiMesh's PS2 class; its draw `0x21a4d0`, by `0x21aa40` at vtable `0x3d2d48` slot 3) draws one mesh
at each of its instances with VU1 program `0x3dc` (`0x1ee0`): 93 % of what VU1 did in a song once the meshes were
native (the tunnel's grid). Its header is 19 quadwords: vf16-19 the camera's clip matrix, vf23 the colour's scale,
vf24 / vf15 the viewport's scale and offset, vf20-22 the texture's transform, the GIF tag, vi12 / vi14 the lighting
sub-program and its switch (vi15 is not in it), vf25-31 the lights. Then the mesh's first batch (`0x2161b0`: the packet
`+0x164`, `+0x174` quadwords without its MSCAL, the instance block at `+0x176`; a pointer packet is copied into the
scratchpad by `0x212500` instead), then chunks of instances, each flushed with MSCAL `0x3dc` or MSCNT: a count and each
instance's x, y, z axes and position (REFs to the instance list's nodes at `+0x10`). The program lights and maps the
vertices once, then draws them through clip matrix times instance, with F 0. Native: the first flush reads the header
and the batch, each chunk's flush its instances (transformed on the CPU, one batch with the camera's matrix), and the
chunk's MSCAL / MSCNT becomes the mark. In the training song no VU1 program `0x3dc` runs; the frames match as before
(0.13 % of the pixels apart by more than 32, at edges). What VU1 still runs, by start: in the song `0x2890` (MSCAL
`0x512`, built at `0x21a124`) and `0x2d40` / `0x2ff0` (MSCAL `0x5a8`, the second packet builder `0x214710`: the
packet `+0x168`, the faces `+0x108`); on the arena selection `0x2d40` / `0x2ff0` run 22 million pairs in 40 frames.

A mesh's line pass (`0x2159b0`, from the mesh's draw) is VU1 program `0x5a8` (`0x2d40`, MSCNT `0x2ff0`): the
packet at `+0x168` (`0x214710`: the lines, pairs of u16 at `+0x108`; `+0x16e` quadwords) with an 8-quadword header
(vf28-31 the clip matrix, the GIF tag, vf26 / vf27 the viewport with the fog's terms in w, vf25 one colour as integers).
A batch: the counts at 8 (vertices, lines), the positions from 9 (two quadwords a vertex), the lines after them
(V2-16). The program does not clip a line: it drops it when either end's clip flags (that test and the one before it,
as FCGET gives them) are set, and drops a batch whose vertices all share a flag. Native: as the mesh pass (the REF
emptied or the pointer packet's copy made a CNT of NOPs, the mark), each kept line a quad one GS pixel tall in GS
coordinates, as the GS path draws a LINE (the renderer's own lines are one window pixel at any resolution: thinner
than the game's). On the arena selection, where it ran 22 million pairs in 40 frames, no VU1 program but `0x512` runs;
the frames match as before (0.18 % apart by more than 32; the song 0.13 %).

A particle system's draw (`0x219f28`, by `0x21a2a8`) is VU1 program `0x512` (`0x2890`): each particle a SPRITE. The
header is 9 quadwords (vf28-31 the clip matrix, vf25 the colour's scale, the GIF tag, vf26 / vf27 the viewport, vf24 the
corners' offset in clip space), then chunks of up to 162 particles, each flushed with MSCAL `0x512` or MSCNT: a count
and each particle's colour (`+0x00`, floats) and position (`+0x20`, w half its size `+0x58`), from the list at `+0xf4`
(next `+0x68`). The corners are the centre in clip space less and plus vf24 times the half size, both projected with
the first corner's 1 / w (so a particle keeps its perspective size); a particle with either corner outside the clip
space is dropped; ST 0, 0 (Q 0) and Q, Q (Q), F 0. Native: the list read when the draw starts, the header at the
first flush, each chunk's sprites made on the CPU (two triangles in GS coordinates, as the GS path draws a SPRITE) and
the chunk's MSCAL / MSCNT made the mark. With it the GS path's own sprites were found wrong: a sprite takes its Q from
its second vertex (as its colour), but the translator gave each corner its own, the particles' first corner Q 1
against the second's 1 / w, and their glow texture was drawn bent toward one corner (the blasters' glows off-centre).
Now both paths draw them centred; in the training song and on the arena selection VU1 runs only a few image-8 passes
(their header not where expected), and the frames match as before (0.12 %, 0.18 % of the pixels apart by more than 32).
`AMP_TRACE_PARTICLES=<n>`: the n-th particle draw's particles, header and first sprites.

**Where the native drawing is answered now: the VU1 program's start.** The hooks above (`0x2155b0`, `0x2159b0`,
`0x21a4d0`, `0x219f28`, the flush `0x211128`) left about one pass in 2000 to VU1 from boot to a song (319 headers not
found, 217 REFs not found, 20 particle draws), all for one reason: the runtime may make guest code leave a port's
override midway (an event due) and run the rest of the function later, outside the hook, with the chain the hook
expected cut or sent by someone else. The programs themselves do not have that problem: at MSCAL / MSCNT
(`startProgram`, `runtime/amp_vu1.cpp`) the VIF has unpacked the batch into VU1's data memory, so `native::program`
reads the header and the batch where the microcode would (from the start's TOPS), draws them with the code above
(the colour sub-programs, the depth clip, the line quads and their clip flags, the sprites), and leaves the unit's
continuation where the program's end would (`0x3a78`, `0x2ff0`, `0x2340`, `0x2a78`). The header is kept from the MSCAL
for the MSCNT batches, as the microcode keeps it in registers; a multi-mesh's mesh is lit once at its MSCAL and its
MSCNTs read only their instance blocks. An MSCAL not read here runs on VU1 with its MSCNTs. The chain rewriting, the
marks and the function hooks are gone. From boot into the training song: 4.2 million batches drawn this way and one
program start left to VU1 (`0x2840`, MSCAL `0x508`, six pairs at boot); the frames match the GS path as before (the
training song 0.12 %, the arena selection 0.18 % of the pixels apart by more than 32, at edges).

Compared further (states saved on the GS path, loaded with `AMP_NATIVE` off and on): the training song with its tutorial
panels (0.11 % of the pixels apart by more than 32) and Neotropolis's results screen over the arena (0.18 %). Two fixes
came of the arena:
- Image 8's clipper (`0x3588`, taken when a vertex is outside the guard band) drops a triangle with any vertex past +z
  (z > |w|) before it clips the rest against -z. Clipped at both planes instead, the native path drew thin spikes of
  the arena's near scenery that the console does not (1.86 % of the pixels apart).
- The point light (`0x6c`) is the arenas' (2106 batches in 40 frames of Neotropolis; once in the whole run to the
  training song): its software-pipelined microcode's clip-flag tests and Q timing were not read exactly, and the
  magenta panels came out lit differently. The sub-program is now run on VU1 alone over the batch
  (`vif::vu1Lighting`: vf25-31, vi14, vi15 from the header, vi2 at image 8's end), and the native path takes the
  colours it leaves. The other sub-programs are native.
`AMP_NATIVE_OFF=<MSCAL start>,...` leaves programs to VU1 (with their MSCNTs): how the spikes were found to be image 8's.
To reach an arena's song: `AMP_UNLOCK_ALL=1` from boot and the song script's Accepts (`build/pad_song.txt`, then CROSS
at 11000-11600); pad presses after a state load take the game's frame numbers.

Further modes, from states saved on the GS path: the FreQmaker (0.30 % of the pixels apart by more than 32: a finely
textured part's texels, sampled a little apart) and four players in Neotropolis (0.48 %: the busy scene's edges; the
first frame after a load is a moment apart in the two runs and is left out). Speed, 600 frames of the training song
unpaced (`AMP_UNPACED=1`, the window hidden), twice each: 337 frames a second on the GS path, 353 native (the VU1
programs were compiled to C++ already; the native path saves their run and the GIF translation of their output).

Two players (Game, Neotropolis) and Remix compared as well: 0.5 % and 0.3-0.45 % of the pixels apart by more than 32,
at edges (a Remix frame 23 % apart is the two runs a moment apart: the track's beat bar further along). The way there
is a state saved at Multi's number of players (boot: `6470:START, 6650:DOWN, 6750:CROSS, 6950:CROSS`, state at 7200),
then, for two players, CROSS at 7220, 7420, 7670, 7870, 8170, 8570, 8970, 9370 and 9770 with `--pad-script2
build/pad_p2.txt`; for Remix, TRIANGLE at 7220, DOWN at 7450 and 7550, CROSS at 7650, 7850, 8100 and on every 300-400
frames, with player 2's CROSS at 8150, 8250 and 8500 (the FreQ select waits for both players).

The native renderer is on by default since these comparisons; `AMP_NATIVE=0` gives the GS path back (VU1 runs the
programs, the translator draws their kicks) for comparing.

(`0x3a78`, the survey's busiest start before, is image 8's MSCNT: its later batches.) The 2D is not a path of its own: the text, the HUD and the menus are meshes (RndText's glyph quads, the panels) drawn
by image 8 like the 3D, so they are native already. With the native renderer on, the GS translator draws 5 primitives
in a song frame and 2 on the arena selection, all untextured sprites: clears of the frame, of FBP `0xd4` / `0xdc` /
`0xe8` / `0xf3` (render-to-texture images) and the like. They come from the camera's and the renderer's own methods
(PsCam `0x2128f0`, the renderer's vtable `0x3d2848`); the GIF tags of every packet go through one helper, `0x2121b0`,
whose other callers are the materials' texture uploads (`0x213698` -> `0x213af0` -> `0x218fe0` -> `0x2176xx`-`0x217be0`)
and PsMovie's draw (`0x37ced0`). The movies' pictures are image transfers into the frame (the backdrop). **Sound timing (in progress).** The port's sound reaches the speakers about 55-60 ms after the game mixed it: BozoEngine's
queue target (32 ms) and SDL's device buffer (512 frames, 10.7 ms), Windows' own mixer, and half a game frame on average
(the mix is pushed once a frame). The game knows of a lag: the config's `synth` section has `lag_ms` 25 (the compiled
DTA `gen/freq2_config.txt.bin`: a value follows its symbol, `volume_smoothing` 0.8, `lag_ms` 0x19), read by
SynthPS2's set-up `0x24d2e0` into the synth (`0x64fc50` in a run) at `+0x54` (getter `0x24d2d8`, vtable `0x3d4268`
slot 13). But only the menu music's transitions read it (`0x197224`, `0x197334`: TransitionMusic.cpp's LagRamp, which
reads its own `lag_ms` too): up to a song, the gameplay never asks for it. So the gems and the hits are judged on the
game's clock with no lag allowed for, and what is heard comes 55-60 ms after. `ClockSyncTask` (`0x25c660`-`0x25c728`,
vtable `0x3d499c`) is the online play's clock sync (the player's name, the network objects at `0x3b0af0`), not the
sound's. The song's clock is the task manager's (the global at `0x436218`, its poll `0x2827f0`): its timer at `+0x28`
(`0x436240`) gives milliseconds from the EE's cycle counter (`0x282c70`: COP0 Count accumulated at `0x3b2260`, the
cycles a millisecond at `0x3b2258`), not from the sound; the poll keeps them at `+0x18` (beats at `+0x1c` through the
tempo), the time the tasks reach at `+0x10` (`0x436228`, the HUD's copy `0x43b734`: 16.683 ms a frame, an NTSC field).
The gems, the judgement and the HUD all follow it. The audio offset (`amp_options`, the Amplitude tab's slider,
`AMP_AUDIO_OFFSET`, 50 ms by default) is taken off that timer's answer (an override on `0x282c70` for the timer at
`0x436240` only): with 50, the clock at a given frame reads 50 ms less (28261.3 against 28211.3 at frame 13730 of the
training song) and the song plays on as before. Whether 50 is right on a given PC is the player's ear (the slider).
**Widescreen and the player's options** (`runtime/amp_options.*`, the window's Amplitude tab
`runtime/app/amp_settings_ui.cpp`). The camera is PsCam (`0x2128f0`, vtable `0x3d2950` slot 3, the camera selected at
`*0x3b0988`): it builds the view-projection `0x43c200` from its projection (`+0x160`) and world (`+0x120`), and
keeps six frustum planes in its own space at `+0x2c0`, which a drawable's visibility test (`0x232b30`: its bounding
sphere, `0x290f90`, then a size cut) checks. Widescreen: each native program start drawing into a displayed frame
(`gs::drawsToFrame`) scales its clip matrix's x by 3/4 (the particles' corner offset too), so the screen's mapping and
the clip tests after follow; the visibility test runs with the planes' x scaled the same way (renormalised, put back
after), so the scenery past the 4:3 edges is drawn; the picture is shown 16:9 (`bozo::app::setPictureAspect`, a
BozoEngine addition) and a movie's backdrop stays 4:3 in the middle. The HUD and menus are meshes under an
orthographic camera: scaled the same way, they stay 4:3 in the middle (their black bands end at the 4:3 edges).
Render-to-texture images are not widened. Sharper effects: those images drawn at twice their GS size
(`setTargetSize`; the draws' mapping is in GS pixels). The cheats moved from `amp_main` to `amp_options` (live,
undone when turned off: the flags they set put back to 0).


## Where it stands

The game boots through the logos and the intro movie (with its music) to the title screen, the menus, the FreQmaker,
the arena and song selection, and **a whole song, scored, to the results screen**, at 60 frames a second with the VU1
programs compiled. Seen so far: the title screen
is silent after the intro, and the intro's last 27 seconds or so are black while its music plays on (both as before
the stream fix; not compared with the console yet).

## Next steps

1. The title's sound against the console; the main menu's black wedge against the console.
2. Timing: input and sound against the beat, compared with the console.
3. Native draws: the renderer answered at its own functions instead of through VIF1 / VU1 / GIF.
4. Upstream to BozoEngine: the IRX translator fix and the mixer's output IRQ are merged; still a native `sceIoctl`
   status poll and `sceSifInitCmd`, the libmpeg defaults (TMB's reference layout, the heap after `.bss`) that break
   other games, and a stream ring filled by banks (`sceCdStRead`).
