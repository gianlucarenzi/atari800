# Afurnace: Furnace ZSM Player for Atari 800 PBI VERA

A minimal, efficient ZSM (Furnace music format) player for Atari 800 XL with VERA PBI card.

## Features

- ✓ Plays Commander X16 ZSM music files on VERA PSG (16 channels)
- ✓ Compact (~2 KB player code)
- ✓ Supports loop markers
- ✓ VBI-synchronized playback (50 Hz)
- ✓ Python converter for packaging ZSM + player as PRG/XEX

## Architecture

```
32 KB RAM available on Atari 800 XL (with PBI):

$0000-$00FF   Zero page (128 bytes, shared with system)
$0300-$03FF   Player state ($0300-$003F = PSG channel state)
$2000-$3FFF   ZSM music data (8 KB max)
$3000-$37FF   Player code (2 KB)
$3800-$8000   Free for future expansion
```

## Building

### Requirements
- Python 3.6+
- `ca65` (assembler, from cc65 suite)
- `ld65` (linker)

### Build Player
```bash
ca65 --cpu 6502 -o zsm_player.o player/zsm_player.s
ld65 -t atari -o zsm_player.bin zsm_player.o
```

### Convert ZSM to PRG
```bash
python3 tools/zsm_convert.py --info music.zsm
python3 tools/zsm_convert.py -o music.xex music.zsm -p player/zsm_player.s
```

## Usage in Atari BASIC

```basic
100 REM Load music player + data
110 LOAD "D:MUSIC.XEX"

200 REM Play music (calls $3000 init routine)
210 X = USR(12288)  ; INIT_PLAYER at $3000
220 REM Music now playing via VBI tick handler
230 REM Install VBI hook to call TICK at $3003
```

## Furnace Export

1. Open Furnace (or download: https://github.com/tildearrow/furnace)
2. Create song using **Commander X16 VERA** system
3. Set tempo to 6 (default, = 50 Hz PAL)
4. Compose music with PSG (avoid PCM for now)
5. Export → **ZSM** format
6. Run: `python3 zsm_convert.py -o result.xex music.zsm`

## File Format: ZSM

See `docs/ZSM_FORMAT.md` for detailed spec.

Quick summary:
- Header: "ZSM" + version (3 + 1 bytes)
- Loop offset: 2 LE bytes (0 = no loop)
- Reserved: 2 bytes
- Commands: variable length
  - Bit 7 = 0: Delay (1-127 ticks)
  - Bit 7 = 1: VERA write (register offset, then value)
  - 0x00: End of track

## Limitations (v1)

- PSG only (no PCM streaming yet)
- Single-threaded playback (no pause/seek)
- Loop marker only (one loop point)
- No channel muting/solo yet
- Up to 8 KB ZSM data

## Future Enhancements

- [ ] PCM support (stream from storage)
- [ ] Pause/resume/seek
- [ ] Per-channel muting
- [ ] Tempo adjustment at runtime
- [ ] VGM support
- [ ] Integration with MyPicoDos boot
- [ ] Browser-based ZSM editor

## References

- Furnace: https://github.com/tildearrow/furnace
- ZSM spec: https://github.com/X16Community/x16-emulator/wiki/ZSM-File-Format
- VERA audio: vera_pbi_rom/src/vera_psg.c

## License

GPL-2.0 (compatible with Atari800 emulator and Furnace)
