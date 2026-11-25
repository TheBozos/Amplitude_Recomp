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

**Where it stands:** the first frame ends (the movie player's vsync wait, `sceGsSyncV`). The run then stops at
`0x21b248`, the game's own display set-up: it writes the GS privileged registers directly (`PMODE`, `SMODE2`,
`DISPFB1/2`, `DISPLAY1/2`, `BGCOLOR`), called through a pointer. That is where the native drawing path starts.

## Next steps

1. Drawing: answer the display set-up `0x21b248` (the display registers the native renderer presents with), then
   find the game's frame and draw submission (its DMA/VIF1 packets, VU1 microprograms in `.vutext`) and answer it
   natively, as Shinobido and Onimusha did.
2. The movie player `0x1aec48` (libmpeg native: needs FFmpeg, `BOZO_REQUIRE_FFMPEG`) and the game's own IPU
   decoder ("Movie", `0x2163c0`).
3. Audio: Harmonix's SYNTH_S/SOFTFX_S IRX and the multitrack `.STR` streaming are the core of the game, and the
   largest native piece to write.
