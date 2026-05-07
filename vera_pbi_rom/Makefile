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

ASFLAGS  = --cpu 6502 --feature labels_without_colons

TARGET   = vera_pbi_handler.rom
OBJ      = vera_pbi_handler.o
SRC      = vera_pbi_handler.s
CFG      = vera_pbi.cfg

# Optional: path where the ROM binary is expected by the emulator.
# Adjust to match your build layout, or pass on the command line:
#   make install ROMDIR=/path/to/roms
ROMDIR   = ../roms

.PHONY: all clean install

all: $(TARGET)

$(OBJ): $(SRC)
	$(CA65) $(ASFLAGS) -o $(OBJ) $(SRC)

$(TARGET): $(OBJ) $(CFG)
	$(LD65) -C $(CFG) -o $(TARGET) $(OBJ)
	@echo "ROM size: $$(wc -c < $(TARGET)) bytes (max 2048)"

clean:
	rm -f $(OBJ) $(TARGET)

install: $(TARGET)
	@mkdir -p $(ROMDIR)
	cp $(TARGET) $(ROMDIR)/$(TARGET)
	@echo "Installed to $(ROMDIR)/$(TARGET)"
