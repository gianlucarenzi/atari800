import os
import sys

# Extract DOS 2.0s system files from a known-good master ATR.
# This is used by the build to avoid committing DOS.SYS/DUP.SYS as separate files.

DEFAULT_ATR = "810_Master_Disk_DOS_2.0s_1980_Atari.atr"
DEFAULT_OUT = ".dos20"
SEC = 128

# DOS 2.0s master layout (for the included ATR):
# - DOS.SYS: starts at sector 4, 39 sectors
# - DUP.SYS: starts at sector 43, 42 sectors
FILES = [("DOS.SYS", 4, 39), ("DUP.SYS", 43, 42)]


def read_sector(f, n: int) -> bytearray:
    f.seek(16 + (n - 1) * SEC)
    return bytearray(f.read(SEC))


def extract_file(f, start: int, nsec: int) -> bytearray:
    data = bytearray()
    cur = start
    for _ in range(nsec):
        s = read_sector(f, cur)
        nbytes = s[127]
        # Forward sector pointer in Atari DOS 2.x sectors.
        next_sec = ((s[125] & 3) << 8) | s[126]
        data += s[:nbytes]
        cur = next_sec
    return data


def main(argv: list[str]) -> int:
    atr = argv[1] if len(argv) >= 2 else DEFAULT_ATR
    out = argv[2] if len(argv) >= 3 else DEFAULT_OUT

    os.makedirs(out, exist_ok=True)

    with open(atr, "rb") as f:
        for name, start, nsec in FILES:
            data = extract_file(f, start, nsec)
            path = os.path.join(out, name)
            with open(path, "wb") as o:
                o.write(data)
            print(f"Extracted {name}: {len(data)} bytes -> {path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
