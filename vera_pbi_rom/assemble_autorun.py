#!/usr/bin/env python3
"""Combine body + fixups + loader into a multi-segment AUTORUN.SYS file.

Reads the linker label files (.lbl, VICE-compatible, produced by ld65 -Ln) to
look up:
  - body  : __EXPORTS_START__, __VCTL_LAST__, __STARTADDRESS__
  - loader: PATCH_BODY_FILE_SIZE, PATCH_BODY_TOTAL_SIZE, PATCH_FIXUP_TABLE,
            bootstrap_entry

The script then:
  1. patches the loader binary with body file size, body total size (incl
     LOWBSS+VCTL), and the runtime fixup table address
  2. emits a DOS .COM file with three load segments and a RUNAD trailer:
       segment 1  $4000 ..             body bytes
       segment 2  $4000+body_size ..   fixup table bytes
       segment 3  $5000 ..             patched loader bytes
       trailer    $02E0..$02E1         bootstrap_entry (RUNAD)

Usage:
    assemble_autorun.py body.bin body.lbl fixups.bin loader.bin loader.lbl out.SYS
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

BODY_SOURCE = 0x4000
LOADER_ADDR = 0x6000
RUNAD = 0x02E0


def parse_lbl(path: Path) -> dict[str, int]:
    """Parse a VICE-compatible .lbl file produced by ld65 -Ln.

    Each line looks like:
        al C:80B6 ._vera_warm_reinit
    """
    # ld65 -Ln emits `al ADDR .SYM`. The VICE convention prefixes the
    # address with `C:`; ld65 may omit it on Atari targets. Accept either.
    pattern = re.compile(r"^al\s+(?:C:)?([0-9A-Fa-f]+)\s+\.(\S+)\s*$")
    symbols: dict[str, int] = {}
    for line in path.read_text().splitlines():
        m = pattern.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        name = m.group(2)
        symbols[name] = addr
    return symbols


def emit_segment(addr: int, data: bytes) -> bytes:
    """Encode one load segment with the Atari DOS .COM 4-byte header."""
    if len(data) == 0:
        return b""
    end = addr + len(data) - 1
    if end > 0xFFFF:
        raise ValueError(f"segment at ${addr:04X} ({len(data)} B) wraps past $FFFF")
    header = bytes([
        addr & 0xFF, (addr >> 8) & 0xFF,
        end & 0xFF,  (end >> 8) & 0xFF,
    ])
    return header + data


def set_word(buf: bytearray, off: int, value: int) -> None:
    buf[off] = value & 0xFF
    buf[off + 1] = (value >> 8) & 0xFF


def main(argv: list[str]) -> int:
    if len(argv) != 7:
        print(__doc__, file=sys.stderr)
        return 2

    (body_path, body_lbl_path, fixups_path,
     loader_path, loader_lbl_path, out_path) = (Path(p) for p in argv[1:])

    body = body_path.read_bytes()
    fixups = fixups_path.read_bytes()
    loader = bytearray(loader_path.read_bytes())

    body_syms = parse_lbl(body_lbl_path)
    loader_syms = parse_lbl(loader_lbl_path)

    # ----- body sizes -----
    # __BODY_START__ / __BODY_LAST__ come from the MEMORY block in vera_sys.cfg
    # (define = yes). __BODY_LAST__ is "one past the last allocated byte" per
    # ld65 semantics, so total_size = __BODY_LAST__ - __BODY_START__.
    try:
        body_start = body_syms["__BODY_START__"]
        body_last = body_syms["__BODY_LAST__"]
    except KeyError as exc:
        print(f"fatal: missing body symbol: {exc}", file=sys.stderr)
        return 1
    body_total_size = body_last - body_start

    body_file_size = len(body)
    if body_file_size > body_total_size:
        print(
            f"fatal: body file size ({body_file_size}) exceeds total "
            f"size ({body_total_size}); BSS handling is off",
            file=sys.stderr,
        )
        return 1

    # ----- loader patches -----
    required = ["PATCH_BODY_FILE_SIZE", "PATCH_BODY_TOTAL_SIZE",
                "PATCH_FIXUP_TABLE", "bootstrap_entry"]
    missing = [s for s in required if s not in loader_syms]
    if missing:
        print(f"fatal: loader.lbl missing symbols: {missing}", file=sys.stderr)
        return 1

    fixup_addr = BODY_SOURCE + body_file_size

    set_word(loader,
             loader_syms["PATCH_BODY_FILE_SIZE"] - LOADER_ADDR,
             body_file_size)
    set_word(loader,
             loader_syms["PATCH_BODY_TOTAL_SIZE"] - LOADER_ADDR,
             body_total_size)
    set_word(loader,
             loader_syms["PATCH_FIXUP_TABLE"] - LOADER_ADDR,
             fixup_addr)

    bootstrap_entry = loader_syms["bootstrap_entry"]

    # ----- emit the .SYS file -----
    out = bytearray()
    out += b"\xFF\xFF"
    out += emit_segment(BODY_SOURCE, body)
    out += emit_segment(fixup_addr, fixups)
    out += emit_segment(LOADER_ADDR, bytes(loader))
    out += emit_segment(RUNAD, bytes([
        bootstrap_entry & 0xFF,
        (bootstrap_entry >> 8) & 0xFF,
    ]))

    out_path.write_bytes(out)
    print(
        f"assemble_autorun: "
        f"body={body_file_size}B (total={body_total_size}B) "
        f"fixups={len(fixups)}B loader={len(loader)}B "
        f"runad=${bootstrap_entry:04X} → {out_path} ({len(out)}B)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
