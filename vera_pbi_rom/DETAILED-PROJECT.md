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
    *   **Coprocessore FX:** Emulazione parziale del coprocessore VERA FX per operazioni come tracciamento di linee, riempimento di poligoni e trasformazioni affini.
    *   **Audio/SPI:** Emulazione dei canali audio PSG/PCM di VERA e dell'interfaccia SPI per l'emulazione della scheda SD.
*   **Integrazione bus:**
    *   **Gestione IRQ:** Gestisce le richieste di interrupt da VERA alla CPU Atari in base alle impostazioni IEN/ISR.
    *   **Configurazione:** Supporta argomenti CLI per abilitare la scheda (`-verax16`), specificare l'immagine ROM handler (`-verax16-rom`) e collegare un'immagine scheda SD per l'interfaccia SPI (`-verax16-sdcard`).
*   **Gestione del ciclo di vita:** Gestisce gli stati di accensione/reset, garantendo che la VRAM sia inizializzata e la scheda sia correttamente abilitata/disabilitata sul bus.

---
[... rest of existing documentation ...]
