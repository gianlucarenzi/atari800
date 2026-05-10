# ZSM Format Specification

ZSM is the Commander X16 music format, designed for VERA audio streaming.

## Header (8 bytes)
```
+0: $7A $53 $4D        ; Magic: "ZSM"
+3: Version (1 byte)   ; 0x00 = v1
+4: Loop offset (2 LE)  ; Absolute offset in file where loop starts (0=no loop)
+6: Reserved (2 bytes)  ; $00 $00
```

## Data Section
After header: sequential VERA register writes or delay commands.

### Command Format (variable length)

**Type 0: Delay (most common)**
```
Bit 7: 0 (command type marker)
Bits 6-0: delay count
  0x00 = end of track
  0x01-0x7F = delay N ticks (50Hz VBI default)
```

**Type 1: VERA Write**
```
Bit 7: 1
Bits 6-0: register offset (0x00-0x7F = $D100-$D17F)
Next byte: value to write
```

Example:
- `0x80 0x00 0x42` = write 0x42 to $D100 (PSG ch0 freq_lo)
- `0x03` = delay 3 ticks
- `0x00` = end of track

## Standard VERA Register Map
```
$D100-$D1F3: PSG (16 channels, 4 bytes each)
  ch=0..15
  +0: Frequency low (8-bit)
  +1: Frequency high (8-bit)
  +2: Volume (bits 5-0) + Stereo (bits 6-7)
  +3: Pulse width (bits 5-0) + Waveform (bits 7-6)

$D1F8: PCM Control
$D1F9: PCM Rate
$D1FA: PCM FIFO (write data)
$D1FB-$D1FF: Reserved
```

## Timing
- Default tempo: 1 tick = 1/50 sec (50 Hz PAL) or 1/60 sec (60 Hz NTSC)
- Furnace export can adjust tempo via tempo flags

## Constraints for Atari PBI
- Max file size: ~28 KB (leaving ~4KB for player code)
- PSG only (PCM requires streaming from storage)
- Single-threaded VBI playback (no pause/seek support initially)
