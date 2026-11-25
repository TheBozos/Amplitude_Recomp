"""amp_prepare.py <SCUS_972.58> <out.elf>: the executable as bozo-recomp should read it.

SCUS_972.58 is stripped but keeps its section table: .text (0x100000-0x3ab970) is the EE code, .vutext the VU microcode
(bozo-recomp leaves it out). One block of EE code sits inside .rodata, 0x42df00-0x42e860: four MMI routines (an IDCT:
the constants 0x5a82, 0x539f, 0x4546 are cosines in Q15, their tables just before at 0x42de00..) that the program
calls with jal (12 call sites, to 0x42df00, 0x42e090, 0x42e190 and 0x42e490). bozo-recomp translates what a code section
covers, so a code section is added for that block. The program header (the load image) is unchanged, and the runner
loads this file. docs/RECOMP_NOTES.md has the evidence.
"""
import hashlib
import struct
import sys
from pathlib import Path

ELF_SHA1 = '3a2e05f454ae36abc85da31225bc6968d8fb5548'  # SCUS_972.58, NTSC-U 1.00
TEXT_SECTION = 1
TEXT_ADDRESS = 0x100000
RODATA_CODE = (0x42df00, 0x42e860)
SH = struct.Struct('<10I')


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    elf = bytearray(Path(sys.argv[1]).read_bytes())
    if hashlib.sha1(elf).hexdigest() != ELF_SHA1:
        print('SCUS_972.58: not the NTSC-U Amplitude executable this port is built against', file=sys.stderr)
        return 1
    shoff = struct.unpack_from('<I', elf, 0x20)[0]
    shnum = struct.unpack_from('<H', elf, 0x30)[0]
    table = [list(SH.unpack_from(elf, shoff + i * 40)) for i in range(shnum)]
    text = table[TEXT_SECTION]
    if text[3] != TEXT_ADDRESS:
        print(f'section {TEXT_SECTION} at {text[3]:#x}, expected .text at {TEXT_ADDRESS:#x}', file=sys.stderr)
        return 1
    # The block's file offset, through the one LOAD segment (vaddr 0x100000 at file offset 0x1000).
    phoff = struct.unpack_from('<I', elf, 0x1C)[0]
    _type, p_offset, p_vaddr, _pa, p_filesz = struct.unpack_from('<5I', elf, phoff)
    if not p_vaddr <= RODATA_CODE[0] < RODATA_CODE[1] <= p_vaddr + p_filesz:
        print('the .rodata code block is outside the load segment', file=sys.stderr)
        return 1
    code = list(text)  # PROGBITS, AX, the name of .text
    code[3], code[4], code[5] = RODATA_CODE[0], p_offset + RODATA_CODE[0] - p_vaddr, RODATA_CODE[1] - RODATA_CODE[0]
    table.append(code)
    # The section table is rewritten at the end of the file with the new section appended.
    del elf[shoff:shoff + shnum * 40]
    elf += bytes((-len(elf)) % 4)
    struct.pack_into('<I', elf, 0x20, len(elf))
    struct.pack_into('<H', elf, 0x30, len(table))
    elf += b''.join(SH.pack(*s) for s in table)
    out = Path(sys.argv[2])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(elf)
    return 0


if __name__ == '__main__':
    sys.exit(main())
