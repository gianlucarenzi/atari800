import struct, os

ATR = "810_Master_Disk_DOS_2.0s_1980_Atari.atr"
OUT = ".dos20"
SEC = 128

def read_sector(f, n):
    f.seek(16 + (n - 1) * SEC)
    return bytearray(f.read(SEC))

def extract_file(f, start, nsec):
    data = bytearray()
    cur = start
    for _ in range(nsec):
        s = read_sector(f, cur)
        nbytes   = s[127]
        next_sec = ((s[125] & 3) << 8) | s[126]
        data += s[:nbytes]
        cur = next_sec
    return data

os.makedirs(OUT, exist_ok=True)

with open(ATR, "rb") as f:
    for name, start, nsec in [("DOS.SYS", 4, 39), ("DUP.SYS", 43, 42)]:
        data = extract_file(f, start, nsec)
        path = os.path.join(OUT, name)
        with open(path, "wb") as o:
            o.write(data)
        print(f"Extracted {name}: {len(data)} bytes -> {path}")
