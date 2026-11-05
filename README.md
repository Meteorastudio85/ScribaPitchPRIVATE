# Scriba Pitch — Meteora Plugins

Editor di intonazione della voce in stile Melodyne, con riconoscimento della
tonalità e **AUTOMATIC CORRECTION** che mette a posto in un clic tutte le note
del brano. Stessi colori e stile di Scriba.

Formati: **VST3** e **AU** (macOS), **VST3** (Windows), più la versione **Standalone**. Supporta **ARA2**.
Versione 0.4.0 (JUCE 8, ARA2).

---

## Novità 0.5.5

- **Suono della nota mentre la trascini, come Melodyne**: un tono pulito e morbido
  all'altezza della nota (non più l'audio di quel pezzo di voce). Suona finché tieni
  premuto il mouse, segue la nota semitono per semitono (al cent con Alt) con un glide di
  25 ms, e si spegne appena lasci. Frecce su / giù e doppio clic: un tono breve.

## Novità 0.5.4

- **SNAP di default = CHROMATIC** (come Melodyne): trascinando, la nota va su qualunque
  semitono, anche fuori tonalità; il colore dice se è in tonalità. SCALE resta disponibile.
- **Alt / Option mentre trascini = nessuno snap** (al cent).
- Trascinando su / giù con lo strumento principale la nota non si sposta più nel tempo per
  piccoli movimenti laterali della mano (serve un trascinamento laterale chiaro, > 8 px).
- Test con una voce vera (`SP_VOICE=voce.wav ScribaPitchPluginTest`): ogni nota rossa
  trascinata di una riga con il mouse.

## Novità 0.5.3

- **Tonalità da Scriba, davvero**: con Scriba 0.6.2 (o successivo) su chitarra / basso / piano,
  Scriba Pitch legge la sua tonalità e i suoi accordi. Provato con il vero Scriba.vst3
  (test `ScribaPitchLinkTest`): stesso processo, processo separato, dopo 12 s, e rimozione.
- **Righello del tempo sincronizzato anche in REAPER senza ARA** (VST3): clic sul righello =
  il cursore di REAPER va lì (anche mentre suona); doppio clic / barra spaziatrice = play/stop.
  Con ARA (Logic, Cubase, Studio One, REAPER) funzionava già.

## Novità 0.5.2

- Note **azzurro chiaro** = in tonalità e intonate, **arancioni** = fuori di qualche cent,
  **rosse** = fuori tonalità. Finché la tonalità non è nota le note restano grigie.
- Riepilogo in basso nel roll dopo la lettura: tonalità, da dove arriva (Scriba / voce /
  manuale) e quante note rosse, arancioni e azzurre.
- Riga info: dice chiaramente "Scriba not found" quando nessuna istanza di Scriba pubblica
  la tonalità (Scriba deve avere Meteora Link, vedi SCRIBA_INTEGRATION.md).

## Novità 0.5.1

- **Consonanti e click di bocca protetti**: sugli attacchi (t, p, k, schiocchi di lingua) il
  motore lascia passare l'originale per pochi millisecondi invece di ripeterli o spostarli —
  era una delle fonti di "gracchiato" sulle voci vere.
- Nuovo tool `ScribaPitchFileTest <voce.wav> <cartella> [scala]`: passa una voce vera nel
  motore (analisi, tonalità, AUTOMATIC CORRECTION, spostamenti) e salva i WAV corretti.

## Novità 0.5.0

- **Correzione fluida, niente più gracchiato.** Il motore ora segue l'intonazione con un
  tracker probabilistico (stile pYIN + Viterbi) che non salta d'ottava sul vocal fry e non
  perde pezzi di nota sui respiri; i punti di taglio dei periodi (marks) sono agganciati in
  fase alla curva e non saltano più quando cambia la vocale o c'è riverbero. Sui test
  (voci pulite, voci in stanza, voci ruvide e soffiate): oscillazione extra di pitch
  ≤ 1,5 cent, perdita di armonicità ≤ 1,6 dB, 0 click.
- **GUI nello stile di Scriba Align**: blu notte, card arrotondate con bordo sottile,
  accento teal, ruota della tonalità con la nota grande al centro, barra di confidenza,
  riga info sulla provenienza della tonalità, pillola di stato in alto a destra.

## Come compilarlo (GitHub Actions, come Hush)

La build scarica da sola **JUCE** e l'**SDK ARA2** di Celemony (gratuito, open source).
Per compilare in locale con ARA: nella cartella del progetto
`git clone --recursive --branch releases/2.2.0 https://github.com/Celemony/ARA_SDK`
(senza la cartella `ARA_SDK` il plugin si compila lo stesso, ma senza ARA).


1. Crea un repository su GitHub e carica dentro tutto il contenuto di questa cartella
   (compresa `.github/workflows/build.yml`).
2. Ogni push avvia la build per **Windows** e **macOS** (Apple Silicon + Intel).
   JUCE viene scaricato in automatico. Puoi anche avviarla a mano da **Actions → Build Scriba Pitch → Run workflow**.
3. A build finita, in fondo alla pagina della run trovi due download:
   `Scriba-Pitch-Windows` e `Scriba-Pitch-macOS`.

Durante la build gira anche un test del motore: se l'intonazione corretta non
torna entro pochi cents, la build fallisce.

Per compilare in locale: `cmake -B build && cmake --build build --config Release`.

## Installazione

- **Windows**: copia `Scriba Pitch.vst3` in `C:\Program Files\Common Files\VST3`.
- **macOS**: copia `Scriba Pitch.vst3` in `/Library/Audio/Plug-Ins/VST3` e
  `Scriba Pitch.component` in `/Library/Audio/Plug-Ins/Components`.
  I file sono firmati ad-hoc: se macOS li blocca, da Terminale
  `xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/Components/Scriba Pitch.component"` (stesso per il .vst3).
- Riavvia la DAW e cerca **Scriba Pitch** sotto **Meteora Plugins**.

---

## Come si usa

### Nelle DAW con ARA2 (Logic Pro, Cubase / Nuendo, Studio One, Reaper e le altre con ARA)

Metti Scriba Pitch sulla traccia della voce: la DAW gli passa direttamente il file
audio, e **in meno di un secondo** (per una voce di qualche minuto, pochi secondi)
compaiono note, tonalità e note fuori tonalità, **senza premere play**. Il chip in
alto mostra **ARA**.

- Logic Pro: basta l'insert (versione AU).
- Reaper: usa la versione **VST3** come **FX di traccia** (resta nel primo slot).
  Se dopo un aggiornamento in alto vedi **NO ARA**: Preferenze → Plug-ins → VST →
  **Clear cache/re-scan**, poi rimetti il plugin (Reaper ricorda la scansione vecchia).
  ARA dev'essere attivo nelle preferenze VST di Reaper (lo è di default).
- Studio One: trascina Scriba Pitch sull'evento audio, oppure insert + "Modifica con".
- Cubase / Nuendo: seleziona l'evento audio, menu **Audio → Estensioni → Scriba Pitch**
  (un normale insert in Cubase non usa ARA).

In alto, accanto alla versione, il plugin mostra **ARA** (la DAW gli passa la traccia)
oppure **NO ARA** (modalità cattura: premi play).

Con ARA il plugin **suona lui la traccia**: se sposti il clip nella DAW le correzioni
lo seguono subito; se cambi il file, Scriba Pitch lo rilegge da solo.
**READ AGAIN** rilegge la traccia tenendo le correzioni.

**Cursore della DAW dal plugin (solo ARA):**
- clic sul righello in alto (numeri di battuta) = il cursore della DAW va lì;
- doppio clic sul righello o **barra spaziatrice** = play / stop.

### Nelle DAW senza ARA (es. Ableton Live)

Metti Scriba Pitch in insert: dopo un paio di secondi si **arma da solo**
(*PRESS PLAY IN YOUR DAW*). Premi play e lascia suonare la voce; quando fermi la DAW
le note compaiono in pochi secondi. Qui il plugin non può muovere il cursore della DAW
(i VST3/AU normali non hanno questo comando).

Metti sempre Scriba Pitch sulla **voce asciutta**, il più in alto possibile.

Colori delle note:
- **ciano**: intonata (entro 10 cents dalla nota);
- **arancione**: nota giusta ma calante o crescente di qualche cent (più di 10);
- **rosso**: nota sbagliata, fuori dalla tonalità, con la nota di arrivo tratteggiata.

Poi: **AUTOMATIC CORRECTION** corregge tutto il brano (un secondo clic annulla),
oppure lavori nota per nota nel roll.

### Tastiera

| Tasto | Funzione |
|---|---|
| **1** | Main (freccia) |
| **2** | Pitch |
| **3** | Pencil (matita) |
| **4** | Pitch Modulation |
| **5** | Pitch Drift |
| **6** | Formant |
| **7** | Amplitude |
| **8** | Timing |
| **9** | Note Separation |
| **B** (tieni premuto) | Senti la voce originale; rilasci = torna la corretta |
| Spazio | Play / stop della DAW (ARA) |
| Cmd/Ctrl+Z, Cmd/Ctrl+Shift+Z | Annulla / ripeti |

### Il roll

| Azione | Cosa fa |
|---|---|
| Trascina una nota su/giù | Cambia nota; la senti mentre la muovi |
| Trascina a destra/sinistra | Sposta la nota nel tempo |
| Trascina il **bordo sinistro** | Sposta l'attacco (parola in ritardo → la rimetti a tempo, la nota si allunga) |
| Trascina il **bordo destro** | Allunga o accorcia la coda |
| Doppio clic | Centra la nota sul semitono |
| Frecce ↑ ↓ | Nota sopra/sotto |
| Frecce ← → (Shift = stretch) | Sposta di 10 ms / allunga |
| Rotella / Shift+rotella / Cmd(Ctrl)+rotella | Scorri note / scorri tempo / zoom |
| Trascina lo sfondo | Muovi la vista |
| Cmd(Ctrl)+Z, Cmd+Shift+Z | Annulla / ripeti |

**SNAP**: *Scale* = la nota finisce solo su note della tonalità, *Chromatic* = ogni
semitono, *Off* = libera, anche di pochi cents.

**Strumenti** (barra in alto): Main (sposta + stretch), Pitch (solo altezza),
**Pencil** (disegni a mano la curva dell'intonazione sopra la nota, come la matita di
Melodyne: il tratto disegnato è bianco; Alt+trascina = cancella e torna alla curva
cantata; doppio clic = cancella tutto il disegno della nota; al rilascio senti il
risultato), Pitch Modulation (vibrato: trascina su/giù), Pitch Drift (raddrizza la curva),
Formant, Amplitude, Timing (solo tempo + stretch), Note Separation (clic = taglia la nota).

**Inspector** (in basso): i parametri della nota selezionata — Pitch Center,
Pitch Drift, Vibrato, Formant, Gain, Timing. Doppio clic su una manopola = valore iniziale.

**DRAG MIDI / DRAG AUDIO**: trascina sulla timeline la melodia corretta in MIDI o
la voce corretta in WAV (rilascia all'inizio della registrazione). Clic = salva il file.

### Pannello a sinistra

- **AUTO KEY / MANUAL KEY**: in AUTO KEY la tonalità arriva da Scriba (se è nel
  progetto, chip **LINKED · SCRIBA**, con i suoi accordi sopra le battute) oppure dalla
  voce. In MANUAL KEY la scegli tu dal menu (parte dalla tonalità in uso).
  **SCALE**: scala della tonalità, minore armonica, minore melodica, dorica,
  misolidia, cromatica.
- **BYPASS**: spegne tutto il plugin (passa la voce originale). È lo stesso bypass
  della DAW: se lo premi nella DAW si accende anche qui.
- **CORRECTION AMOUNT**: quanto centrare ogni nota (100% = esatta).
- **PITCH DRIFT**: quanto raddrizzare le calanti/crescenti dentro la nota.
- **HUMANIZE**: lascia qualche cent di imperfezione naturale.
- **KEEP VIBRATO & SLIDES**: tiene il vibrato e i portamenti.

**A/B in alto**: il pulsante mostra CORRECTED; **tienilo premuto** (o tieni premuto **B**)
per sentire la voce originale, rilascia per tornare a quella corretta. Il plugin resta
acceso (note, colori e anteprime funzionano): per spegnerlo del tutto c'è BYPASS.

---

## CPU quasi a zero: come funziona

- L'analisi gira una volta, in un thread separato.
- La voce corretta viene **renderizzata in background** (pitch shift sincrono al
  periodo, formanti preservate) e salvata in memoria. In riproduzione il plugin
  legge solo campioni già pronti: costa come una traccia audio.
- Quando modifichi una nota viene ricalcolata **solo quella zona** (pochi millisecondi).
- Le parti che non tocchi escono **identiche bit per bit** all'originale.
- Latenza aggiunta: zero.

Memoria: circa 35 MB di RAM per ogni minuto di voce a 48 kHz (la presa originale
più due copie della voce corretta, per poter aggiornare il render senza mai
fermare l'audio). Durante la cattura il plugin riserva spazio per 8 minuti
(circa 90 MB), poi lo riduce alla durata reale.

## Dove finiscono i file

La presa registrata viene salvata in FLAC, così il progetto si riapre con tutte le correzioni:
- macOS: `~/Library/Application Support/Meteora Plugins/Scriba Pitch/Takes`
- Windows: `%APPDATA%\Meteora Plugins\Scriba Pitch\Takes`

Se apri il progetto su un altro computer senza questa cartella, il plugin mostra
*TAKE MISSING*: premi di nuovo ANALYZE.

## Limiti di questa versione (0.1)

- Senza ARA serve far suonare il brano una volta per catturare la voce
  (con ARA no). Se poi cambi la traccia audio nella DAW, rifai ANALYZE.
- Con ARA il time-stretch fatto dalla DAW sul clip non viene seguito (si legge il
  file alla sua velocità originale).
- L'uscita corretta è mono (la voce sommata), copiata su entrambi i canali.
- Pensato per una voce solista: cori e voci già pesantemente autotunate danno note meno precise.
- Il collegamento con Scriba (Meteora Link) funziona quando anche Scriba scrive il
  file di collegamento: vedi `SCRIBA_INTEGRATION.md` (circa 20 righe da aggiungere a Scriba).

## Test

`Tests/EngineTest.cpp` sintetizza una melodia stonata in La minore, la analizza,
la corregge e misura il risultato:

- rilevamento delle 13 note entro ±2 cents, tonalità La minore;
- dopo la correzione tutte le note entro ±1 cent dalla nota giusta
  (F#4 → G4 e C#4 → C4 spostate sulla scala);
- nota spostata di 2 semitoni e nota stirata nel tempo misurate corrette;
- audio non toccato identico all'originale;
- render dell'intero brano circa 700 volte più veloce del tempo reale.

`Tests/QualityTest.cpp` misura la qualità del suono su voci realistiche (maschile
grave, femminile, con riverbero, consonanti, voce rotta, frasi legate): pulizia della
voce (HNR) e click dopo spostamenti di ±3 semitoni, correzione automatica, stretch e
12 trascinamenti successivi di una nota.

`Tests/ARAHostTest.cpp` (solo Linux) è una piccola DAW ARA: carica il VST3, gli dà la
voce come file su una traccia e verifica che le note compaiano senza premere play
(0,65 s per 9 s di voce), la riproduzione via ARA, la correzione, lo spostamento del
clip e la riapertura del progetto.

`Tests/PluginTest.cpp` (solo Linux) prova il plugin intero senza DAW: cattura,
analisi, AUTOMATIC CORRECTION, drag e stretch col mouse, undo, salvataggio e
riapertura del progetto.

---

© 2026 Meteora Plugins. Font Nunito (SIL Open Font License, `Resources/OFL-Nunito.txt`). Basato su JUCE.
