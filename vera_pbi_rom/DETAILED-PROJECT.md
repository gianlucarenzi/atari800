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
    *   Inizializza i registri hardware VERA per una modalità VGA-compatibile 640×480, configurando il Layer 1 in modalità **80×60 caratteri** (tile 8×8).
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

## Implementazione lato emulatore (Atari800)

Il nucleo dell'emulatore `atari800` è stato esteso per supportare la periferica VERA PBI. L'implementazione lato emulatore (`src/pbi_verax16.c`, `src/pbi_verax16.h`) gestisce l'emulazione hardware del chip VERA e la sua integrazione nel bus PBI Atari.

### Funzionalità di emulazione principali:
*   **Memory Mapping:** Intercetta gli accessi al range `$D100-$D11F` per gestire le letture/scritture dei registri VERA, e gestisce il mapping della ROM handler su `$D800-$DFFF` tramite il latch del dispositivo PBI (`$D1FF`).
*   **Emulazione hardware:**
    *   **Registri VERA:** Emulazione completa dei registri VERA (porte indirizzo, porte dati, CTRL, IEN, ISR) e dei registri DC multiplexati.
    *   **VRAM:** Emula lo spazio di memoria VRAM da 128KB.
    *   **Coprocessore FX:** Emulazione parziale del coprocessore VERA FX per operazioni come tracciamento di linee, riempimento di poligoni e trasformazioni affini.
    *   **Audio/SPI:** Emulazione dei canali audio PSG/PCM di VERA e dell'interfaccia SPI per l'emulazione della scheda SD.
*   **Integrazione bus:**
    *   **Gestione IRQ:** Gestisce le richieste di interrupt da VERA alla CPU Atari in base alle impostazioni IEN/ISR.
    *   **Configurazione:** Supporta argomenti CLI per abilitare la scheda (`-verax16`), specificare l'immagine ROM handler (`-verax16-rom`) e collegare un'immagine scheda SD per l'interfaccia SPI (`-verax16-sdcard`).
*   **Gestione del ciclo di vita:** Gestisce gli stati di accensione/reset, garantendo che la VRAM sia inizializzata e la scheda sia correttamente abilitata/disabilitata sul bus.

## Problemi noti e correzioni

### Instabilità del cursore e corruzione visiva (fase iniziale)
Durante lo sviluppo, una race condition tra l'interrupt VBI (che gestisce il lampeggio del cursore) e le routine di manipolazione dello schermo (`scroll_up`, `do_delete_line`, `do_insert_line`, `do_delete_char`, `do_insert_char`) causava la scomparsa del cursore e corruzione visiva intermittente.

**Correzione:**
1.  **Invalidazione del cursore:** Aggiunte chiamate esplicite a `_vera_cursor_invalidate` all'inizio di tutte le routine di manipolazione dello schermo, per garantire che il cursore venga cancellato prima delle modifiche alla VRAM.
2.  **Preservazione dei registri:** Refactoring di `_vera_cursor_invalidate` per salvare e ripristinare tutti i registri CPU (`A`, `X`, `Y`) e il registro `VERA_CTRL`, garantendo che le routine chiamanti mantengano l'integrità del loro stato e delle impostazioni del controller VERA.

---

### Corruzione durante lo scroll ("  AB" inserito durante scroll lunghi)

**Sintomo:** Eseguendo `1 PRINT "AB" : 2 GOTO 1` e lasciando scorrere, apparivano occasionalmente righe `"  AB"` — la stringa "AB" era spostata di due colonne a destra.

**Causa radice:** `scroll_up` era racchiusa da `sei / lda #1; sta CRITIC / ... / lda #0; sta CRITIC / cli`. Questo azzerava `CRITIC` prematuramente, prima che `@done_putc` di `_VeraPutByte` avesse sincronizzato `COLCRS_OS`/`ROWCRS_OS` alla nuova posizione del cursore. Il VBI differito scattava nella finestra, leggeva il valore stantio `COLCRS_OS=2`, scriveva `CURSOR_X=2` nel blocco di controllo, e il carattere successivo veniva renderizzato alla colonna 2.

**Correzione (`vera_driver.s` — `scroll_up`):** Rimossi completamente `sei`, set/clear di `CRITIC` e `cli` da `scroll_up`. Il salvataggio/ripristino di DMACTL rimane; il contesto di protezione CRITIC è fornito da `_CallVeraApiService` (che racchiude l'intera chiamata PUT BYTE, incluso qualsiasi scroll innescato al suo interno).

---

### Cursore che scompare dopo BREAK / jmp $A000

Tre bug separati contribuivano al problema.

#### Bug 1 — `cursor_tick` saltava il ridisegno quando la posizione non cambiava ma il cursore era stato cancellato

**Sintomo:** Dopo che `scroll_up` chiamava `_vera_cursor_invalidate` (che imposta `cursor_drawn=0`), se la posizione del cursore non cambiava tra un tick e l'altro, `cursor_tick` eseguiva il percorso rapido `beq @done` e non ridisegnava mai il cursore.

**Correzione (`vera_sys_vbi.s` — `cursor_tick`):** Sostituito il percorso rapido `beq @done` (salta tutto quando la posizione corrisponde) con un controllo esplicito di `cursor_drawn`: se `cursor_drawn=0` alla stessa posizione, chiama `cursor_draw` per ripristinare il cursore visibile. Gestisce il caso in cui `_vera_cursor_invalidate` (chiamata da `scroll_up`) cancella il cursore senza spostarlo.

#### Bug 2 — `cursor_draw` rifiutava posizioni di riga valide 25–59

**Sintomo:** Il cursore era invisibile su qualsiasi riga oltre la riga 24. Il viewport 80×60 usa le righe 0–59, ma il controllo OOB confrontava con `SCREEN_ROWS` (25) invece di `SCREEN_ROWS_VIEW` (60).

**Correzione (`vera_sys_vbi.s` — `cursor_draw`):** Cambiato `cmp #SCREEN_ROWS` in `cmp #SCREEN_ROWS_VIEW` (60).

#### Bug 3 — L'hook CASINI eseguiva un salto attraverso il puntatore null `_vera_saved_casini`

**Sintomo:** Dopo un `jmp $A000` (cold start da cartridge), il sistema andava in crash prima che `common_reinit` potesse reinstallare il VBI. Il cursore non riappariva nemmeno dopo il recupero dal crash.

**Causa radice:** `_vera_saved_casini` vale `$0000` quando non era installato nessun handler CASINI precedente. La correzione originale (errata) chiamava `jmp (_vera_saved_casini)` incondizionatamente, saltando nelle variabili RAM di pagina zero Atari.

**Correzione (`vera_sys_dosini.s` — `_vera_casini_asm_hook`):** Aggiunto un controllo null: `lda _vera_saved_casini; ora _vera_saved_casini+1; beq @done` — la tail-call viene saltata completamente se il puntatore salvato è zero.

---

### Cursore che scompare uscendo dal DOS verso il BASIC ("B" da DUP.SYS)

**Sintomo:** Entrando nel DOS (DUP.SYS) e tornando al BASIC con l'opzione "B", il cursore risultava invisibile. BREAK (warm start) funzionava correttamente dopo le correzioni precedenti.

**Causa radice:** Atari DOS (DUP.SYS) installa il proprio handler VBI differito (`VVBLKD`), sovrascrivendo il nostro. Quando l'utente seleziona "B", DOS esegue `JMP (DOSVEC)` — un salto diretto al BASIC senza warm/cold start OS. Né gli hook `DOSINI` né `CASINI` scattano. L'IOCB 0 è già aperto, quindi `vera_editor_open` non viene mai chiamata. Il prompt "READY" del BASIC è il primo output dopo la transizione.

Due correzioni parziali sono state applicate in sequenza:

**Correzione parziale — `vera_editor_open` (`vera_sys_es_hook.s`):** Aggiunto `jsr _InitVbi` (con `sei` + disabilitazione/ripristino del bit 6 di NMIEN) a `vera_editor_open`. Reinstalla il VBI quando l'OS apre E: durante qualsiasi warm o cold start OS, coprendo il percorso BREAK. Non copre il percorso diretto `JMP (DOSVEC)` perché IOCB 0 è già aperto.

**Correzione completa — `ensure_vbi` in `vera_editor_put` / `vera_screen_put` (`vera_sys_es_hook.s`):** Aggiunto un controllo VBI lazy all'ingresso di entrambi gli handler PUT BYTE. Ad ogni chiamata, `ensure_vbi` confronta `VVBLKD` con l'indirizzo dell'handler rilocato letto da `__VERA_EXPORTS__+EXP_VBI_HANDLER`. Se differiscono, reinstalla l'handler VBI con la stessa protezione SEI/NMIEN usata in `vera_editor_open`. Il primo output del BASIC dopo qualsiasi transizione è il prompt "READY", quindi il VBI viene reinstallato prima che l'utente possa interagire.

Questo pattern è robusto contro qualsiasi percorso di rientro futuro che bypassa la sequenza di init OS.

---

### Banner di avvio scomparso dopo aggiunta di codice al driver (`vera_sys_loader.s`)

**Sintomo:** Dopo aver aggiunto la funzione `ensure_vbi` in `vera_sys_es_hook.s` (~30 byte in più nel segmento CODE), il banner "DEVICE DRIVER INSTALLED" non appariva più all'avvio — la pausa da 2 secondi era ancora presente, ma lo schermo rimaneva vuoto durante l'attesa.

**Causa radice:** Il segmento `LOWBSS` è di tipo `bss` nel linker config (`vera_sys.cfg`) e non produce byte nel file binario — il loader copia solo i segmenti EXPORTS+CODE+RODATA+DATA tramite `copy_block`, e poi avanza MEMLO di `PATCH_BODY_TOTAL_SIZE` (che include la BSS). I byte del segmento `LOWBSS` in RAM non vengono mai azzerati dal loader.

La variabile `first_init` si trova nel segmento `LOWBSS` di `vera_driver.s`. Il suo offset rispetto a `MEMLO` è uguale alla dimensione del file binario (`PATCH_BODY_FILE_SIZE`). Ogni volta che il codice del driver cresce, `first_init` si sposta a un indirizzo RAM più alto. Se quell'indirizzo conteneva già il valore `1` (scritto da una sessione precedente con una versione del driver più piccola), `_vera_warm_reinit` trovava `first_init=1` e saltava la visualizzazione del banner.

Lo stesso bug latente colpisce qualsiasi variabile di stato in `LOWBSS` (state del cursore, vettori salvati, ecc.) nel momento in cui il codice cresce e la BSS si sposta su memoria già usata.

**Correzione (`vera_sys_loader.s`):** Aggiunta la routine `zero_block` e una chiamata ad essa immediatamente dopo `copy_block` (step 2b). Calcola la dimensione della BSS come `PATCH_BODY_TOTAL_SIZE − PATCH_BODY_FILE_SIZE` e azzera tutti i byte da `exp_base + PATCH_BODY_FILE_SIZE` in avanti. Questo garantisce che `first_init` e tutte le altre variabili `LOWBSS` partano sempre da zero ad ogni caricamento di `VERA.SYS`, indipendentemente dalle dimensioni del codice.

---

### Ottimizzazione screen clear con disabilitazione DMA ANTIC (`vera_driver.s`, `vera_pbi_handler.s`)

Le routine di pulizia schermo (`do_clear`, `_pbi_clear_screen` in `vera_driver.s` e `CLEAR_SCREEN`/`PBI_CLEAR_SCREEN` in `vera_pbi_handler.s`) ora salvano il registro `DMACTL` ($D400), disabilitano il DMA di ANTIC per tutta la durata dell'operazione, quindi ripristinano il valore originale. Questo è lo stesso pattern già usato in `scroll_up` e impedisce che ANTIC contenda il bus di memoria con la CPU durante le scritture intensive in VRAM.

---

### Passaggio a 80x60 Nativo e Pulizia ROM (`vera_pbi_handler.s`)

Per uniformare l'esperienza visiva fin dal boot e ottimizzare lo spazio in ROM, sono state apportate le seguenti modifiche all'inizializzazione hardware:

1.  **Inizializzazione 80x60:** La ROM PBI ora configura il Layer 1 di VERA per utilizzare tile 8×8 invece di 8×16. Questo imposta una risoluzione di testo nativa di 80 colonne per 60 righe già durante la visualizzazione del banner di boot.
2.  **Rimozione Logo Butterfly:** Le tile grafiche del logo "butterfly" sono state rimosse dai dati del font di boot e dalla routine di visualizzazione. Il banner ora mostra esclusivamente le informazioni testuali (versione FW e modello host), rendendo il codice più snello e focalizzato.
3.  **Ottimizzazione Font Loading:** La routine `LOAD_BOOT_FONT` è stata aggiornata per supportare l'allineamento a 8 byte delle tile 8×8 in VRAM. Carica solo i 27 caratteri strettamente necessari per le scritte di boot.
4.  **Ottimizzazione Simboli ZP:** Le definizioni dei simboli Zero Page (`TMP0-2`, `TMP_PTR`) sono state spostate all'inizio del file sorgente. Questo permette all'assemblatore `ca65` di utilizzare istruzioni con indirizzamento Zero Page (più corte e veloci) invece di quello assoluto, garantendo che l'intera ROM rientri nel limite fisico di 2048 byte nonostante le evoluzioni del driver.

---

### Bug 6502 page-crossing in `jmp (abs)` — `vera_sys_dosini.s`

**Sintomo (errore di build):** Dopo l'aggiunta del codice DMACTL alle routine di pulizia (~24 byte in più nel segmento CODE), ca65 emetteva:

```
vera_sys_dosini.s:52: Error: Assertion failed: "jmp (abs)" across page border
```

**Causa radice:** L'hardware 6502 ha un bug noto: `jmp ($xxFF)` legge il byte alto dall'indirizzo `$xx00` invece di `$(xx+1)00`. ca65 rileva staticamente questa condizione in fase di assemblaggio. Ogni volta che il codice cresce, l'indirizzo nominale (base `$A000`) di `_vera_saved_dosini` può cadere esattamente a `$xxFF`, scatenando l'errore.

Approcci scartati:
- **ZP-indiretto `jmp ($CB)`**: $CB è libero dopo `common_reinit`, ma non garantisce l'uso esclusivo — un IRQ potrebbe corrompere quei byte nella finestra tra `sta $CC` e `jmp ($CB)`.
- **Aggiunta di byte di padding**: spostare l'offset del vettore con codice artificiale è fragile; la prossima modifica al codice riproduce il problema.

**Correzione (`vera_sys_dosini.s`):** Sostituito `jmp (_vera_saved_dosini)` e `jmp (_vera_saved_casini)` con **self-modifying code**: l'indirizzo del vettore viene scritto a runtime nei due byte operando di una istruzione `jmp $0000` assoluta che si trova nel segmento CODE:

```asm
_vera_dosini_asm_hook:
    jsr common_reinit
    lda _vera_saved_dosini
    sta @jmp+1
    lda _vera_saved_dosini+1
    sta @jmp+2
@jmp:
    jmp $0000       ; operand patchato a runtime — JMP assoluto, non indiretto
```

Perché questa soluzione è robusta:
- `jmp $0000` è opcode `$4C` (JMP assoluto diretto), **non** `$6C` (indiretto) — il bug page-crossing non si applica.
- `@jmp` è un'etichetta locale nel segmento CODE; il suo offset dal base è fisso a link-time e non dipende dalla dimensione del codice.
- Non usa ZP, eliminando qualsiasi problema di ownership o race con gli IRQ.
- Il relocator non tocca il `$0000` nel binario (non è un riferimento a simbolo) — viene patchato esclusivamente dal codice sopra riportato.

---

### Bug del Backspace limitato nell'Editor (`vera_sys_es_hook.s`)

**Sintomo:** Durante l'editing di una riga, il tasto Backspace smetteva di cancellare non appena si raggiungeva il punto in cui il cursore era stato riposizionato manualmente (ad esempio dopo essersi spostati con le frecce). L'utente era costretto a usare le frecce per tornare indietro, rompendo il flusso naturale dello "Screen Editor".

**Causa radice:** L'handler `E:` manteneva variabili locali (`input_col0`, `input_wr`) per tracciare l'inizio della sessione di input corrente e imponeva un blocco artificiale al Backspace basato su queste. Poiché le frecce cursore resettavano queste variabili alla nuova posizione X, il driver "dimenticava" che la riga logica continuava a sinistra.

**Correzione:** Rimosse le variabili `input_col0` e `input_wr` e la relativa logica di clamping. Il driver ora permette al Backspace di retrocedere liberamente fino al margine sinistro (`LMARGN`). La coerenza dei dati è garantita dal fatto che, alla pressione di RETURN, l'intero contenuto della riga viene comunque scansionato direttamente dalla VRAM di VERA.

---

### Allineamento dinamico dei banner di boot (`vera_driver.s`, `vera_pbi_handler.s`)

**Sintomo:** Le scritte del banner apparivano disallineate o troppo a sinistra (colonna 0) a causa dell'uso di valori non inizializzati o hardcoded.

**Correzione:**
1.  **Driver (`vera_driver.s`):** La scritta "DEVICE DRIVER INSTALLED" ora preleva la colonna di inizio dalla locazione OS `LMARGN` ($52), garantendo l'allineamento con il resto del sistema.
2.  **ROM PBI (`vera_pbi_handler.s`):** Le routine `PRINT_VERSION_LINE` e `PRINT_HOST_LINE` sono state aggiornate per leggere `LMARGN` a runtime invece di usare il valore fisso colonna 2.
3.  **Posizionamento:** La riga del banner del driver è stata spostata alla riga **5** (tramite `READY_ROW` in `vera_common.inc`) per posizionarsi correttamente sotto le informazioni della ROM.

---

### Consolidamento e pulizia
*   **`vera_common.inc`:** Riallineamento estetico rigoroso (simboli a 24 caratteri, '=' a colonna 25) per migliorare la manutenibilità.
*   **Refactoring:** Rimossi i residui di codice inutilizzato dopo la semplificazione della routine di input.

**Sintomo:** Avviando VERA.SYS senza la ROM PBI handler (`vera_pbi_handler.rom`), lo sfondo risultava blu ma i caratteri erano invisibili. I comandi venivano comunque eseguiti (al buio).

**Causa radice:** `_vera_warm_reinit` nel driver non configura i registri hardware di VERA: Layer 1 e il display composer vengono inizializzati esclusivamente da `INIT_VERA_SCREEN` nella ROM PBI. In assenza di quella ROM, VERA rimane nello stato di reset:

- `VERA_DC_VIDEO = $00`: nessun output VGA attivo, nessun layer abilitato
- `VERA_L1_MAPBASE` / `VERA_L1_TILEBASE`: puntano a `$0000` anziché a `SCREEN_ADDR` / `CHARSET_ADDR`
- `VERA_L1_CONFIG`: non configurato per la tilemap 128×64
- `VERA_DC_HSCALE` / `VERA_DC_VSCALE`: `$00` (scaling disattivato)

Il blue di sfondo era dovuto al valore di reset di `VERA_DC_BORDER`. Il font veniva caricato correttamente in VRAM da `vera_load_font`, ma poiché questa routine legge `VERA_DC_VIDEO`, lo salva, e lo ripristina invariato (`$00`), il Layer 1 restava disabilitato dopo il caricamento.

**Correzione (`vera_driver.s`):** Aggiunta la routine `vera_init_hw` chiamata all'inizio di `_vera_warm_reinit`, prima di `vera_load_font`. La routine configura tutti i registri VERA necessari — identico a ciò che fa `INIT_VERA_SCREEN` nella ROM PBI:

- Layer 1: `CONFIG = VERA_MAP_128x64`, `MAPBASE = SCREEN_MAPBASE`, `TILEBASE = SCREEN_TILEBASE`, scroll azzerati
- DC bank 1 (DCSEL=1): `HSTART/HSTOP/VSTART/VSTOP` per l'area attiva 640×480
- DC bank 0 (DCSEL=0): `DC_VIDEO = VGA | LAYER1_EN`, `HSCALE = VSCALE = $80`, `BORDER = $06`

Le scritture sono idempotenti: se la ROM PBI è presente e ha già configurato VERA, riscrivere gli stessi valori non produce effetti collaterali.

---

### Consolidamento delle equate (`vera_common.inc`)

Tutti gli indirizzi dei registri hardware, le costanti di layout dello schermo, i codici di controllo ATASCII, gli offset del blocco VCTL e le equate OS precedentemente dispersi nei singoli file `.s` sono stati consolidati in `vera_common.inc`. Tutti i moduli includono questo file; i duplicati per-modulo sono stati rimossi.

Aggiunte rilevanti:
- `SCREEN_ROWS_VIEW = 60` (distinto da `SCREEN_ROWS = 25`)
- `ROWCRS_OS = $54`, `COLCRS_OS = $55` (shadow cursore OS per la sincronizzazione VBI)
- Tabella completa dei codici di controllo ATASCII
- Offset `VERACTL_*` e bitmask `VCTL_FLAG_*`

---

### Parametrizzazione build: font 8×16 (80×30) o 8×8 (80×60)

Il driver supporta due modalità di visualizzazione selezionabili a tempo di build tramite la variabile `FONT_SIZE` del Makefile:

| `FONT_SIZE` | Tile | Viewport | Comando |
|---|---|---|---|
| `8x16` (default) | 8×16 px | 80 × 30 righe | `make` oppure `make FONT_SIZE=8x16` |
| `8x8` | 8×8 px | 80 × 60 righe | `make FONT_SIZE=8x8` |

**Implementazione:**

La selezione è centralizzata in un unico blocco condizionale all'inizio di `vera_common.inc`, prima della sezione delle dimensioni viewport. Quando il Makefile passa `-D FONT_8X8=1` a ca65, il blocco `.ifdef FONT_8X8` definisce:
- `TILE_HEIGHT = 8`, `FONT_PAGES = 4`, `SCREEN_ROWS = 60`
- `SCREEN_TILEBASE_REG = <(CHARSET_ADDR>>9)` — registro L1_TILEBASE senza il bit 1 (tile height = 8 px)

Il ramo `.else` (default 8×16) definisce:
- `TILE_HEIGHT = 16`, `FONT_PAGES = 8`, `SCREEN_ROWS = 30`
- `SCREEN_TILEBASE_REG = <(CHARSET_ADDR>>9) | 2` — bit 1 = 1 (tile height = 16 px)

Le costanti `SCREEN_ROWS_VIEW = SCREEN_ROWS` e `SCREEN_COLS_VIEW = SCREEN_COLS` (invariato a 80) propagano automaticamente la scelta a tutti i bounds check di scroll, cursore e margini in `vera_driver.s`, `vera_sys_vbi.s` e `vera_sys_es_hook.s` senza ulteriori modifiche.

Le due sole righe modificate nel driver sono:
- `vera_driver.s` — `vera_init_hw`: `lda #SCREEN_TILEBASE_REG` (sostituisce `lda #(SCREEN_TILEBASE | 2)`)
- `vera_driver.s` — `vera_load_font`: `ldx #FONT_PAGES` (sostituisce `ldx #8`)

In `vera_sys_font.s` il `.incbin` è condizionale: include `font8x8.bin` (1024 B) o `font8x16.bin` (2048 B) in base alla stessa define.

**Isolamento del ROM handler:**

L'handler OS ROM (`vera_pbi_handler.s`) è sempre compilato con `-D FONT_8X8=1` forzato nella sua regola Makefile, indipendentemente da `FONT_SIZE`. Questo garantisce che il ROM operi sempre nel contesto 8×8 / 80×60 — che è il suo comportamento hardware fisso — e non venga mai influenzato dalla scelta del driver. Il driver `vera_init_hw` sovrascrive poi i registri VERA con la configurazione corretta per la modalità compilata (le scritture sono idempotenti se le due modalità coincidono).

---

### Velocità tastiera e click audio (`vera_driver.s`, `vera_sys_es_hook.s`, `vera_common.inc`)

**Velocità di ripetizione tasti:**

Il driver imposta all'avvio valori più reattivi nelle variabili RAM XL/XE che governano la ripetizione tasti (`KRPDEL`/$02D9 e `KEYREP`/$02DA), già definite in `atari.inc`. Le costanti configurabili in `vera_common.inc` sono:

| Costante | Valore | Frame a 60Hz | Tempo | Default OS |
|---|---|---|---|---|
| `KBD_KRPDEL_FAST` | `$18` | 24 | ≈ 0.4 s | `$30` = 0.8 s |
| `KBD_KEYREP_FAST` | `$03` |  3 | ≈ 0.05 s (20 cps) | `$06` = 0.1 s |

Le scritture avvengono in `_vera_warm_reinit` subito dopo `vera_load_font`, prima di qualsiasi output sullo schermo. Essendo variabili RAM ordinarie, l'utente può sovrascriverle in qualsiasi momento da BASIC (`POKE 729,X` / `POKE 730,X`) senza reboot.

**Click audio:**

L'infrastruttura POKEY per il click (`_vera_trigger_click` / `click_tick` nel VBI) era già presente e funzionante, ma non veniva mai invocata. Nel loop di polling della tastiera (`vera_sys_es_hook.s`), subito dopo il consumo di `CH`, è stato aggiunto il controllo del flag OS `NOCLIK` ($02DB) e la chiamata al click:

```asm
lda NOCLIK
bne @no_click
jsr _vera_trigger_click
@no_click:
```

Il click scatta ad ogni pressione fisica di tasto (incluse le CAPS toggle), esattamente come nel driver OS `emuos`. L'utente può disabilitarlo con `POKE 731,1`, ripristinarlo con `POKE 731,0`.

---

### Correzione bug percorso ESC: caratteri ≥ 128 e reverse video (`vera_driver.s`)

**Sintomo:** il programma di test `10 FOR I=0 TO 255:PRINT CHR$(27);CHR$(I);:NEXT I` mostrava:
- Caratteri 128–255: pixel casuali invece del glifo in reverse video.
- Caratteri 0–31: non visualizzati (problema di contenuto font, vedi sotto).

**Causa:** il percorso ESC in `_VeraPutByte` chiamava `jsr print_literal` con il byte grezzo, senza estrarre il bit 7. Per indici ≥ 128, VERA cercava il glifo a posizioni 128–255 della tabella font, che contiene solo 128 glifi (0–127): l'accesso cadeva fuori area, leggendo VRAM non inizializzata → pixel casuali. Inoltre, `putc_inverse` non veniva impostato, rendendo impossibile il reverse video anche per i caratteri validi.

**Fix (stessa logica del percorso stampabile standard):**

```asm
; Percorso ESC — prima del fix:
pla
jsr print_literal
jmp @done_putc

; Dopo il fix:
pla
pha
and #$80
sta putc_inverse        ; $80 = inverse, $00 = normale
pla
and #$7F                ; riconduce l'indice nel range 0–127
jsr print_literal
jmp @done_putc
```

**Font e caratteri 0–31:** entrambi i font (`font8x8.bin` e `font8x16.bin`) sono stati aggiornati con `vera_font_editor.py` per includere i semigrafici ATASCII alle posizioni 0–31 (cornici, frecce, simboli). `ESC + CHR$(X)` funziona correttamente per tutti e 256 i caratteri ATASCII (128 normali + 128 inverse video).

---

### Eliminazione lag tastiera e perdita caratteri (`vera_driver.s`)

**Sintomo:** Durante la digitazione veloce, alcuni caratteri venivano persi (es. "Vera" diventava "Vra"). Il problema si accentuava durante operazioni video onerose come lo scroll.

**Causa radice:** Il dispatcher API `_CallVeraApiService` utilizzava `sei` e `cli` per racchiudere l'intera operazione di rendering (inclusi gli scroll). Questo disabilitava gli interrupt della CPU per finestre temporali troppo lunghe, impedendo all'IRQ della tastiera di catturare i tasti premuti in rapida successione. Poiché l'hardware POKEY non ha un buffer FIFO, i tasti arrivati durante la `sei` venivano sovrascritti o persi.

**Correzione:** Rimossi `sei` e `cli` da `_CallVeraApiService`. La protezione dell'integrità dei registri VERA rispetto al Vertical Blank Interrupt (VBI) è garantita dal flag `CRITIC`. Poiché l'handler della tastiera non accede all'hardware VERA, è sicuro permettere l'esecuzione degli IRQ durante il rendering video.

---

### Configurazione feedback audio: BELL vs Keyboard Click

**Modifica:** Il feedback sonoro della tastiera ("click") è stato disabilitato per default per non interferire con la velocità di digitazione e per preferenza utente. È stata invece implementata la funzione BELL.

1. **Rimozione Click:** Eliminata la chiamata a `_vera_trigger_click` dal polling loop della tastiera in `vera_sys_es_hook.s`.
2. **Implementazione BELL (`vera_driver.s`):** La routine `do_bell` (associata al carattere ATASCII `$FD` / 253) ora invoca direttamente `_vera_trigger_click`. Questo permette ai programmi di generare un feedback sonoro esplicito tramite `PRINT CHR$(253);` pur mantenendo silenziosa la normale digitazione.

---

### Pipeline tastiera POKEY via IRQ diretto (`vera_sys_es_hook.s`, `vera_sys_vbi.s`)

**Motivazione:** Il vecchio GET handler di `E:` interrogava la shadow OS `CH` ($02FC), che viene aggiornata dal VBI di sistema. Questo comportava un ritardo di un frame e dipendeva dall'handler VBI OS per il debouncing. Per rendere il driver completamente autonomo, la pipeline tastiera è stata riscritta sull'IRQ hardware POKEY diretto.

**Architettura:**

- **`_vera_kbd_irq_handler`** (VKEYBD = $0208): sostituisce l'handler OS `KeyboardIRQ`. Viene invocato dall'IRQ dispatcher OS (`irq816.s`) con la convenzione `pha; jmp (vkeybd)` — il dispatcher salva solo A sullo stack; il nostro handler salva e ripristina X e Y, esce con `pla; rti`. Legge il registro hardware `KBCODE` ($D209), traduce tramite `kbcode_table` (256 entry: 4 blocchi × 64 = unshifted / SHIFT / CTRL / CTRL+SHIFT), applica CAPS LOCK, e deposita il carattere ATASCII nel ring buffer.

- **Ring buffer** (`kbd_ring_buf`, 16 slot, power-of-2): `kbd_ring_wr` aggiornato solo dall'IRQ, `kbd_ring_rd` aggiornato solo dal GET handler; pieno quando `(wr+1)&$0F == rd`. Nessuna contesa: il 6502 esegue gli IRQ con I=1, e il GET handler (foreground) usa SEI/CLI solo nel tick VBI.

- **`_vera_kbd_repeat_tick`** (chiamato dal VBI differito ogni frame): se `SKSTAT` bit 2 = 0 (tasto tenuto) e `KBCODE` coincide con `kbd_repeat_raw`, decrementa `kbd_repeat_cnt`. A zero, emette un carattere di ripetizione e ricarica `KEYREP`. Usa SEI/CLI perché il VBI differito gira con interrupt abilitati e il ring può essere scritto concorrentemente dall'IRQ tastiera.

- **GET handler (`vera_editor_get @poll`)**: semplice busy-wait su `kbd_ring_rd != kbd_ring_wr`; legge il carattere già tradotto e con CAPS applicato, avanza `kbd_ring_rd`.

- **`_install_kbd_irq`**: scrive l'indirizzo rilocato di `_vera_kbd_irq_handler` (letto dalla EXPORTS table, che è già stata patchata dal relocator) in VKEYBD. Chiamata da `_install_es_hooks` al primo boot e ad ogni warm start.

**Bug risolti durante lo sviluppo:**

1. **Crash al primo tasto (schermo verde):** il dispatcher OS fa `pha; jmp (vkeybd)` — NON una JSR. Il primo tentativo usava `rts` come uscita; il 6502 saltava al valore di A salvato sull'stack (garbage). Correzione: uscita con `pla; rti`, salvando X e Y a mano all'entrata.

2. **Ring buffer scriveva l'indice invece del carattere:** in `@push_char`, la sequenza `lda wr; adc #1; and #$0F` per il controllo "pieno" sovrascriveva A (che conteneva il carattere) prima di `sta kbd_ring_buf,x`. Correzione: `tax` (salva char in X) prima del calcolo di `new_wr`; `pha/pla` per proteggere `new_wr` durante la scrittura.

3. **Stesso carattere garbage per ogni tasto (bug nel GET handler):** in `@poll`, dopo `lda kbd_ring_buf,x` (A = char), la sequenza `inx; txa; and #$0F; sta kbd_ring_rd` riscriveva A con il nuovo indice di lettura. Il dispatch successivo (`cmp #ATASCII_EOL` ecc.) operava sul valore dell'indice (1, 2, 3…) invece che sul carattere. Correzione: `tay` subito dopo `lda kbd_ring_buf,x` per salvare il carattere, `tya` dopo `sta kbd_ring_rd` per ripristinarlo in A.

---

### Corruzione font alla prima riga: glifi 0–15 (8×16) o 0–31 (8×8) non stampabili (`vera_driver.s`)

**Sintomo:** Il programma `test_font.c` — che invia `ESC + CHR$(i)` per `i=0..255` — mostrava garbage nei primi 256 byte del display invece dei glifi di test. Tutti i caratteri ASCII "normali" (32–127) apparivano correttamente; i caratteri con indice basso (0–15 per 8×16, 0–31 per 8×8) producevano pixel casuali.

**Causa radice:** La routine `do_clear` in `vera_driver.s` usava il classico pattern 6502 per un loop di 256 iterazioni:

```asm
ldy #0
@col_loop:
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    dey
    bne @col_loop
```

Con `ldy #0; dey` l'indice parte da −1 = 255 e scende: il loop esegue **256 iterazioni**. Ogni iterazione scrive 2 byte (char + color), producendo **512 byte per riga**.

La tilemap VERA è però di soli **128 tile × 2 byte = 256 byte per riga**. I restanti 256 byte scritti da `do_clear` cadono nella riga successiva in VRAM. Per le righe 0–62 questo sovrascrive solo la parte iniziale della riga seguente (corretta poi dall'iterazione successiva del `@row_loop`). Per **la riga 63** (l'ultima, a VRAM offset `$1EF00`), i 256 byte in eccesso cadono a `$1F000–$1F0FF`, che è esattamente **`CHARSET_ADDR`**: i primi 256 byte del font in VRAM.

Con font 8×16 (16 byte/glifo): 256 ÷ 16 = **16 glifi** corrotti → indici 0–15.
Con font 8×8 (8 byte/glifo): 256 ÷ 8 = **32 glifi** corrotti → indici 0–31.

La corruzione avveniva ad ogni `do_clear`, incluso il primo avvio tramite `_vera_warm_reinit` che chiama `do_clear` per ripulire lo schermo.

**Evidenza:** La routine `_pbi_clear_screen` in `vera_pbi_handler.s` usava già correttamente `ldx #MAP_COLS` (128) nello stesso tipo di loop, confermando che `MAP_COLS` è il valore atteso.

**Correzione (`vera_driver.s` — `do_clear`):** Sostituito `ldy #0` con `ldy #MAP_COLS`:

```asm
; Prima (bug — 256 iterazioni × 2 byte = 512 byte/riga):
    ldy #0
@col_loop:
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    dey
    bne @col_loop

; Dopo (fix — 128 iterazioni × 2 byte = 256 byte/riga):
    ldy #MAP_COLS               ; 128 tile × 2 byte = 256 byte per riga
@col_loop:
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    dey
    bne @col_loop
```

Con `MAP_COLS = 128`, la prima iterazione parte da 127 (non da 255): il loop esegue esattamente 128 iterazioni per un totale di 256 byte per riga, senza mai sconfinare in `CHARSET_ADDR`. I glifi 0–31 (e le loro versioni in reverse video 128–159) sono ora stampabili correttamente via `ESC + CHR$(x)`.

---

### Correzione bug editing multi-riga e sincronizzazione scroll (`vera_sys_es_hook.s`, `vera_driver.s`)

**Sintomo:** Durante l'input di una riga logica lunga (che occupa due righe fisiche), l'editing diventava incoerente se la schermata scorreva verso l'alto o se si utilizzavano i tasti `DELETE` o `INSERT`. Il Backspace poteva smettere di funzionare correttamente o puntare a righe fisiche errate.

**Causa radice:**
1.  **Perdita di sincronia fisica:** Il driver `E:` traccia la riga fisica di inizio input in `input_start_row`. Quando il sistema esegue uno scroll verso l'alto (es. premendo RETURN in fondo allo schermo o arrivando a fine riga nell'ultima riga dello schermo), tutto il contenuto video sale di una riga, ma `input_start_row` rimaneva invariato, puntando alla riga fisica precedente (ormai occupata da altro testo).
2.  **Mancanza di logica multi-riga per DELETE/INSERT:** I tasti `DELETE CHAR` ($FE) e `INSERT CHAR` ($FF) non erano gestiti esplicitamente dal loop di input dell'editor, ricadendo nelle routine base del driver che operano su una singola riga fisica, ignorando il ponte tra riga 1 e riga 2 della riga logica.

**Correzione:**
1.  **Scroll Hook:** Aggiunta la routine `_vera_scroll_hook` nell'handler `E:`, esportata e richiamata dal driver `vera_driver.s` all'inizio di ogni operazione `scroll_up`. La routine decrementa `input_start_row` ad ogni scroll, mantenendo l'editor sempre allineato alla reale posizione fisica del testo in VRAM.
2.  **Editing Logico:** Implementate le routine `do_logical_delete` e `do_logical_insert`. Queste routine gestiscono lo shift dei caratteri attraverso il confine tra le due righe fisiche (bridge):
    *   In `DELETE`, se il cursore è sulla riga 1 e la riga logica prosegue sulla riga 2, il primo carattere della riga 2 viene "tirato" nella colonna 79 della riga 1.
    *   In `INSERT`, se la riga logica occupa due righe fisiche, il carattere in colonna 79 della riga 1 viene "spinto" nella colonna 0 della riga 2.
3.  **Ottimizzazione VRAM:** Refactoring di `bs_shift_and_blank` e `ins_shift_and_blank` per utilizzare i due data port indipendenti di VERA (`DATA0` e `DATA1`). Questo elimina la necessità di cambiare continuamente il registro `VERA_CTRL` durante lo shift di una riga, riducendo drasticamente il numero di accessi ai registri hardware e rendendo l'editing fluido anche a 80 colonne.
4.  **Auto-decremento:** Introdotta la costante `VERA_ADDR_H_BASE_N1` in `vera_common.inc` per configurare l'auto-decremento di VERA (step -1), permettendo shift verso destra (INSERT) più efficienti senza ricalcoli manuali di indirizzo per ogni cella.

---

## Ottimizzazione Footprint e RAM di sistema

Per massimizzare la memoria RAM disponibile per le applicazioni dell'utente, è stata intrapresa una fase di revisione del codice mirata a ridurre l'impronta (`footprint`) del driver in memoria.

### Modifiche implementate:
*   **Rimozione simboli obsoleti**: È stata eliminata la definizione e l'esportazione dei simboli `_vera_orig_editor_put` e `_vera_orig_screen_put` all'interno dell'handler `E:` (`vera_sys_es_hook.s`). Queste variabili venivano inizialmente previste come backup per un eventuale concatenamento degli handler (chaining), ma non venivano mai lette dal sistema.
*   **Risultati**: La rimozione ha liberato **4 byte** nello spazio `LOWBSS` ed eliminato istruzioni di caricamento (`sta`) inutilizzate all'interno della routine di installazione degli hook, riducendo le dimensioni complessive del binario `VERA.SYS` e migliorando la manutenibilità del codice.

---

### Ottimizzazioni micro-architetturali (Loop Unrolling)

Per massimizzare la fluidità delle operazioni video intensive (scroll e editing), è stato applicato il *loop unrolling* alle routine più critiche.

*   **Routine ottimizzate**: `scroll_up` (in `vera_driver.s`) e `bs_shift_and_blank` (in `vera_sys_es_hook.s`).
*   **Dettagli**: Il loop di copia riga per riga è stato srotolato per processare 8 blocchi di dati (carattere + attributo) per iterazione, riducendo drasticamente il numero di cicli spesi per il controllo del contatore (`dey`) e il salto condizionato (`bne`).
*   **Risultati**: La velocità di esecuzione delle operazioni di scorrimento video e di shift orizzontale durante l'editing è aumentata significativamente. 
*   **Bilancio**: Queste modifiche hanno comportato un leggero aumento delle dimensioni del binario finale (`VERA.SYS`), un compromesso deliberato per migliorare la reattività dell'interfaccia utente a 80 colonne.

---

---

### Correzione range error 6502 nel dispatcher tasti (`vera_sys_es_hook.s`)

**Sintomo (errore di build):** Dopo l'aggiunta di `check_cursor_warning` e del trampoline `@store_char_jmp`, ca65 emetteva:

```
vera_sys_es_hook.s:546: Error: Range error (132 not in [-128..127])
```

**Causa radice:** Il blocco di dispatch all'inizio del `@key_loop` usava `beq @got_return`, `beq @got_backspace`, ecc. per saltare ai rispettivi handler. Le nuove istruzioni inserite tra il punto di branch e i target avevano allungato la distanza oltre i 127 byte consentiti dal 6502.

**Correzione:** Sostituite le quattro `beq` dirette con branch verso trampolines locali immediatamente adiacenti:

```asm
    cmp #ATASCII_EOL
    beq @jmp_got_return
    ...
    jmp @dispatch_done
@jmp_got_return:        jmp @got_return
@jmp_got_backspace:     jmp @got_backspace
@jmp_got_delete_char:   jmp @got_delete_char
@jmp_got_insert_char:   jmp @got_insert_char
@dispatch_done:
```

Il `jmp @dispatch_done` garantisce che il flusso normale salti i trampolines senza eseguirli.

---

### Avviso sonoro di prossimità a fine riga logica (`vera_sys_es_hook.s`)

**Funzionalità:** Viene emesso un click BELL quando il cursore attraversa la soglia della colonna 75 sulla seconda riga fisica di una riga logica (posizione logica = 80 + 75 = 155), avvisando l'utente che sta per raggiungere il limite massimo di input (160 caratteri).

**Implementazione:**

- Nuova variabile BSS `warning_beep_state` ($FF = soglia raggiunta, $00 = sotto soglia).
- Nuova routine `check_cursor_warning`: chiamata dopo ogni operazione che sposta il cursore (echo di caratteri, backspace, delete, insert). Calcola la posizione logica del cursore (`(Y - input_start_row) * 80 + X`), la confronta con `INPUT_LINE_MAX - 5` (155). Emette il click via `_vera_trigger_click` solo al **cambio di stato** (crossing bidirezionale), così non suona in modo continuo.
- Se `input_on_row2 = $00` (riga singola), la routine azzera `warning_beep_state` senza fare nulla.

---

### Correzione posizionamento cursore dopo RETURN su riga logica a 2 righe (`vera_sys_es_hook.s`)

**Sintomo:** Premendo RETURN mentre il cursore era sulla prima riga fisica (`input_start_row`) di una riga logica che si estendeva su 2 righe fisiche, il cursore atterrava su `input_start_row + 1` — cioè sulla seconda riga fisica della stessa riga logica precedente. Il successivo inserimento di testo sovrascriveva il contenuto già presente.

**Causa radice:** `echo_to_vera(ATASCII_EOL)` chiama `cr_lf`, che fa semplicemente `CURSOR_Y++` dalla posizione corrente. Con `CURSOR_Y = input_start_row`, il risultato era `input_start_row + 1`, che è ancora dentro la riga logica.

**Correzione:** Prima di chiamare `echo_to_vera(EOL)`, se `input_on_row2 = $FF`, il cursore viene esplicitamente posizionato su `input_start_row + 1`. Così `cr_lf` lo porta a `input_start_row + 2`, prima riga libera dopo la riga logica. Lo scroll (se necessario) è gestito da `cr_lf` come di consueto.

```asm
    lda input_on_row2
    beq @ret_eol
    lda input_start_row
    clc
    adc #1
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    sta ROWCRS
@ret_eol:
    lda #ATASCII_EOL
    jsr echo_to_vera
```

---

### Navigazione su righe precedenti: RETURN legge la riga logica completa (`vera_sys_es_hook.s`)

**Sintomo:** Spostando il cursore su una riga logica precedente con i tasti freccia e premendo RETURN, il driver leggeva la riga sbagliata (quella dove era partita la sessione GET originale). Per le righe a 2 righe fisiche, veniva letta solo la prima riga.

**Causa radice:** `input_start_row` e `input_on_row2` venivano impostati una volta sola all'inizio di ogni sessione GET (`@need_input`) e non venivano mai aggiornati quando il cursore si spostava su un'altra riga con i tasti cursore.

**Architettura della soluzione:**

Introdotte le seguenti variabili BSS:

| Variabile | Descrizione |
|---|---|
| `session_start_row` | Riga di inizio della sessione GET corrente (mai cambia) |
| `session_on_row2` | Replica `input_on_row2` per la riga di sessione (aggiornato su `@now_on_row2`) |
| `input_start_col` | Colonna X di inizio input (per saltare il prompt `?`) |
| `session_start_col` | Replica `input_start_col` per la riga di sessione |
| `row2_map[SCREEN_ROWS_VIEW]` | Per ogni riga fisica: `$FF` se è la seconda riga di una riga logica |
| `start_col_map[SCREEN_ROWS_VIEW]` | Per ogni riga fisica: colonna X di inizio input al momento del RETURN |

**Aggiornamento `row2_map` e `start_col_map`:** Ad ogni RETURN, dopo lo strip del contenuto, viene aggiornato `row2_map[input_start_row + 1]` ($FF se il contenuto eccede la capacità di riga 1, $00 altrimenti) e `start_col_map[input_start_row]` con la colonna di inizio della sessione corrente.

**Shift su scroll:** `_vera_scroll_hook` è stato esteso per shiftare di 1 posizione verso l'alto sia `row2_map` che `start_col_map` (parallelo allo shift del contenuto video), e per decrementare anche `session_start_row`.

**`rederive_from_cursor`:** Nuova routine che re-imposta incondizionatamente `input_start_row`, `input_on_row2`, `input_start_col`, `input_full` e `warning_beep_state` dalla posizione corrente di `CURSOR_Y` e dalla `row2_map`:
- Se `row2_map[CURSOR_Y] = $FF`: cursore sulla seconda riga → `input_start_row = CURSOR_Y - 1`, `input_on_row2 = $FF`
- Altrimenti: `input_start_row = CURSOR_Y`; se `row2_map[CURSOR_Y + 1] = $FF` → `input_on_row2 = $FF`, altrimenti `$00`
- In entrambi i casi carica `input_start_col = start_col_map[input_start_row]`

Questa routine è chiamata all'inizio di `@got_return`, garantendo che la lettura VRAM usi sempre le coordinate corrette indipendentemente da dove il cursore si trova.

**`rederive_if_navigated`:** Chiamata dopo ogni echo di tasto cursore. Se `CURSOR_Y < session_start_row` (cursore salito su una riga precedente), chiama `rederive_from_cursor` così le operazioni di editing (backspace, insert, delete) lavorano sulla riga logica corretta. Se `CURSOR_Y >= session_start_row` (cursore tornato sulla riga di sessione), ripristina `input_start_row`, `input_on_row2` e `input_start_col` dai valori di sessione salvati.

---

### Correzione inclusione del prompt `?` nel buffer INPUT (`vera_sys_es_hook.s`)

**Sintomo:** Eseguendo `INPUT A$` in Atari BASIC, la stringa restituita conteneva il carattere `?` come primo carattere (es. `?CIAO MONDO` invece di `CIAO MONDO`).

**Causa radice:** Atari BASIC invia `?` via CIO PUT BYTE prima di chiamare GET. Il cursore avanza a colonna 1. `@got_return` leggeva la VRAM dall'indirizzo colonna 0 (primo byte della riga) e restituiva il buffer a partire da `input_rd = 0`, includendo il `?`.

**Correzione:** `input_start_col` (registrato a `@need_input` come `CURSOR_X` al momento dell'inizio GET) è usato come valore iniziale di `input_rd` invece di 0:

```asm
    lda input_start_col
    sta input_rd            ; salta i caratteri del prompt all'inizio del buffer
```

Per garantire che l'EOL sia sempre accessibile anche nel caso di input vuoto (l'utente preme RETURN senza digitare nulla), X viene clamped a `input_start_col` prima di scrivere il terminatore:

```asm
@write_eol:
    cpx input_start_col
    bcs @eol_col_ok
    ldx input_start_col     ; clamp: EOL non può essere prima del punto di inizio lettura
@eol_col_ok:
    lda #ATASCII_EOL
    sta input_buf, x
```

Questo fix funziona correttamente per qualsiasi prompt (single char `?`, doppio `? `, nessun prompt con `input_start_col = 0`), ed è trasparente per la navigazione su righe precedenti grazie a `start_col_map` che memorizza la colonna originale di ogni riga già sottomessa.

---

## Strategia di integrazione
Il driver rende effettivamente la scheda VERA il dispositivo di visualizzazione *primario*. Le routine OS PUT BYTE originali *non* vengono chiamate; il driver custom reindirizza invece tutto l'output di testo direttamente nella VRAM di VERA. Impostando i margini di sistema (`LMARGIN`, `RMARGIN`) a 0/79 durante l'OPEN, il driver garantisce che il software OS Atari veda un dispositivo standard a 80 colonne.

