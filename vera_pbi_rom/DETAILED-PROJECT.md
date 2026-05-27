# Descrizione dettagliata del progetto: Fork di Atari800 con dispositivo VERA PBI

## Panoramica
Questo progetto è un fork specializzato dell'emulatore Atari800, esteso per supportare una periferica custom su Parallel Bus Interface (PBI) basata sul chipset VERA (comunemente presente nel Commander X16). L'obiettivo è fornire un sottosistema video moderno ad alta risoluzione basato su VERA per i computer Atari 8-bit, che agisce come dispositivo di visualizzazione primario bypassando i limiti della grafica ANTIC/GTIA originale.

## Architettura del progetto

Il progetto è composto da due componenti principali, entrambi nella directory `vera_pbi_rom` e strettamente integrati con l'OS Atari:

### 1. Handler OS PBI (`vera_pbi_handler.rom`)
*   **Ruolo:** Inizializza la scheda ed espone un'interfaccia OS PBI standard.
*   **Posizione:** Mappato a `$D800-$DFFF` quando selezionato tramite il latch PBI `$D1FF`.
*   **Funzionalità:**
    *   Implementa l'header ROM PBI Atari standard (checksum, device ID, vettori JMP).
    *   Gestisce l'inizializzazione PBI (handler `INIT`) ai cold/warm start.
    *   Fornisce routine stub per le operazioni CIO, permettendo all'OS di riconoscere la scheda come periferica attiva.
    *   Inizializza i registri hardware VERA per una modalità VGA-compatibile 640×480, configurando il Layer 1 in modalità **80×60 caratteri** (tile 8×8) o **80×30 caratteri** (tile 8×16).
    *   Carica un font di boot minimale (solo i caratteri necessari per il banner) nella VRAM.

### 2. Driver OS rilocabile (`AUTORUN.SYS`)
*   **Ruolo:** Agisce come driver di sistema primario, installato automaticamente al boot.
*   **Funzionalità:**
    *   **Installazione:** Si aggancia alla HATABS (Handler Address Table) per sostituire gli handler standard dei dispositivi Editor (`E:`) e Screen (`S:`) con versioni abilitate per VERA.
    *   **Gestione PUTC:** Sostituisce le routine CIO PUT BYTE standard con una state machine custom che renderizza testo ATASCII direttamente nella VRAM di VERA (viewport 80×60), bypassando la memoria video ANTIC/GTIA originale.
    *   **Hook VBI:** Installa routine di Vertical Blank Interrupt per gestire il lampeggio del cursore e le funzioni metronomo.
    *   **Resilienza al warm start:** Si aggancia ai vettori di reset di sistema (catena `DOSINI`/`CASINI`) per garantire che il driver rimanga attivo e la scheda VERA venga re-inizializzata dopo un reset di sistema.

## Moduli di implementazione principali (vera_pbi_rom/*.s)

I seguenti moduli assembly costituiscono il nucleo dell'implementazione:

*   **`vera_pbi_handler.s`**: Gestisce il protocollo PBI a basso livello, la definizione dell'header ROM e la configurazione hardware iniziale durante la sequenza di cold boot.
*   **`vera_driver.s`**: La state machine PUT BYTE principale. Implementa un viewport ATASCII 40×24/80×60, gestendo i caratteri di controllo (EOL, CLEAR, TAB, ecc.) e il rendering diretto in VRAM.
*   **`vera_sys_es_hook.s`**: Installa gli handler sostitutivi per i dispositivi E: e S: agganciando la HATABS e aggiornando i puntatori PUT BYTE degli IOCB aperti. Gestisce anche il buffering dell'input e la traduzione dei codici tastiera POKEY grezzi in ATASCII.
*   **`vera_sys_vbi.s`**: Gestisce il lampeggio del cursore pilotato dal VBI (salvando la posizione del cursore e invertendo le nibble di colore foreground/background) e garantisce che i task in background non confliggano con le scritture VRAM in foreground.
*   **`vera_sys_loader.s`**: Bootstrap di installazione che gestisce la rilocazione dinamica del driver VERA nella memoria protetta (RAMTOP).

## Meccanismo di Rilocazione Robust RAMTOP (`vera_sys_loader.s`)

Il driver VERA implementa un sistema di rilocazione dinamica che permette di risiedere nella zona alta della RAM Atari, proteggendosi dalla sovrascrittura.

### Funzionamento del Relocator Robust
1.  **Calcolo Dinamico**: Il loader calcola dinamicamente la locazione di destinazione come `RAMTOP_PAGINA - Dimensione_Driver`, assicurando l'allineamento a pagina.
2.  **Protezione Memoria**: Subito dopo la copia e la rilocazione, il loader aggiorna `RAMTOP` ($6A) e `MEMTOP` ($2E5) per riflettere il nuovo limite della memoria utente, rendendo l'area occupata dal driver inaccessibile al sistema operativo e al BASIC.
3.  **Patching Indirizzi**: Utilizza una tabella di `fixups` generata a tempo di build (con tecnica build-twice) per aggiornare tutti gli indirizzi di memoria assoluti interni al driver in modo che puntino correttamente alla nuova locazione rilocata.
4.  **Re-inizializzazione Video**: Fornisce una procedura sicura per chiudere (`CLOSE #0`) e riaprire (`OPEN E:`) il device video, forzando il sistema operativo a ricostruire la Display List e la Screen RAM nello spazio appena ridotto sotto il nuovo `MEMTOP`, evitando corruzioni video all'avvio.
5.  **Sicurezza IRQ/VBI**: Tutte le operazioni critiche (scrittura RAMTOP, modifica vettori di sistema) sono protette disabilitando interrupt e DMA di ANTIC (`CRITIC` flag e `SDMCTL`/`DMACTL`) per prevenire corruzioni del bus durante la rilocazione.

## Implementazione lato emulatore (Atari800)

Il nucleo dell'emulatore `atari800` è stato esteso per supportare la periferica VERA PBI. L'implementazione lato emulatore (`src/pbi_verax16.c`, `src/pbi_verax16.h`) gestisce l'emulazione hardware del chip VERA e la sua integrazione nel bus PBI Atari.

### Funzionalità di emulazione principali:
*   **Memory Mapping:** Intercetta gli accessi al range `$D100-$D11F` per gestire le letture/scritture dei registri VERA, e gestisce il mapping della ROM handler su `$D800-$DFFF` tramite il latch del dispositivo PBI (`$D1FF`).
*   **Emulazione hardware:**
    *   **Registri VERA:** Emulazione completa dei registri VERA (porte indirizzo, porte dati, CTRL, IEN, ISR) e dei registri DC multiplexati.
    *   **VRAM:** Emula lo spazio di memoria VRAM da 128KB.
    *   **Coprocessore FX:** Emulazione completa del coprocessore VERA FX — tutti i registri sono leggibili e scrivibili con comportamento conforme all'hardware (vedi sezione dedicata).
    *   **Audio/SPI:** Emulazione dei canali audio PSG/PCM di VERA e dell'interfaccia SPI per l'emulazione della scheda SD.
*   **Integrazione bus:**
    *   **Gestione IRQ:** Gestisce le richieste di interrupt da VERA alla CPU Atari in base alle impostazioni IEN/ISR.
    *   **Configurazione:** Supporta argomenti CLI per abilitare la scheda (`-verax16`), specificare l'immagine ROM handler (`-verax16-rom`), collegare un'immagine scheda SD (`-verax16-sdcard`) e regolare il livello di log (`-verax16-debuglevel`).
*   **Gestione del ciclo di vita:** Gestisce gli stati di accensione/reset, garantendo che la VRAM sia inizializzata e la scheda sia correttamente abilitata/disabilitata sul bus.

## Coprocessore FX — Copertura completa registri (`src/pbi_verax16.c`)

Il coprocessore VERA FX espone i propri registri agli indirizzi fisici `$D109–$D10C`, multiplexati dal campo `DCSEL` del registro `CTRL` (`$D105`). Tutti i banchi da DCSEL=2 a DCSEL=6 sono emulati con comportamento read/write completo.

### Mappa registri FX per banco DCSEL

| DCSEL | Offset | Nome registro     | Descrizione                                              |
|-------|--------|-------------------|----------------------------------------------------------|
| 2     | $09    | FX_CTRL           | Modo Addr1, cache flags, trasparenza, 4-bit/nibble mode  |
| 2     | $0A    | FX_TILEBASE       | Base tile-data (addr>>9)[5:0], clip, 2-bit polygon       |
| 2     | $0B    | FX_MAPBASE        | Base tilemap (addr>>9)[5:0], dimensione mappa [1:0]      |
| 2     | $0C    | FX_MULT           | Controllo moltiplicatore e accumulatore                  |
| 3     | $09    | FX_X_INCR_L       | Incremento X bit[-2:-9] (virgola fissa 6.9, signed)      |
| 3     | $0A    | FX_X_INCR_H       | [7]=flag 32×, [6:0]=X_Incr[5:-1]                        |
| 3     | $0B    | FX_Y_INCR_L       | Incremento Y bit[-2:-9]                                  |
| 3     | $0C    | FX_Y_INCR_H       | [7]=flag 32×, [6:0]=Y_Incr[5:-1]                        |
| 4     | $09    | FX_X_POS_L        | Posizione X intera X_Pos[7:0]                            |
| 4     | $0A    | FX_X_POS_H        | [7]=X_Pos[-9], [2:0]=X_Pos[10:8]                        |
| 4     | $0B    | FX_Y_POS_L        | Posizione Y intera Y_Pos[7:0]                            |
| 4     | $0C    | FX_Y_POS_H        | [7]=Y_Pos[-9], [2:0]=Y_Pos[10:8]                        |
| 5     | $09    | FX_X_POS_S        | Frazione subpixel X_Pos[-1:-8] (read/write)              |
| 5     | $0A    | FX_Y_POS_S        | Frazione subpixel Y_Pos[-1:-8] (read/write)              |
| 5     | $0B    | FX_POLY_FILL_L    | Lunghezza span polygon fill — low byte (read-only)       |
| 5     | $0C    | FX_POLY_FILL_H    | Lunghezza span polygon fill — high byte (read-only)      |
| 6     | $09    | FX_CACHE_L / FX_ACCUM_RESET | Cache byte 0; lettura azzera `fx_mult_accumulator` |
| 6     | $0A    | FX_CACHE_M / FX_ACCUM       | Cache byte 1; lettura esegue un passo accumula     |
| 6     | $0B    | FX_CACHE_H        | Cache byte 2 (mult operando B low)                       |
| 6     | $0C    | FX_CACHE_U        | Cache byte 3 (mult operando B high)                      |

### Formato interno posizione (20 bit)

La variabile interna `fx_pixel_pos_x` (e analoga `fx_pixel_pos_y`) usa un layout a 20 bit:

```
bit 0       = X_Pos[-9]      (subpixel MSB, lettura/scrittura via DCSEL=4 POS_H bit7)
bits[8:1]   = X_Pos[-1:-8]   (frazione subpixel, DCSEL=5 POS_S)
bits[16:9]  = X_Pos[7:0]     (parte intera low, DCSEL=4 POS_L)
bits[19:17] = X_Pos[10:8]    (parte intera high, DCSEL=4 POS_H bit[2:0])
```

### Funzione helper `vera_fx_accumulate_step()`

Estratta come helper statico condiviso per evitare duplicazione:

```c
static void vera_fx_accumulate_step(void)
{
    int32_t m_result =
        (int16_t)((vera_fx_cache_get_byte(1) << 8) | vera_fx_cache_get_byte(0)) *
        (int16_t)((vera_fx_cache_get_byte(3) << 8) | vera_fx_cache_get_byte(2));
    if (fx_add_or_sub)
        fx_mult_accumulator -= m_result;
    else
        fx_mult_accumulator += m_result;
}
```

Viene richiamata sia dal write handler di `FX_MULT` (DCSEL=2, $D10C) quando il bit `VERA_FX_MULT_ACCUM` è attivo, sia come side-effect della lettura di `FX_ACCUM` ($D10A con DCSEL=6).

## Correzioni comportamento FX (`src/pbi_verax16.c`)

Nove bug comportamentali sono stati identificati tramite audit sistematico del VHDL (`fpga-vera/47.0.2/`) e corretti:

| # | Funzione / contesto | Difetto | Correzione |
|---|---------------------|---------|------------|
| 1 | `vera_fx_affine_prefetch()` | Shift tile `>>10` e sub-tile `>>7` errati di 2 bit → errore di scala 4× nell'indirizzo VRAM | Corretti a `>>12` (tile) e `>>9` (sub-tile), coerenti con il layout 20-bit della posizione |
| 2 | DATA1 read AFFINE / POLY_FILL — step posizione | Shift `4:11` applicato a un accumulatore di posizione in scala identica all'incremento → step 2048× eccessivo | Shift corretto a `5:0` (32× flag = 2^5; incremento diretto senza shift aggiuntivo) |
| 3 | DATA1 write LINE DRAW — step Bresenham | Fattore 32× usava shift 4 → scala 128× invece di 32× | Corretto a shift 6 (differenza 11-6=5, ovvero 2^5=32) |
| 4 | DATA1 write AFFINE — stub vuoto | Dopo la write su DATA1 in modalità affine, ADDR1 non veniva aggiornato e la posizione non avanzava | Aggiunto step posizione (shift `5:0`) + chiamata a `vera_fx_affine_prefetch()` |
| 5 | DATA1 write POLY_FILL — stub vuoto | Dopo la write su DATA1 in modalità polygon fill, ADDR1 rimaneva fermo | Aggiunto `vera_advance(1)` per avanzare al pixel successivo |
| 6 | DATA0 read POLY_FILL — ADDR1 ricalcolato erroneamente | Il path DATA0 ricalcolava ADDR1 dopo il passo di posizione; il VHDL aggiorna solo le posizioni X1/X2, non ADDR1 | Rimosso il ricalcolo di ADDR1; il path ora avanza solo le posizioni |
| 7 | DATA0 read POLY_FILL — cache byte cycling | Il cycling di `fx_cache_byte_index` scattava su DATA1 read (port errata) e ignorava `fx_cache_increment_mode` | Spostato su DATA0 read con logica 2-byte pingpong / 4-byte wrap coerente con il VHDL |
| 8 | `vera_fx_write_data()` — path non-cache | La transparenza (`fx_transparency_enabled`) era verificata solo nel path cache; le write dirette sovrascrivevano VRAM anche con byte/nibble zero | Aggiunta guard transparency nel path non-cache (`value==0` per 8-bit, `value & 0x0F == 0` per 4-bit) |
| 9 | `vera_chip_reset()` — stato iniziale posizione | `fx_pixel_pos_x` e `fx_pixel_pos_y` inizializzati a 0; il VHDL li inizializza a 256 (0.5 sub-pixel) | Corretti a 256, eliminando errori di arrotondamento al mezzo pixel alla prima operazione FX |

## Simboli condivisi (`vera_pbi_rom/vera_common.inc`)

Il file `vera_common.inc` è incluso da tutti i moduli assembly del progetto e centralizza indirizzi, costanti e bitmask. I nomi dei simboli sono allineati a 24 caratteri (il `=` cade alla colonna 25).

### Nuove costanti DCSEL (DCSEL=2–6)

```asm
VERA_DCSEL2  = $04   ; CTRL: banco FX 2 (FX_CTRL … FX_MULT)
VERA_DCSEL3  = $06   ; CTRL: banco FX 3 (FX_X/Y_INCR)
VERA_DCSEL4  = $08   ; CTRL: banco FX 4 (FX_X/Y_POS intero)
VERA_DCSEL5  = $0A   ; CTRL: banco FX 5 (FX_X/Y_POS_S, poly fill)
VERA_DCSEL6  = $0C   ; CTRL: banco FX 6 (FX_CACHE)
```

### Alias indirizzi registri FX (20 simboli)

Tutti i 20 registri FX (da `VERA_FX_CTRL` a `VERA_FX_CACHE_U`) sono definiti come alias degli indirizzi fisici `$D109–$D10C`. Gli stessi indirizzi fisici coprono banchi DCSEL diversi: il contesto è determinato dal valore scritto in `VERA_CTRL_REG` prima dell'accesso.

### Bitmask FX_CTRL (DCSEL=2, $D109) — 8 costanti

| Simbolo                  | Valore | Descrizione                                    |
|--------------------------|--------|------------------------------------------------|
| `VERA_FX_TRANSP`         | `$80`  | Trasparenza: salta byte/nibble zero in scrittura |
| `VERA_FX_CACHE_WR_EN`    | `$40`  | Scrittura cache 32-bit su ogni DATA write       |
| `VERA_FX_CACHE_FILL_EN`  | `$20`  | Riempimento cache da letture DATA0/DATA1        |
| `VERA_FX_ONE_BYTE_CACHE` | `$10`  | Cycling cache mono-byte (vs 4-byte)             |
| `VERA_FX_16BIT_HOP`      | `$08`  | Hop 16-bit su porta indirizzo 1                 |
| `VERA_FX_4BIT_MODE`      | `$04`  | Modalità pixel nibble (4 bit)                   |
| `VERA_FX_ADDR1_NORMAL`   | `$00`  | Addr1 normale (nessun helper FX)                |
| `VERA_FX_ADDR1_LINE`     | `$01`  | Addr1 helper line-draw                          |
| `VERA_FX_ADDR1_POLY`     | `$02`  | Addr1 helper polygon-fill                       |
| `VERA_FX_ADDR1_AFFINE`   | `$03`  | Addr1 helper affine-transform                   |

### Bitmask FX_MULT (DCSEL=2, $D10C) — 4 costanti

| Simbolo                    | Valore | Descrizione                                  |
|----------------------------|--------|----------------------------------------------|
| `VERA_FX_MULT_RESET_ACCUM` | `$80`  | Reset accumulatore a 0 (trigger write-only)  |
| `VERA_FX_MULT_ACCUM`       | `$40`  | Esegui un passo accumula sulla scrittura     |
| `VERA_FX_MULT_SUB_EN`      | `$20`  | Sottrai (invece di aggiungere) nell'accumulo |
| `VERA_FX_MULT_EN`          | `$10`  | Usa risultato moltiplicatore per write VRAM  |

## API di rilevamento hardware (`vera_pbi_rom/vera-tests/vera_detect.h`)

Il file header `vera_detect.h` espone due funzioni C per i programmi di test e di sistema:

```c
#define VERA_CARD_ID  ((unsigned int)0x5658)  /* 'V','X' */

static unsigned int vera_detect(void);
static unsigned int vera_require(void);
```

### Algoritmo di rilevamento

`vera_detect()` scrive due valori probe (`0x2A`, `0xD5`) nel registro `$D100` (VERA_ADDR_L) e li rilegge. Se entrambi corrispondono, la scheda è presente e la funzione restituisce `VERA_CARD_ID` (`0x5658`); altrimenti restituisce `0`. Al termine azzera anche i tre byte di indirizzo (`$D100–$D102`) per lasciare la porta in uno stato pulito.

La funzione usa indirizzi raw (`(volatile unsigned char *)0xD100`) internamente per evitare conflitti con i `#define` locali dei file che già definiscono `VERA_ADDR_L` con lo stesso valore.

`vera_require()` chiama `vera_detect()` e, se il risultato è zero, stampa un messaggio di errore con le istruzioni di invocazione corrette ed esce con `exit(1)`. In caso di successo restituisce il valore ID per uso opzionale dal chiamante.

## Programmi di test funzionali (`vera_pbi_rom/vera-tests/`)

Tutti i programmi di test in `vera-tests/` includono ora `vera_detect.h` e chiamano `vera_require()` all'avvio. Se la scheda VERA non è presente nell'emulatore, il test stampa un messaggio diagnostico con le opzioni CLI corrette ed esce, invece di bloccarsi o produrre output incomprensibile.

| File                      | Modifica                                                         |
|---------------------------|------------------------------------------------------------------|
| `test_font.c`             | `printf("VeraX16 detected (ID: 0x%04X)\n", vera_require());`    |
| `test_maze.c`             | `vera_require();` prima di qualsiasi accesso hardware            |
| `test_gradient_scroll.c`  | `vera_require();` prima di `fill_gradient()`                     |

Il valore restituito da `vera_require()` (`0x5658`, ovvero i caratteri ASCII `'V','X'`) funge da marker di identità — distinguibile da un semplice booleano e utile per log diagnostici.

## Sistema di build (`vera_pbi_rom/Makefile`)

Per compilare e verificare qualsiasi modifica ai sorgenti in `vera_pbi_rom/` usare sempre:

```
make -C vera_pbi_rom clean all atr
```

Questo target ricostruisce in sequenza: la ROM PBI handler (`vera_pbi_handler.rom`), il driver rilocabile (`VERA.SYS` / `AUTORUN.SYS`), tutti i programmi di test `.COM` nella directory `vera-tests/`, e l'immagine disco `vera_pbi.atr` contenente tutti i file pronti per l'uso nell'emulatore.
