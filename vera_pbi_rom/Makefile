# Makefile — VeraX16 PBI ROM handler + relocatable VERA.SYS driver.
#
# Requires the cc65 toolchain: ca65 (assembler) and ld65 (linker).
# Install on Debian/Ubuntu:  apt install cc65
# Install on macOS (brew):   brew install cc65
#
# Build:   make
# Clean:   make clean
# Install: make install   (copies ROM into the emulator build tree)

CA65     = ca65
LD65     = ld65
DIR2ATR  ?= dir2atr
PYTHON   ?= python3

ASFLAGS    = --cpu 6502 --feature labels_without_colons
SYSASFLAGS = $(ASFLAGS) -D __ATARIXL__ -D SHRAM_HANDLERS

# Font/viewport selection: 8x8 → 80×60, 8x16 → 80×30 (default)
# Usage: make FONT_SIZE=8x8   or   make FONT_SIZE=8x16
FONT_SIZE  ?= 8x16
ifeq ($(FONT_SIZE),8x8)
  SYSASFLAGS += -D FONT_8X8=1
endif

# --- PBI ROM (unchanged) -----------------------------------------------------

TARGET   = vera_pbi_handler.rom
OBJ      = vera_pbi_handler.o
SRC      = vera_pbi_handler.s
CFG      = vera_pbi.cfg
MAPFILE_ROM = vera_pbi.map

# --- Relocatable VERA.SYS body ----------------------------------------------

SYS         = VERA.SYS
SYSCFG      = vera_sys.cfg
BODY_BASE_A = 0xA000
BODY_BASE_B = 0xA100

BODY_SRC = vera_stub.s vera_driver.s vera_sys_vbi.s vera_sys_es_hook.s \
           vera_sys_font.s vera_sys_dosini.s
BODY_OBJ = $(BODY_SRC:.s=.o)

BODY_BIN_A = body_A000.bin
BODY_BIN_B = body_A100.bin
BODY_LBL_A = body_A000.lbl
BODY_LBL_B = body_A100.lbl
BODY_MAP_A = body_A000.map
BODY_MAP_B = body_A100.map

FIXUPS_BIN = fixups.bin

# --- One-shot bootstrap loader ----------------------------------------------

LOADER_CFG = vera_loader.cfg
LOADER_SRC = vera_sys_loader.s
LOADER_OBJ = vera_sys_loader.o
LOADER_BIN = loader.bin
LOADER_LBL = loader.lbl
LOADER_MAP = loader.map

# --- Output / packaging -----------------------------------------------------

AUTORUN  = AUTORUN.SYS
ATR      = vera_pbi.atr
ATRBUILD = .atrbuild
ROMDIR   = ../roms
LABELS   = vera.lbl

GEN_FIXUPS = gen_fixups.py
ASSEMBLE   = assemble_autorun.py

.PHONY: all clean cleanall install atr labels

all: $(TARGET) $(SYS)

# === PBI ROM ================================================================

$(OBJ): $(SRC)
	$(CA65) $(ASFLAGS) -D FONT_8X8=1 -o $(OBJ) $(SRC)

$(TARGET): $(OBJ) $(CFG)
	$(LD65) -C $(CFG) --mapfile $(MAPFILE_ROM) -vm -o $(TARGET) $(OBJ)
	@echo "ROM size: $$(wc -c < $(TARGET)) bytes (max 2048)"

# === VERA.SYS body ===========================================================

$(BODY_OBJ): %.o: %.s
	$(CA65) $(SYSASFLAGS) -o $@ $<

# Two builds at adjacent base addresses (delta $100). gen_fixups.py diffs
# them to discover every internal absolute pointer.
$(BODY_BIN_A) $(BODY_LBL_A) $(BODY_MAP_A): $(BODY_OBJ) $(SYSCFG)
	$(LD65) -C $(SYSCFG) -S $(BODY_BASE_A) \
		--mapfile $(BODY_MAP_A) -Ln $(BODY_LBL_A) \
		-vm -o $(BODY_BIN_A) $(BODY_OBJ)

$(BODY_BIN_B) $(BODY_LBL_B) $(BODY_MAP_B): $(BODY_OBJ) $(SYSCFG)
	$(LD65) -C $(SYSCFG) -S $(BODY_BASE_B) \
		--mapfile $(BODY_MAP_B) -Ln $(BODY_LBL_B) \
		-vm -o $(BODY_BIN_B) $(BODY_OBJ)

$(FIXUPS_BIN): $(BODY_BIN_A) $(BODY_BIN_B) $(GEN_FIXUPS)
	$(PYTHON) $(GEN_FIXUPS) $(BODY_BIN_A) $(BODY_BIN_B) $(FIXUPS_BIN)

# === Loader ==================================================================

$(LOADER_OBJ): $(LOADER_SRC)
	$(CA65) $(SYSASFLAGS) -o $(LOADER_OBJ) $(LOADER_SRC)

$(LOADER_BIN) $(LOADER_LBL) $(LOADER_MAP): $(LOADER_OBJ) $(LOADER_CFG)
	$(LD65) -C $(LOADER_CFG) \
		--mapfile $(LOADER_MAP) -Ln $(LOADER_LBL) \
		-vm -o $(LOADER_BIN) $(LOADER_OBJ)

# === AUTORUN.SYS assembly ===================================================

$(SYS): $(BODY_BIN_A) $(BODY_LBL_A) $(FIXUPS_BIN) \
        $(LOADER_BIN) $(LOADER_LBL) $(ASSEMBLE)
	$(PYTHON) $(ASSEMBLE) \
		$(BODY_BIN_A) $(BODY_LBL_A) $(FIXUPS_BIN) \
		$(LOADER_BIN) $(LOADER_LBL) $(SYS)

labels: $(TARGET) $(SYS)
	$(PYTHON) make_labels.py $(MAPFILE_ROM) $(BODY_MAP_A) > $(LABELS)
	@echo "Labels written to $(LABELS) ($$(wc -l < $(LABELS)) entries)"

$(AUTORUN): $(SYS)
	cp $(SYS) $(AUTORUN)

# === ATR image ==============================================================

DOS20DIR     = .dos20
DOS20EXTRACT = extract_dos20.py

TEST_SRC = ../test_font.c
TEST_EXE = TEST.COM

$(TEST_EXE): $(TEST_SRC)
	cl65 -t atari -o $(TEST_EXE) $(TEST_SRC)

$(DOS20DIR)/DOS.SYS: $(DOS20EXTRACT) 810_Master_Disk_DOS_2.0s_1980_Atari.atr
	$(PYTHON) $(DOS20EXTRACT)

atr: $(TARGET) $(AUTORUN) $(DOS20DIR)/DOS.SYS $(TEST_EXE)
	rm -rf $(ATRBUILD)
	mkdir -p $(ATRBUILD)
	cp $(AUTORUN) $(ATRBUILD)/$(AUTORUN)
	cp $(TEST_EXE) $(ATRBUILD)/
	cp $(DOS20DIR)/DOS.SYS $(DOS20DIR)/DUP.SYS $(ATRBUILD)/
	$(DIR2ATR) -b Dos20 $(ATR) $(ATRBUILD)
	@echo "ATR written to $(ATR)"

# === Cleanup ================================================================

clean:
	rm -rf $(OBJ) $(TARGET) $(SYS) $(AUTORUN) $(ATR) $(ATRBUILD) $(LABELS) \
		$(BODY_OBJ) $(LOADER_OBJ) \
		$(BODY_BIN_A) $(BODY_BIN_B) $(BODY_LBL_A) $(BODY_LBL_B) \
		$(LOADER_BIN) $(LOADER_LBL) $(FIXUPS_BIN)

cleanall: clean
	rm -rf $(MAPFILE_ROM) $(BODY_MAP_A) $(BODY_MAP_B) $(LOADER_MAP)

install: $(TARGET)
	@mkdir -p $(ROMDIR)
	cp $(TARGET) $(ROMDIR)/$(TARGET)
	@echo "Installed to $(ROMDIR)/$(TARGET)"
