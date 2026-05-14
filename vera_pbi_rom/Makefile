# Makefile — VeraX16 PBI ROM handler
#
# Requires the cc65 toolchain: ca65 (assembler) and ld65 (linker).
# Install on Debian/Ubuntu:  apt install cc65
# Install on macOS (brew):   brew install cc65
#
# Build:   make
# Clean:   make clean
# Install: make install  (copies ROM into the emulator build tree)

CA65     = ca65
LD65     = ld65
DIR2ATR  ?= dir2atr

ASFLAGS  = --cpu 6502 --feature labels_without_colons
SYSCFG   = vera_sys.cfg
SYSASFLAGS = $(ASFLAGS) -D __ATARIXL__ -D SHRAM_HANDLERS
SYSAUTORUNSRC = vera_sys_autorun.s

TARGET   = vera_pbi_handler.rom
OBJ      = vera_pbi_handler.o
SRC      = vera_pbi_handler.s
CFG      = vera_pbi.cfg
SYS = VERA.SYS
SYSSRC = vera_stub.s vera_driver.s
SYSASMSRC = vera_sys_vbi.s vera_sys_es_hook.s vera_sys_font.s vera_sys_dosini.s
SYSHDRSRC = vera_sys_header.s
SYSOBJ = $(SYSSRC:.s=.o) $(SYSASMSRC:.s=.o) $(SYSAUTORUNSRC:.s=.o) $(SYSHDRSRC:.s=.o)

AUTORUN  = AUTORUN.SYS
ATR      = vera_pbi.atr
ATRBUILD = .atrbuild

# Optional: path where the ROM binary is expected by the emulator.
# Adjust to match your build layout, or pass on the command line:
#   make install ROMDIR=/path/to/roms
ROMDIR   = ../roms

MAPFILE_ROM = vera_pbi.map
MAPFILE_SYS = vera_sys.map
LABELS      = vera.lbl

.PHONY: all clean cleanall install atr labels

all: $(TARGET) $(SYS)

$(OBJ): $(SRC)
	$(CA65) $(ASFLAGS) -o $(OBJ) $(SRC)

$(TARGET): $(OBJ) $(CFG)
	$(LD65) -C $(CFG) --mapfile $(MAPFILE_ROM) -vm -o $(TARGET) $(OBJ)
	@echo "ROM size: $$(wc -c < $(TARGET)) bytes (max 2048)"

$(SYSOBJ): %.o: %.s
	$(CA65) $(SYSASFLAGS) -o $@ $<

$(SYS): $(SYSOBJ) $(SYSCFG)
	$(LD65) -C $(SYSCFG) --mapfile $(MAPFILE_SYS) -vm -o $(SYS) $(SYSOBJ)
	@echo "SYS size: $$(wc -c < $(SYS)) bytes"

labels: $(TARGET) $(SYS)
	python3 make_labels.py $(MAPFILE_ROM) $(MAPFILE_SYS) > $(LABELS)
	@echo "Labels written to $(LABELS) ($$(wc -l < $(LABELS)) entries)"

$(AUTORUN): $(SYS)
	cp $(SYS) $(AUTORUN)

DOS20DIR  = .dos20
DOS20EXTRACT = extract_dos20.py

$(DOS20DIR)/DOS.SYS: $(DOS20EXTRACT) 810_Master_Disk_DOS_2.0s_1980_Atari.atr
	python3 $(DOS20EXTRACT)

atr: $(TARGET) $(AUTORUN) $(DOS20DIR)/DOS.SYS
	rm -rf $(ATRBUILD)
	mkdir -p $(ATRBUILD)
	cp $(AUTORUN) $(ATRBUILD)/$(AUTORUN)
	cp $(DOS20DIR)/DOS.SYS $(DOS20DIR)/DUP.SYS $(ATRBUILD)/
	$(DIR2ATR) -b Dos20 $(ATR) $(ATRBUILD)
	@echo "ATR written to $(ATR)"

clean:
	rm -rf $(OBJ) $(TARGET) $(SYS) $(AUTORUN) $(ATR) $(ATRBUILD) $(LABELS) \
		$(SYSOBJ)

cleanall: clean
	rm -rf $(MAPFILE_ROM) $(MAPFILE_SYS)

install: $(TARGET)
	@mkdir -p $(ROMDIR)
	cp $(TARGET) $(ROMDIR)/$(TARGET)
	@echo "Installed to $(ROMDIR)/$(TARGET)"
