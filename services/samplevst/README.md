# samplevst

Crea uno strumento suonabile via MIDI dai campioni audio dell'utente, in due formati:

| Formato | Dove si usa |
|---|---|
| **SoundFont** (`.sf2`) | MuseScore (Windows, macOS, Linux) e VibeScore (Android): Mixer → suono dello strumento |
| **VST3** (`.vst3`) | MuseScore e qualsiasi host VST3 (DAW) su Windows, macOS, Linux. Android non carica plugin VST |

## Che campioni vanno bene

| Cosa si carica | Risultato |
|---|---|
| Una sola nota (es. un La di violino) | Trasposta su tutta la tastiera |
| Più note, in più file o nello stesso file (una dopo l'altra, legate o separate da silenzio) | Ogni campione copre i tasti più vicini alla sua nota |
| La stessa nota registrata piano e forte | Livelli di dinamica (velocity) |
| Batteria, effetti, suoni senza altezza | Kit: un suono per tasto, non trasposto. `kick`, `snare`, `hihat`, `crash`, `ride`, `tom`, `clap`, `cassa`, `rullante`… vanno sul loro tasto General MIDI (C2 cassa, D2 rullante, F#2 charleston chiuso…), così una parte di batteria scritta in MuseScore suona i suoni giusti; gli altri da C4 in su |
| Un solo suono senza altezza (un «boing», un colpo) | Suona alla sua altezza su C4 e viene trasposto sugli altri tasti |

- Formati: WAV, AIFF, FLAC, MP3, M4A, OGG, anche l'audio di un video. Fino a 120 s per file, 128 suoni per strumento.
- Una **nota nel nome del file** vince su quella rilevata: `violino_A4.wav`, `Flauto C#5 ff.wav`, `tromba-sol3.mp3`
  (C4 = Do centrale = 60, come in MuseScore; nomi italiani do re mi fa sol la si).
- Il silenzio prima e dopo viene tagliato; i livelli dei campioni vengono pareggiati (picchi a −1 dB).
- I suoni **tenuti** (archi, organo, voce, fiati) ricevono un **loop**: la nota dura finché il tasto è premuto.
  I suoni che si spengono (pianoforte, pizzicato, percussioni) suonano fino in fondo o fino al rilascio del tasto.
- Il charleston aperto viene zittito da quello chiuso (gruppo), come su una batteria vera.

## Come si usa: dalle app, via Drive

- **Desktop**: *File > Make instrument from samples…*
- **Tab Drive** (desktop e telefono/tablet): menu ⋯ → *Make instrument from samples…*, si scrive il nome (vuoto =
  ricavato dai nomi dei file) e si scelgono i campioni.

I campioni finiscono in una cartella con il nome dello strumento dentro la cartella Drive **`Nuovo strumento`**, con un
`strumento.json` scritto per ultimo. Il workflow `Samplevst: Drive` di GitHub Actions la controlla ogni 10 minuti
(GitHub spesso ritarda di qualche minuto; *Run workflow* parte subito) e mette nella cartella Drive **`Strumenti`**:

- `<Nome>.sf2` e `<Nome>.vst3.zip`, con un riepilogo nella descrizione del file (note dei campioni, estensione, loop, avvisi)
- le richieste fatte vanno in `Nuovo strumento/Fatti`, quelle fallite in `Nuovo strumento/Errori` col motivo nella descrizione

Le app, dopo ogni sincronizzazione (al massimo ogni 5 minuti), installano quello che in `Strumenti` è nuovo o cambiato:

- il **SoundFont** nella cartella SoundFonts dell'utente (Impostazioni > Cartelle in MuseScore), su ogni dispositivo
- il **VST3** sui computer, nella cartella VST3 dell'utente, quella che MuseScore e gli altri host cercano senza
  impostazioni: `%LOCALAPPDATA%\Programs\Common\VST3` (Windows), `~/Library/Audio/Plug-Ins/VST3` (macOS), `~/.vst3`
  (Linux). Lo strumento nuovo viene aggiunto all'elenco dei plugin dell'app.

Ricreare uno strumento con lo **stesso nome** lo sostituisce (stesso ID del plugin): le partiture che lo usano
continuano a funzionare, con i campioni nuovi.

Anche senza app: una cartella creata a mano in `Nuovo strumento` dal sito di Drive viene presa quando non cambia da
10 minuti (il nome della cartella è il nome dello strumento); un file audio messo direttamente in `Nuovo strumento`
diventa uno strumento a sé, col nome del file.

`strumento.json` (tutte le chiavi facoltative):

```json
{"nome": "Violino", "tipo": "auto|melodico|batteria", "nota": "A4", "loop": "auto|si|no",
 "attacco": 0.01, "rilascio": 0.3, "dividi": true}
```

`nota` fissa la nota di un campione singolo, `dividi: false` non divide un file in più suoni.

## Il plugin VST3

Un solo campionatore generico (`plugin/`, C++, [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) con licenza MIT)
che legge nome, ID e campioni dalla cartella `Contents/Resources` del proprio bundle. Creare uno strumento = copiare il
bundle precompilato e aggiungerci i campioni: niente compilatore. Il bundle contiene i binari per Windows (x64),
macOS (Apple Silicon e Intel) e Linux (x64).

- 64 voci, interpolazione a 4 punti, loop, livelli di velocity, kit con gruppi
- Pedale (CC64), pitch bend (±2 semitoni, regolabile), volume (CC7), espressione (CC11), all notes off
- Parametri, nella vista generica dell'host: Volume, Attack, Release, Tune, Velocity (sensibilità), Bend range.
  Nessuna interfaccia grafica propria.

Il workflow `Samplevst: plugin and tests` lo compila su Windows, macOS e Linux; su ciascuno crea uno strumento di prova
con quella build e lo passa al validator Steinberg e a un host JUCE (pedalboard). Poi unisce le tre build in
`SampleVST.vst3.zip`, pubblicato come release **`samplevst-plugin`**, da cui lo prende il worker Drive. Finché la
release non esiste il worker crea solo i SoundFont (e lo scrive nella descrizione).

macOS: il binario è firmato ad hoc dal linker, non notarizzato. Va bene per gli strumenti installati dall'app; un
`.vst3` scaricato dal browser viene bloccato da Gatekeeper finché non si toglie la quarantena
(`xattr -dr com.apple.quarantine <Nome>.vst3`).

## Da riga di comando

```
pip install -r requirements.txt          # numpy, scipy, soundfile; ffmpeg per MP3/M4A/video
python -m samplevst.pipeline violino_A4.wav violino_A5.wav -n "Violino" -o out/ --template SampleVST.vst3
```

Il plugin: `cmake -S plugin -B build -Dvst3sdk_SOURCE_DIR=<vst3sdk> -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release`.

Test: `pip install -r requirements-test.txt` e `SAMPLEVST_TEMPLATE=<SampleVST.vst3> SAMPLEVST_VALIDATOR=<validator> pytest tests`
(i test del SoundFont usano FluidSynth, il motore audio di MuseScore, e si saltano se manca).

## Limiti

- L'altezza viene da un rilevatore monofonico: un accordo registrato o un suono molto rumoroso non danno una nota;
  in quel caso conviene scrivere la nota nel nome del file.
- Un campione trasposto di molte ottave suona innaturale: più note si registrano, meglio suona lo strumento.
- Il loop è cercato automaticamente; su suoni con vibrato marcato o che cambiano molto nel tempo una nota tenuta a
  lungo può pulsare leggermente (lo dice la descrizione del file).
