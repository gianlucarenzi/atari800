# FujiNet-PC for Atari — Complete Guide

FujiNet-PC is a software implementation of FujiNet that runs on Linux/macOS/Windows
instead of physical ESP32 hardware. It communicates with the atari800 emulator via
the **NetSIO** protocol (SIO over UDP) — no real serial port required.

---

## Directory layout

```
vera_pbi_rom/FujiNet/fujinet-pc-ATARI/
├── fujinet                   ← ready-to-run binary (pre-built nightly)
├── run-fujinet               ← startup script with auto-restart
├── fnconfig.ini              ← configuration (TNFS hosts, baud, etc.)
├── data/                     ← firmware resources (850 handler, fonts, web UI, …)
├── SD/                       ← virtual SD card (disk images, CPM files, …)
│   └── CPM/
└── fujinet-firmware-nightly_fn_pc_v1.6.1-dev-git-c5c1cff9e/   ← source tree
```

---

## Running (pre-built binary — normal case)

**Always use `run-fujinet`, never `./fujinet` directly.**

When atari800 boots it sends a cold-reset signal (byte `0xFF`) to FujiNet, which
causes it to exit with code 75. `run-fujinet` catches that code and restarts
FujiNet automatically — without this loop the connection is permanently lost after
the first Atari boot.

**Terminal 1 — start FujiNet-PC (from its own directory):**

```sh
cd vera_pbi_rom/FujiNet/fujinet-pc-ATARI/
./run-fujinet -c fnconfig.ini -s SD/
```

FujiNet-PC listens on:
- **UDP 9997** — NetSIO (SIO-over-network protocol used by the emulator)
- **HTTP 8000** — Web UI → http://localhost:8000

Wait for `### NetSIO stopped ###` in the log before starting the emulator —
that line means FujiNet is up and waiting for a connection.

**Terminal 2 — start atari800 with NetSIO (from the project root):**

```sh
src/atari800 -verax16 -verax16-rom vera_pbi_rom/vera_pbi_handler.rom \
             -netsio -xl -pal
```

### Verified connection sequence

```
FujiNet starts, waiting for atari800:
  "### NetSIO stopped ###"      ← retries every ~1 s

atari800 starts, FujiNet connects:
  "### NetSIO initialized ###"

atari800 cold-starts, sends COLD_RESET (0xFF):
  "SystemManager::reboot - exiting ..."
  "### NetSIO stopped ###"

run-fujinet detects exit code 75 and restarts:
  "Restarting FujiNet"
  "### NetSIO initialized ###"  ← reconnected, SIO active
  "ATR READ 1 / 720"            ← disk reads working
```

To use a non-default port:
```sh
src/atari800 -netsio 9998 ...
```
Update `fnconfig.ini` accordingly:
```ini
[BOIP]
port=9998
```

---

## Configuration — fnconfig.ini

FujiNet reads (and writes) this file at startup. Key sections:

```ini
[BOIP]
enabled=1
host=localhost
port=          ; empty = default 9997

[Host1]
type=SD
name=SD        ; maps the local SD/ directory

[Host2]
type=TNFS
name=fujinet.online   ; remote TNFS server
```

To add disk images: copy `.atr`/`.xex` files into `SD/` and mount them from the
Web UI (http://localhost:8000) or from the FUJINET-CONFIG program on the Atari.

---

## Building from source (optional)

Only needed if you want to modify the firmware or update to a newer version.
A helper script is provided: `FujiNet/build-fujinet-pc.sh`.

### Dependencies (Debian/Ubuntu)

```sh
sudo apt install cmake g++ libssl-dev libcurl4-openssl-dev \
                 liblua5.3-dev libexpat1-dev python3 rsync
```

### Build and install

```sh
./vera_pbi_rom/FujiNet/build-fujinet-pc.sh \
    vera_pbi_rom/FujiNet/fujinet-pc-ATARI/fujinet-firmware-nightly_fn_pc_v1.6.1-dev-git-c5c1cff9e \
    vera_pbi_rom/FujiNet/fujinet-pc-ATARI
```

The script:
1. Runs `cmake` configure with `-DFUJINET_TARGET=ATARI`
2. Builds with `--target=dist` using all available CPU cores
3. Copies `fujinet`, `run-fujinet`, `data/`, and any bundled `.so` files
4. **Never touches** `fnconfig.ini` or `SD/`

To force a Debug build:
```sh
BUILD_TYPE=Debug ./vera_pbi_rom/FujiNet/build-fujinet-pc.sh <SRC> <DEST>
```

### What the dist target produces

```
build/ATARI/dist/
├── fujinet          ← compiled binary
├── run-fujinet      ← restart-wrapper script
├── fnconfig.ini     ← default config (do NOT copy over existing one)
├── SD/              ← default SD skeleton (do NOT copy over existing one)
├── data/            ← firmware resources
└── *.so             ← bundled shared libraries (Linux)
```

---

## Architecture — how NetSIO works

```
atari800 emulator
    │
    │  UDP localhost:9997
    │  (NetSIO protocol — byte-level SIO bus over UDP)
    ▼
FujiNet-PC (fujinet)
    │
    ├── D1:–D8:   disk images (.atr) from SD/ or TNFS servers
    ├── N:        TCP/HTTP/UDP networking via the host OS
    ├── P:        printer emulation
    └── R:        Hayes modem emulation (WiFi → TCP)
```

FujiNet-PC does **not** use a physical serial port. The `-netsio` flag in atari800
replaces the normal SIO handler entirely with UDP transport to FujiNet-PC.

---

## Copying the ATR to the FujiNet SD card

The Makefile supports an optional `FUJINET_SD_PATH` variable that copies the built
`vera_pbi.atr` directly into the FujiNet SD directory after each build.

**Important:** `make -C vera_pbi_rom/` runs with `vera_pbi_rom/` as its working
directory, so `FUJINET_SD_PATH` must be relative to `vera_pbi_rom/` (not to the
project root), or an absolute path.

```sh
# relative path (from vera_pbi_rom/)
make -C vera_pbi_rom/ atr FUJINET_SD_PATH=FujiNet/fujinet-pc-ATARI/SD

# absolute path (safe from anywhere)
make -C vera_pbi_rom/ atr FUJINET_SD_PATH=$(pwd)/vera_pbi_rom/FujiNet/fujinet-pc-ATARI/SD
```

Combined with a full rebuild:
```sh
make -C vera_pbi_rom/ clean all atr FUJINET_SD_PATH=FujiNet/fujinet-pc-ATARI/SD
```

---

## Quick-start — one-liner sequence

```sh
# 1. Start FujiNet-PC in background
(cd vera_pbi_rom/FujiNet/fujinet-pc-ATARI && \
 ./run-fujinet -c fnconfig.ini -s SD/) &
FNPID=$!

# 2. Start the emulator with VERA + FujiNet
src/atari800 -verax16 -verax16-rom vera_pbi_rom/vera_pbi_handler.rom \
             -netsio -xl -pal

# 3. Stop FujiNet when done
kill $FNPID
```

FujiNet can be started before or after atari800 — it retries the connection every
second — but starting it first avoids an extra cold-reset cycle in the log.
