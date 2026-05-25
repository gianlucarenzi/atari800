#!/usr/bin/env python3
"""
bundle_vera.py — prepend VERA.SYS installer to a test .COM.

Produces a combined Atari binary whose DOS load sequence is:

  1. All VERA.SYS segments loaded (body $4000, fixups, loader $6000).
  2. INITAD=$6006 segment: DOS loader JSRs to bootstrap_entry, which
     installs the driver into high RAM and returns (RTS).
  3. Test segments loaded (cc65 CRT at $2E00, code at $5000, etc.).
  4. Test INITAD (cc65 CRT init): fires with VERA E: handler already active.
  5. Test RUNAD ($5000): overrides VERA.SYS's RUNAD; DOS JMPs to the test.

bootstrap_entry ends with RTS so the DOS loader continues after step 2.
The test code may overlap $6000+ without risk because the VERA loader is
done before those bytes are written.

Usage: python3 bundle_vera.py VERA.SYS test.COM output.COM
"""

import sys
from pathlib import Path

ATARI_RUNAD      = 0x02E0
ATARI_INITAD     = 0x02E2
VERA_BOOTSTRAP   = 0x6006   # bootstrap_entry address (fixed by vera_loader.cfg)


def parse_segments(data: bytes) -> list[tuple[int, int, bytes]]:
    segs = []
    i = 0
    while i < len(data):
        if i + 1 < len(data) and data[i] == 0xFF and data[i + 1] == 0xFF:
            i += 2
            continue
        if i + 4 > len(data):
            break
        start  = data[i]     | (data[i + 1] << 8)
        end    = data[i + 2] | (data[i + 3] << 8)
        length = end - start + 1
        i += 4
        if length <= 0 or i + length > len(data):
            break
        segs.append((start, end, data[i:i + length]))
        i += length
    return segs


def encode_segment(start: int, end: int, payload: bytes) -> bytes:
    return bytes([start & 0xFF, start >> 8, end & 0xFF, end >> 8]) + payload


def main() -> int:
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} VERA.SYS test.COM output.COM",
              file=sys.stderr)
        return 2

    vera_path = Path(sys.argv[1])
    test_path = Path(sys.argv[2])
    out_path  = Path(sys.argv[3])

    vera_segs = parse_segments(vera_path.read_bytes())
    test_segs = parse_segments(test_path.read_bytes())

    out = bytearray(b'\xFF\xFF')

    # 1. VERA.SYS segments as-is (body, fixups, loader, its RUNAD=$6006)
    for start, end, payload in vera_segs:
        out += encode_segment(start, end, payload)

    # 2. INITAD=$6006: DOS loader calls bootstrap_entry as JSR, installs
    #    the driver, and RTS returns control to the loader.
    out += encode_segment(ATARI_INITAD, ATARI_INITAD + 1,
                          bytes([VERA_BOOTSTRAP & 0xFF, VERA_BOOTSTRAP >> 8]))

    # 3. Test segments (includes cc65 INITAD and the test RUNAD=$5000 which
    #    overwrites VERA.SYS's RUNAD=$6006 as the final jump target).
    for start, end, payload in test_segs:
        out += encode_segment(start, end, payload)

    out_path.write_bytes(bytes(out))

    def seg_list(segs):
        return ','.join(f'${s:04X}-${e:04X}' for s, e, _ in segs)

    print(f"bundle_vera: {vera_path.name}[{seg_list(vera_segs)}] "
          f"+ INITAD=${VERA_BOOTSTRAP:04X} "
          f"+ {test_path.name}[{seg_list(test_segs)}] "
          f"→ {out_path.name} ({len(out)}B)")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
