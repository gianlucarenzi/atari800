# XEP80 — Riferimento tecnico
Fonte: https://atariwiki.org/wiki/Wiki.jsp?page=XEP80

## Panoramica
L'XEP80 è un'interfaccia display a 80 colonne per Atari XL/XE, che fornisce capacità di visualizzazione testo estesa rispetto alle standard 40 colonne ANTIC/GTIA.

---

## Architettura hardware

**Video Controller:**
- Chip a 48 pin con bus dati a 16 bit sul lato video
- Lettura simultanea di due RAM (testo e attributi)
- Clock di sistema: 12 MHz

**Interfaccia seriale:**
- UART equivalente alla specifica del chip 6551
- Velocità di trasferimento dati: 15.7 kbit/s (circa 63.7 µs/bit)
- Pre-divisore a 4 bit: divide il clock di sistema in 16 step
- Divisore baudrate a 11 bit ulteriormente diviso per 16
- Configurazione standard: 15625 baud

---

## Capacità di visualizzazione

**Formato schermo:**
- 80 colonne × 25 righe
- Set di caratteri ATASCII
- Attributi per singolo carattere

**Attributi supportati:**
- Inverse video
- Half-bright / variazioni di luminosità
- Flickering / blinking
- Testo doppia altezza
- Testo doppia larghezza
- Underline (personalizzabile)
- Testo nascosto
- Miscelazione grafica con testo

---

## Protocollo di comunicazione

**Formato dati:** parole a 9 bit (8 bit dati + 1 bit di controllo)

**Comandi principali:**

| Opcode | Nome   | Descrizione |
|--------|--------|-------------|
| $50    | XCH80  | Posizione cursore orizzontale |
| $60    | LMG80  | Impostazione margine sinistro (low) |
| $70    | LMH80  | Impostazione margine sinistro (high) |
| $80    | YCR80  | Posizione cursore verticale |
| $99    | SGR80  | Set modalità grafica |
| $9A    | PAG80  | Configurazione pagina |
| $A0    | RMG80  | Margine destro (low) |
| $B0    | RMH80  | Margine destro (high) |
| $C0    | GET80  | Richiedi carattere |
| $C1    | CUR80  | Controllo cursore |
| $C2    | RST80  | Reset |
| $C3    | PST80  | Post / inizializzazione |
| $C4    | CLR80  | Clear screen |
| $D0    | LIS80  | List flag |
| $D2    | SCR80  | Scroll control |
| $D3    | SCB80  | Scroll back |
| $D4    | GRF80  | Modalità grafica |
| $D5    | ICM80  | International character mode |
| $D7    | PAL80  | Standard PAL/NTSC (50Hz/60Hz) |
| $D9    | CRS80  | Visibilità cursore |
| $DB    | MCF80  | Modifica formato cursore |
| $DD    | PNT80  | Print mode |

---

## Light Pen

- **HPEN**: posizione orizzontale a 7 bit (128 posizioni)
- **VPEN**: posizione verticale a 5 bit (32 posizioni)
- Storage della posizione basato su interrupt

---

## Note prestazionali

Il manuale originale dichiara 15.7 kBit/s di trasferimento. Alcune applicazioni terminale come BobTerm erano limitate a 4800 baud a causa dell'overhead dell'istruzione WSYNC.

---

## Mappatura memoria

Il driver occupa circa 3 KB tra `$9600–$9C1F` (configurazione NTSC), con aggiustamento di MEMHI necessario per la gestione corretta della memoria.

---

## Standard supportati

- NTSC (60 Hz)
- PAL (50 Hz) — richiede modifica del driver o comandi di inizializzazione specifici (`PAL80`)
