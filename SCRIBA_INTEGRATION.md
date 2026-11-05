# Collegare Scriba a Scriba Pitch (Meteora Link)

> **Fatto in Scriba 0.6.2**: `publishMeteoraLink()` in `PluginProcessor.cpp` di Scriba.
> Questo documento resta come riferimento del formato.

Scriba e Scriba Pitch sono due plugin separati (due file .vst3/.component
diversi), quindi non possono condividere variabili in memoria come fanno tra
loro le istanze di Scriba. Si parlano con **Meteora Link**: ogni istanza scrive
un file JSON minuscolo e legge quelli delle altre istanze **nella stessa DAW**
(stesso processo). È tutto in un solo file header, `Source/Link/MeteoraLink.h`,
che va copiato identico anche in Scriba.

- Cartella: macOS `~/Library/Application Support/Meteora Plugins/Link`,
  Windows `%APPDATA%\Meteora Plugins\Link`
- Un file per istanza: `<pid della DAW>_<id istanza>.json`, cancellato quando
  l'istanza si chiude. I file rimasti da un crash vengono ignorati (pid diverso)
  e rimossi dopo 2 giorni.
- Costo: scrittura solo quando cambia il risultato, lettura una volta al secondo
  sul message thread. Mai nel thread audio.

Il formato del file:

```json
{
  "version": 1,
  "plugin": "Scriba",
  "instrument": "GUITAR",
  "key": 21, "alt": 0, "confidence": 0.94,
  "updated": 1793305277000,
  "chords": [ { "ppq": 0.0, "len": 4.0, "name": "Am" }, { "ppq": 4.0, "len": 4.0, "name": "F" } ]
}
```

`key`: 0–11 = Do…Si maggiore, 12–23 = Do…Si minore (La minore = 21), -1 = nessuna.
`ppq` = posizione in quarti dall'inizio del progetto (battuta 1 = 0).

## Cosa aggiungere a Scriba (circa 20 righe)

1. Copia `MeteoraLink.h` in Scriba (es. `Source/Link/MeteoraLink.h`).

2. Nel processor di Scriba:

```cpp
#include "Link/MeteoraLink.h"

// membro della classe
meteora::link::Publisher linkPublisher;
```

3. Quando un'analisi finisce (o cambi strumento/sensibilità), sul message thread:

```cpp
void ScribaProcessor::publishToMeteoraLink()
{
    meteora::link::Info info;
    info.plugin     = "Scriba";                 // esattamente così: Scriba Pitch filtra su questo nome
    info.instrument = currentInstrumentName();  // "BASS", "GUITAR", "PIANO", "KICK", "SNARE", "VOCALS"
    info.key        = detectedKeyIndex;         // 0..23 come sopra, -1 se non c'è
    info.alt        = alternativeKeyIndex;
    info.confidence = keyConfidence;            // 0..1

    // accordi battuta per battuta (solo GUITAR / PIANO / BASS)
    const double quartersPerBar = beatsPerBar * 4.0 / beatUnit;   // 4/4 -> 4
    for (const auto& c : chordsByBar)          // c.bar = battuta della DAW (1 = prima)
        info.chords.push_back ({ (c.bar - 1) * quartersPerBar, c.bars * quartersPerBar, c.name });

    linkPublisher.publish (info);
}
```

Il file si cancella da solo quando l'istanza si chiude (distruttore di `Publisher`).

4. (Facoltativo) Perché lo snare e i VOCALS di Scriba prendano la tonalità
   anche da Scriba Pitch, dove oggi leggete la tonalità condivisa:

```cpp
const auto others = meteora::link::readOthers (linkPublisher.getInstanceId());
const auto song   = meteora::link::combine (others);   // tutti i plugin Meteora
if (song.key >= 0) { /* usa song.key, song.confidence, song.chords */ }
```

Il collegamento interno tra le istanze di Scriba può restare com'è: Meteora
Link si aggiunge, non lo sostituisce.

## Cosa fa Scriba Pitch con questi dati

- **SONG KEY = Auto**: se c'è almeno uno Scriba con una tonalità, usa quella
  (basso, chitarra e piano pesano di più della voce) e il chip in alto diventa
  **LINKED · SCRIBA**. Altrimenti usa la tonalità trovata nella voce.
  Una SONG KEY scelta a mano vince sempre.
- Gli **accordi** di Scriba compaiono in una corsia sopra le battute del roll;
  l'accordo di tonica è in ciano.
- Scriba Pitch pubblica a sua volta la tonalità della voce
  (`plugin: "Scriba Pitch"`, `instrument: "VOCALS"`).
- Valgono le stesse regole di Scriba: in Bitwig la modalità normale dei plugin
  (non isolata), perché le istanze devono girare nello stesso processo della DAW.
