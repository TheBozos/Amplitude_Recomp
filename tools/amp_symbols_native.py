"""amp_symbols_native.py <symbols.txt> <guest names.txt> <out.txt>: the symbol map without the names the game's own code
keeps (recomp/amp_guest_names.txt): the map bozo-externals reads, so those functions are not answered natively."""
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    guest = {line.split('#', 1)[0].strip() for line in Path(sys.argv[2]).read_text(encoding='utf-8').splitlines()} - {''}
    kept = [line for line in Path(sys.argv[1]).read_text(encoding='utf-8').splitlines()
            if not line.split() or line.split()[0] not in guest]
    Path(sys.argv[3]).write_text('\n'.join(kept) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
