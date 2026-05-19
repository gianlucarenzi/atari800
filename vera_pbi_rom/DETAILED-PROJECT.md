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

## Strategia di integrazione
Il driver rende effettivamente la scheda VERA il dispositivo di visualizzazione *primario*. Le routine OS PUT BYTE originali *non* vengono chiamate; il driver custom reindirizza invece tutto l'output di testo direttamente nella VRAM di VERA. Impostando i margini di sistema (`LMARGIN`, `RMARGIN`) a 0/79 durante l'OPEN, il driver garantisce che il software OS Atari veda un dispositivo standard a 80 colonne.
