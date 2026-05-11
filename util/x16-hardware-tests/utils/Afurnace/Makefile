.PHONY: all clean help info

PLAYER_ASM  = player/zsm_player.s
PLAYER_C    = player/zsm_player.c
PLAYER_OBJ  = player/zsm_player.o
PLAYER_BIN  = player/zsm_player.bin

CA65       ?= ca65
CC65       ?= cc65
LD65       ?= ld65
PYTHON3    ?= python3

all: $(PLAYER_BIN)

# Compile C to assembly
player/zsm_player.s: $(PLAYER_C)
	$(CC65) --cpu 6502 -Oi -t none -o $@ $<

$(PLAYER_OBJ): player/zsm_player.s
	$(CA65) --cpu 6502 -o $@ $<

$(PLAYER_BIN): $(PLAYER_OBJ) player/cc65.cfg
	$(LD65) -C player/cc65.cfg -L /usr/share/cc65/lib -o $@ $< atari.lib
	@echo "Player size: `wc -c < $@` bytes"
	@ls -lh $@

info:
	@echo "Afurnace: Furnace ZSM Player for Atari XL/XE PBI VERA (cc65 version)"
	@echo ""
	@echo "Targets:"
	@echo "  make           - Build player code from C"
	@echo "  make clean     - Remove build artifacts"
	@echo "  make help      - Show this help"
	@echo ""
	@echo "Usage:"
	@echo "  1. Export ZSM from Furnace (File → Export → ZSM)"
	@echo "  2. python3 tools/zsm_convert.py --info music.zsm"
	@echo "  3. python3 tools/zsm_convert.py -o music.xex music.zsm"
	@echo "  4. Load music.xex in Atari800 emulator"

help: info

clean:
	rm -f player/zsm_player.s player/zsm_player.o player/zsm_player.bin

# Validate ZSM file (requires argument: make validate FILE=test.zsm)
validate:
	@if [ -z "$(FILE)" ]; then \
		echo "Usage: make validate FILE=music.zsm"; exit 1; \
	fi
	$(PYTHON3) tools/zsm_convert.py --info $(FILE)

# Convert ZSM to XEX (requires arguments: make convert INFILE=test.zsm OUTFILE=test.xex)
convert:
	@if [ -z "$(INFILE)" ] || [ -z "$(OUTFILE)" ]; then \
		echo "Usage: make convert INFILE=music.zsm OUTFILE=music.xex"; exit 1; \
	fi
	$(PYTHON3) tools/zsm_convert.py -o $(OUTFILE) $(INFILE) -p $(PLAYER_BIN)
