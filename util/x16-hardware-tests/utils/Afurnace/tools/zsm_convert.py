#!/usr/bin/env python3
"""
ZSM to Atari PRG/XEX converter
Converts Furnace ZSM exports to Atari-executable music player
"""

import struct
import sys
import argparse
from pathlib import Path

class ZSMInfo:
    def __init__(self, filename):
        with open(filename, 'rb') as f:
            data = f.read()
        
        if len(data) < 8:
            raise ValueError("ZSM file too short (need header)")
        
        if data[0:3] != b'ZSM':
            raise ValueError("Invalid ZSM magic")
        
        self.version = data[3]
        self.loop_offset = struct.unpack('<H', data[4:6])[0]
        self.data = data
        self.size = len(data)
    
    def info_str(self):
        return f"ZSM v{self.version}, size={self.size} bytes, loop at +${self.loop_offset:04X}"

def validate_zsm(filename):
    """Check if ZSM is valid"""
    info = ZSMInfo(filename)
    print(f"✓ {info.info_str()}")
    
    # Count commands
    pos = 8
    cmds = 0
    while pos < len(info.data):
        byte = info.data[pos]
        if byte == 0:
            print(f"  End marker at offset ${pos:04X}")
            break
        if byte & 0x80:
            # VERA write: skip 2 bytes
            pos += 2
            cmds += 1
        else:
            # Delay
            pos += 1
            cmds += 1
    
    print(f"  Commands: {cmds}")
    return True

def pack_xex(zsm_filename, player_bin):
    """
    Pack ZSM + player into Atari XEX format
    
    Memory layout:
    $2000-$27FF: ZSM data (up to 2KB)
    $3000-$37FF: Player code (from zsm_player.bin)
    RUNAD: $3000 (entry point)
    """
    
    info = ZSMInfo(zsm_filename)
    
    if info.size > 0x2000:
        raise ValueError(f"ZSM too large: {info.size} bytes (max 8KB)")
    
    # Read player binary
    with open(player_bin, 'rb') as f:
        player_data = f.read()
    
    if len(player_data) > 0x800:
        raise ValueError(f"Player too large: {len(player_data)} bytes (max 2KB)")
    
    # Build XEX: list of segments
    # Format: $FFFD markers separate segments
    # $FFFD $FFFD = header record (not used for raw bin data)
    # Each segment: ADDR_LO ADDR_HI LEN_LO LEN_HI ... data
    # Last: $FFFD RUNAD_LO RUNAD_HI
    
    xex = bytearray()
    
    # Segment 1: ZSM data at $2000
    xex.extend([0xFF, 0xFE])                    # Segment header
    xex.extend([0x00, 0x20])                    # Address $2000
    zsm_size = info.size
    xex.extend([zsm_size & 0xFF, (zsm_size >> 8) & 0xFF])  # Length
    xex.extend(info.data)
    
    # Segment 2: Player code at $3000
    xex.extend([0xFF, 0xFE])                    # Segment header
    xex.extend([0x00, 0x30])                    # Address $3000
    player_size = len(player_data)
    xex.extend([player_size & 0xFF, (player_size >> 8) & 0xFF])  # Length
    xex.extend(player_data)
    
    # Run address (RUNAD) = $3000
    xex.extend([0xFF, 0xFD])                    # RUNAD marker
    xex.extend([0x00, 0x30])                    # Address $3000
    
    return bytes(xex)

def main():
    parser = argparse.ArgumentParser(description='ZSM to Atari converter')
    parser.add_argument('input', help='Input ZSM file')
    parser.add_argument('-o', '--output', help='Output XEX file')
    parser.add_argument('-i', '--info', action='store_true', help='Show ZSM info')
    parser.add_argument('-p', '--player', default='player/zsm_player.bin', help='Player binary')
    
    args = parser.parse_args()
    
    try:
        if args.info:
            validate_zsm(args.input)
        
        if args.output:
            print(f"Converting {args.input} to {args.output}...")
            
            # Check if player exists
            if not Path(args.player).exists():
                print(f"✗ Player binary not found: {args.player}", file=sys.stderr)
                print(f"  Run 'make' to build player", file=sys.stderr)
                sys.exit(1)
            
            xex = pack_xex(args.input, args.player)
            with open(args.output, 'wb') as f:
                f.write(xex)
            print(f"✓ Wrote {len(xex)} bytes to {args.output}")
    
    except Exception as e:
        print(f"✗ Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()
