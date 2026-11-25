"""amp_ark.py <MAIN.ARK> [list | extract <name> <out file>]: Amplitude's ARK archive (version 2), read for development.

The header: the version (2), the file count, then per file five words (offset, the name's and the directory's indices in
the string index (0xffffffff: none), the size, 0), then the string table (its size, then the NUL-terminated strings), then the string index
(its count, then each string's offset in the table). The files follow. Extracted files are the player's game data: keep
them in build/.
"""
import struct
import sys


def entries(path):
    with open(path, 'rb') as f:
        version, count = struct.unpack('<2I', f.read(8))
        if version != 2:
            raise SystemExit(f'ARK version {version}: expected 2')
        table = [struct.unpack('<5I', f.read(20)) for _ in range(count)]
        size, = struct.unpack('<I', f.read(4))
        strings = f.read(size)
        indices, = struct.unpack('<I', f.read(4))
        index = struct.unpack(f'<{indices}I', f.read(4 * indices))

    def name(i):
        if i >= len(index):  # 0xffffffff: no directory (the root)
            return ''
        start = index[i]
        return strings[start:strings.index(b'\0', start)].decode('latin-1')
    return [(name(d) + '/' + name(n) if name(d) else name(n), off, length) for off, n, d, length, _ in table]


def main():
    path = sys.argv[1]
    files = entries(path)
    if len(sys.argv) == 2 or sys.argv[2] == 'list':
        for name, off, length in files:
            print(f'{off:#010x} {length:#010x} {name}')
    elif sys.argv[2] == 'extract':
        want = sys.argv[3]
        for name, off, length in files:
            if name == want or name.endswith('/' + want):
                with open(path, 'rb') as f:
                    f.seek(off)
                    open(sys.argv[4], 'wb').write(f.read(length))
                print(name, hex(off), hex(length))
                return
        raise SystemExit(f'{want}: not in the archive')


if __name__ == '__main__':
    main()
