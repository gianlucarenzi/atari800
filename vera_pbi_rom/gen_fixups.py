#!/usr/bin/env python3
"""Generate the fixup table for the relocatable VERA.SYS body.

The body is linked twice at adjacent base addresses (delta = $0100).
Wherever the two binaries differ, the byte is the HI half of an internal
absolute pointer; the LO half sits at offset-1. Emit a stream of LE 16-bit
offsets to the LO byte of each such pointer, terminated by $FFFF.

IMPORTANT — source-code constraint for the body:
  Do NOT use the `#<symbol` / `#>symbol` immediate-byte pattern with INTERNAL
  symbols. With a page-aligned build-delta, only HI bytes change, so a
  `lda #>internal_sym` produces a diff byte whose "LO partner" is actually
  the preceding opcode. At runtime the relocator would add delta_lo to the
  opcode and corrupt it whenever MEMLO is not page-aligned. Use absolute
  reads from a relocatable 16-bit data slot instead (e.g. read from
  __VERA_EXPORTS__).

Usage:
    gen_fixups.py BODY_BASE.bin BODY_PLUS100.bin out_fixups.bin
"""

from __future__ import annotations

import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2

    a_path = Path(argv[1])
    b_path = Path(argv[2])
    out_path = Path(argv[3])

    a = a_path.read_bytes()
    b = b_path.read_bytes()

    if len(a) != len(b):
        print(
            f"fatal: build sizes differ ({a_path}={len(a)} vs {b_path}={len(b)})",
            file=sys.stderr,
        )
        return 1

    hi_offsets: list[int] = []
    for i in range(len(a)):
        if a[i] == b[i]:
            continue
        delta = (b[i] - a[i]) & 0xFF
        if delta != 0x01:
            print(
                f"fatal: byte {i:#06x} differs by {delta:#04x}, expected 0x01 "
                f"(a={a[i]:#04x} b={b[i]:#04x}). "
                "Build base delta must be $100 and only HI bytes should change.",
                file=sys.stderr,
            )
            return 1
        hi_offsets.append(i)

    # Each HI byte at offset i has its LO partner at offset i-1.
    lo_offsets = [h - 1 for h in hi_offsets]
    for lo in lo_offsets:
        if lo < 0:
            print(f"fatal: HI byte at offset 0 has no LO partner", file=sys.stderr)
            return 1

    payload = bytearray()
    for off in lo_offsets:
        payload.append(off & 0xFF)
        payload.append((off >> 8) & 0xFF)
    payload.append(0xFF)
    payload.append(0xFF)

    out_path.write_bytes(payload)
    print(
        f"gen_fixups: {len(lo_offsets)} fixups, "
        f"{len(payload)} bytes → {out_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
