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

- **Code inside `.rodata`**: about `0x42df00`..`0x42e490` holds real code (it uses MMI instructions, e.g. `0x70e73c88`)
  that the program calls with `jal`. bozo-recomp reports these as "call target outside every function"
  (4 targets, 12 calls); they need a code range added (or a port-side handler) before the program can reach them.
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
- 3 `jr` the analysis could not resolve (`0x102c18`, `0x1035f0`, `0x1037d8`, all early in `.text`: likely
  switch tables to check by hand).
- 12 calls to the 4 `.rodata` code targets above.

## Symbol map

`scripts/symbols.sh <reference.elf>...` (Shinobido's recipe): BozoEngine's matcher names the stripped ELF's library
functions from executables that keep their symbols, here Dark Cloud's `SCUS_971.11`, Twisted Metal: Black's
`SCUS_971.01` and Onimusha's `SLUS_200.18` (the player's own discs, never committed). Output: `recomp/amp_symbols.txt`
(names and addresses only), the report in `build/bozo/amp_symbols_report.txt`.

- First pass, no reviewed names: **444 functions** named (exact 319, layout 85, calls 37, drift 2, rpc 1);
  **135 of the engine's 272** natively answered names.
- Found: the kernel syscall stubs, `InitThread`, `sceCdInit`, `scePadInit`/`scePadInit2`/`scePadPortOpen`.
- Missing or ambiguous: most of libmc (`sceMcInit`, `sceMcGetInfo`, `sceMcGetDir`, ...: their neighbours are laid out
  differently in this link), `sceSifInitRpc`, `sceSifLoadModule`/`sceSifLoadModuleBuffer` (same-shape candidates),
  `sceCdInitEeCB`. Next: name them by hand in `recomp/amp_symbols_reviewed.txt`, pairing each `sceSifCallRpc` caller
  with its server id and function number (libmc `0x80000400`, libcdvd `0x80000592`/`0x80000597`, libpad
  `0x80000100`), as Shinobido and TTA did.

## Next steps

1. Finish the symbol map (the reviewed names above) and pass it to bozo-recomp/bozo-externals (`--symbols`).
2. Code ranges for the `.rodata` routines; resolve the 3 open `jr`.
3. `tools/amp_prepare.py`, `CMakeLists.txt` and a thin `runtime/` (main, port registry) on Onimusha's model, to
   link `amp-headless` and boot to the first frame.
4. Audio: Harmonix's SYNTH_S/SOFTFX_S IRX and the multitrack `.STR` streaming are the core of the game, and the
   largest native piece to write.
