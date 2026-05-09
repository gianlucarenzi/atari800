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
CL65     ?= cl65
DIR2ATR  ?= dir2atr

ASFLAGS  = --cpu 6502 --feature labels_without_colons
CFLAGS   ?= -Oirs -t atarixl
SYSSTART ?= 0x8000
SYSCFLAGS = $(CFLAGS) --start-addr $(SYSSTART)

TARGET   = vera_pbi_handler.rom
OBJ      = vera_pbi_handler.o
SRC      = vera_pbi_handler.s
CFG      = vera_pbi.cfg
SYS      = VERA.SYS
SYSSRC   = vera_sys.c
SYSASMSRC = vera_sys_vbi.s
AR0      = mypdos40.ar0
ATR      = vera_pbi.atr
ATRBUILD = .atrbuild

# Optional: path where the ROM binary is expected by the emulator.
# Adjust to match your build layout, or pass on the command line:
#   make install ROMDIR=/path/to/roms
ROMDIR   = ../roms

.PHONY: all clean install atr

all: $(TARGET) $(SYS)

$(OBJ): $(SRC)
	$(CA65) $(ASFLAGS) -o $(OBJ) $(SRC)

$(TARGET): $(OBJ) $(CFG)
	$(LD65) -C $(CFG) -o $(TARGET) $(OBJ)
	@echo "ROM size: $$(wc -c < $(TARGET)) bytes (max 2048)"

$(SYS): $(SYSSRC) $(SYSASMSRC)
	$(CL65) $(SYSCFLAGS) -o $(SYS) $(SYSSRC) $(SYSASMSRC)
	@echo "SYS size: $$(wc -c < $(SYS)) bytes"

$(AR0): $(SYS)
	cp $(SYS) $(AR0)

atr: $(AR0)
	rm -rf $(ATRBUILD)
	mkdir -p $(ATRBUILD)
	cp $(AR0) $(ATRBUILD)/$(AR0)
	$(DIR2ATR) -a -b MyPicoDos406 $(ATR) $(ATRBUILD)
	@echo "ATR written to $(ATR)"

clean:
	rm -rf $(OBJ) $(TARGET) $(SYS) $(AR0) $(ATR) $(ATRBUILD) \
		vera_sys.o vera_sys_vbi.o

install: $(TARGET)
	@mkdir -p $(ROMDIR)
	cp $(TARGET) $(ROMDIR)/$(TARGET)
	@echo "Installed to $(ROMDIR)/$(TARGET)"
