# Makefile — VeraX16 PBI ROM handler + relocatable VERA.SYS driver.
#
# This Makefile generates the PBI ROM (fixed at 80x60) and three different 
# versions of the relocatable VERA.SYS driver for various resolutions.

CA65     = ca65
LD65     = ld65
DIR2ATR  ?= dir2atr
PYTHON   ?= python3

ASFLAGS    = --cpu 6502 --feature labels_without_colons
# PBI ROM always starts in 80x60 mode (8x8 font, 1:1 scale) to provide
# a consistent boot experience before any driver is loaded.
ROM_ASFLAGS = $(ASFLAGS) -D FONT_8X8=1
SYSASFLAGS = $(ASFLAGS) -D __ATARIXL__ -D SHRAM_HANDLERS

# Screen/viewport selection for the RAM driver (passed via sub-make calls):
#   SCREEN=80x60 → 80×60 viewport (8-pixel tiles, 640×480)
#   SCREEN=80x30 → 80×30 viewport (16-pixel tiles, 640×480, default)
#   SCREEN=40x30 → 40×30 viewport (8-pixel tiles, 2× scale, 320×240)
SCREEN  ?= 80x30
ifeq ($(SCREEN),80x60)
  SYSASFLAGS += -D FONT_8X8=1
endif
ifeq ($(SCREEN),40x30)
  SYSASFLAGS += -D MODE_40X30=1 -D FONT_8X8=1
endif

# --- Output filenames for multi-resolution drivers ---

SYS4030 = VERA4030.SYS
SYS8030 = VERA8030.SYS
SYS8060 = VERA8060.SYS

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

ATR      = vera_pbi.atr
ATRBUILD = .atrbuild
ROMDIR   = ../roms
LABELS   = vera.lbl

GEN_FIXUPS  = gen_fixups.py
ASSEMBLE    = assemble_autorun.py
BUNDLE_VERA = bundle_vera.py

.PHONY: all clean cleanall install atr labels drivers clean_objs

# The default target builds the PBI ROM and all three driver versions.
all: $(TARGET) $(SYS) drivers

# Rule to generate the three resolution-specific drivers.
# Each build requires a clean objects pass to ensure correct defines are applied.
drivers: $(SYS4030) $(SYS8030) $(SYS8060)

$(SYS4030):
	$(MAKE) clean_objs
	$(MAKE) SCREEN=40x30 $(SYS)
	cp $(SYS) $(SYS4030)

$(SYS8030): $(BODY_SRC) $(LOADER_SRC) vera_common.inc
	$(MAKE) clean_objs
	$(MAKE) SCREEN=80x30 $(SYS)
	cp $(SYS) $(SYS8030)

$(SYS8060):
	$(MAKE) clean_objs
	$(MAKE) SCREEN=80x60 $(SYS)
	cp $(SYS) $(SYS8060)

# Deletes only the object files and intermediate binaries to allow switching
# between different resolution builds without deleting the final renamed drivers.
clean_objs:
	rm -rf $(OBJ) $(BODY_OBJ) $(LOADER_OBJ) $(BODY_BIN_A) $(BODY_BIN_B) $(FIXUPS_BIN) $(LOADER_BIN) $(SYS)

# === PBI ROM ================================================================

$(OBJ): $(SRC)
	$(CA65) $(ROM_ASFLAGS) -o $(OBJ) $(SRC)

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

# === ATR image ==============================================================

TEST_SRC      = vera-tests/test_font.c
TEST_GS_SRC   = vera-tests/test_gradient_scroll.c
TEST_MAZE_SRC = vera-tests/test_maze.c
TEST_FX_SRC   = vera-tests/test_fx.c
TEST_FX_EXE   = TESTFX.COM

# Resolution-specific bundled variants (suffix: 4=40x30, 8=80x30, 6=80x60)
TEST_EXES     = TEST4.COM TEST8.COM TEST6.COM
TESTGS_EXES   = TESTGS4.COM TESTGS8.COM TESTGS6.COM
TESTMAZE_EXES = TESTMAZ4.COM TESTMAZ8.COM TESTMAZ6.COM
ALL_TEST_EXES = $(TEST_EXES) $(TESTGS_EXES) $(TESTMAZE_EXES) $(TEST_FX_EXE) $(RUNCPM_EXE)

# Template: compile once to an intermediate binary, then bundle three times.
# $(1) = output base name (7 chars max, no digit suffix)
# $(2) = intermediate raw binary (e.g. _TEST.COM)
# $(3) = C source file
define make_test_variants
$(2): $(3)
	cl65 -t atari --start-addr 0x5000 -o $$@ $$<

$(1)4.COM: $(2) $(SYS4030) $(BUNDLE_VERA)
	$$(PYTHON) $$(BUNDLE_VERA) $$(SYS4030) $$< $$@

$(1)8.COM: $(2) $(SYS8030) $(BUNDLE_VERA)
	$$(PYTHON) $$(BUNDLE_VERA) $$(SYS8030) $$< $$@

$(1)6.COM: $(2) $(SYS8060) $(BUNDLE_VERA)
	$$(PYTHON) $$(BUNDLE_VERA) $$(SYS8060) $$< $$@
endef

$(eval $(call make_test_variants,TEST,_TEST.COM,$(TEST_SRC)))
$(eval $(call make_test_variants,TESTGS,_TESTGS.COM,$(TEST_GS_SRC)))
$(eval $(call make_test_variants,TESTMAZ,_TESTMAZE.COM,$(TEST_MAZE_SRC)))

# RUNCPM.COM specifically for 80x30
RUNCPM_SRC = vera-tests/runcpm.c
RUNCPM_EXE = RUNCPM.COM
RUNCPM80_EXE = RUNCPM80.COM

$(RUNCPM_EXE): $(RUNCPM_SRC) vera-tests/serterm_handler.o vera_sys_font.o
	# Load RUNCPM low enough to fit even if resident drivers (VERA/FujiNet/etc.) lower MEMTOP.
	# Use a cc65 cfg without the SYSCHK chunk at $2E00.
	cl65 -t atari -C vera-tests/atari_nosyschk.cfg --start-addr 0x3000 -o $(RUNCPM_EXE) $(RUNCPM_SRC) vera-tests/serterm_handler.o vera_sys_font.o

$(RUNCPM80_EXE): $(RUNCPM_EXE) $(SYS8030) $(BUNDLE_VERA)
	$(PYTHON) $(BUNDLE_VERA) $(SYS8030) $(RUNCPM_EXE) $(RUNCPM80_EXE)

vera-tests/serterm_handler.o: vera-tests/serterm_handler.s
	$(CA65) -I . -o $@ $<

$(TEST_FX_EXE): $(TEST_FX_SRC)
	cl65 -t atari --start-addr 0x5000 -o $(TEST_FX_EXE) $(TEST_FX_SRC)

# ATR image configuration
REQUIRED_TEST_EXES = TEST4.COM TEST6.COM TESTFX.COM

# FUJINET_SD_PATH can be set to the path of your Fujinet SD card
# Example: make atr FUJINET_SD_PATH=/media/user/FUJINET/
FUJINET_SD_PATH ?=

# DOS 2.0s master disk used as source to extract DOS.SYS/DUP.SYS.
# Note: DOS 2.0s itself doesn't support ED, but this still produces a bootable
# image; extra sectors are simply unused by DOS.
DOS20_ATR    = 810_Master_Disk_DOS_2.0s_1980_Atari.atr
DOS20_DIR    = .dos20
DOS20_EXTRACT = extract_dos20.py

$(DOS20_DIR)/DOS.SYS $(DOS20_DIR)/DUP.SYS: $(DOS20_ATR) $(DOS20_EXTRACT)
	$(PYTHON) $(DOS20_EXTRACT) $(DOS20_ATR) $(DOS20_DIR)

# Build a bootable DOS 2.0s ED ATR containing RUNCPM.COM and both
# VERA8030.SYS (80x30) and VERA8060.SYS (80x60) drivers.
atr: $(TARGET) $(RUNCPM_EXE) $(SYS8030) $(SYS8060) $(DOS20_DIR)/DOS.SYS $(DOS20_DIR)/DUP.SYS
	rm -rf $(ATRBUILD)
	mkdir -p $(ATRBUILD)
	cp $(DOS20_DIR)/DOS.SYS $(DOS20_DIR)/DUP.SYS $(RUNCPM_EXE) $(SYS8030) $(SYS8060) $(ATRBUILD)/
	$(DIR2ATR) -E -b Dos20 $(ATR) $(ATRBUILD)
	@echo "ATR written to $(ATR)"
	@if [ ! -z "$(FUJINET_SD_PATH)" ]; then \
		cp $(ATR) $(FUJINET_SD_PATH)/; \
		echo "ATR copied to $(FUJINET_SD_PATH)"; \
	fi

# === Cleanup ================================================================

clean: clean_objs
	rm -rf $(TARGET) $(ATR) $(ATRBUILD) $(LABELS) \
		$(BODY_LBL_A) $(BODY_LBL_B) \
		$(LOADER_LBL) \
		$(ALL_TEST_EXES) \
		_TEST.COM _TESTGS.COM _TESTMAZE.COM \
		$(SYS4030) $(SYS8030) $(SYS8060) \
		.dos20

cleanall: clean
	rm -rf $(MAPFILE_ROM) $(BODY_MAP_A) $(BODY_MAP_B) $(LOADER_MAP) \
		$(SYS4030) $(SYS8030) $(SYS8060)

install: $(TARGET)
	@mkdir -p $(ROMDIR)
	cp $(TARGET) $(ROMDIR)/$(TARGET)
	@echo "Installed to $(ROMDIR)/$(TARGET)"
