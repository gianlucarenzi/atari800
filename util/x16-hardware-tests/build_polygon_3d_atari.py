#!/usr/bin/env python3

import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "fx_tests" / "polygon_3d.s"
BIN_OUT = ROOT / "polygon_3d_atari.bin"
XEX_OUT = ROOT / "polygon_3d_atari.xex"
CORE_LOAD = 0x2000
LOADER_LOAD = 0x0800
RUNAD = 0x02E0


def build_core() -> int:
    with tempfile.NamedTemporaryFile(
        prefix="polygon_3d_atari_",
        suffix=".lst",
        dir=ROOT,
        delete=False,
    ) as listing_file:
        listing_path = Path(listing_file.name)

    try:
        subprocess.run(
            [
                "vasm6502_oldstyle",
                "-Fbin",
                "-dotdir",
                "-wdc02",
                "-L",
                str(listing_path),
                str(SOURCE),
                "-D",
                "DEFAULT",
                "-o",
                str(BIN_OUT),
            ],
            cwd=ROOT,
            check=True,
        )

        lines = listing_path.read_text().splitlines()
        for index, line in enumerate(lines):
            if re.search(r"\breset:\s*$", line):
                for follower in lines[index + 1 :]:
                    match = re.match(r"00:([0-9A-Fa-f]{4})\b", follower)
                    if match:
                        return int(match.group(1), 16)
    finally:
        listing_path.unlink(missing_ok=True)
    raise RuntimeError("Unable to locate reset entry point in listing")


def make_loader(entry_point: int) -> bytes:
    low = entry_point & 0xFF
    high = entry_point >> 8
    return bytes(
        [
            0x78,  # sei
            0xD8,  # cld
            0xA9,
            0x00,  # lda #$00
            0x8D,
            0x0E,
            0xD4,  # sta NMIEN
            0x8D,
            0x0E,
            0xD2,  # sta IRQEN
            0x8D,
            0x00,
            0xD4,  # sta DMACTL
            0x8D,
            0x2F,
            0x02,  # sta SDMCTL
            0xAD,
            0x01,
            0xD3,  # lda PORTB
            0x09,
            0x02,  # ora #$02  ; BASIC off
            0x29,
            0xFE,  # and #$FE  ; OS off
            0x8D,
            0x01,
            0xD3,  # sta PORTB
            0xA2,
            0xFF,  # ldx #$ff
            0x9A,  # txs
            0x4C,
            low,
            high,  # jmp reset
        ]
    )


def write_segment(handle, start: int, payload: bytes) -> None:
    end = start + len(payload) - 1
    handle.write(start.to_bytes(2, "little"))
    handle.write(end.to_bytes(2, "little"))
    handle.write(payload)


def build_xex(loader: bytes) -> None:
    core = BIN_OUT.read_bytes()
    with XEX_OUT.open("wb") as handle:
        handle.write(b"\xff\xff")
        write_segment(handle, LOADER_LOAD, loader)
        write_segment(handle, CORE_LOAD, core)
        write_segment(handle, RUNAD, LOADER_LOAD.to_bytes(2, "little"))


def main() -> None:
    entry_point = build_core()
    loader = make_loader(entry_point)
    build_xex(loader)
    print(f"reset=${entry_point:04X}")
    print(f"{BIN_OUT.name} {BIN_OUT.stat().st_size}")
    print(f"{XEX_OUT.name} {XEX_OUT.stat().st_size}")


if __name__ == "__main__":
    main()
