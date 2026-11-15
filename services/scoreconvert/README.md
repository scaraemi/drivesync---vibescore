# scoreconvert

Converte in `.mscz` e `.mid` qualsiasi file musicale: PDF e foto di spartiti, registrazioni audio, MIDI, MusicXML e
gli altri formati di notazione. È il servizio dietro la voce **Importa da PDF / foto / audio** di MuseScore e VibeScore.

| Ingresso | Motore | Qualità attesa |
|---|---|---|
| MIDI, MusicXML/MXL, Guitar Pro, Capella, MEI, BWW, Overture… | MuseScore (import nativo) | Esatta |
| ABC | music21 → MusicXML | Esatta |
| PDF e scansioni stampate | Audiveris (OMR) | Buona su stampa pulita, da rivedere |
| Foto da telefono | Pulizia luce/ombre (OpenCV) + Audiveris | Discreta se la foto è dritta e a fuoco |
| Manoscritti, foto difficili | Claude vision → MusicXML (se `ANTHROPIC_API_KEY`) | **Sperimentale** |
| Audio WAV/MP3/M4A/OGG/video… | Basic Pitch + allineamento ai battiti | **Sperimentale**: note giuste, ritmo da rivedere |

Il tipo si riconosce dal contenuto, non dal nome (un MIDI arrivato come `.bin` da WhatsApp funziona).

## Audio

1. ffmpeg porta tutto a mono 22 kHz. Se il fondo è sopra −45 dB rispetto al picco (stanza, chiesa, esterno) il
   profilo diventa `noisy`: riduzione del rumore, soglie più alte, note minime più lunghe.
2. Basic Pitch trova le note in secondi.
3. librosa trova i battiti; le note vengono spostate sui battiti e allineate a una griglia (sedicesimi di default),
   poi scritte a tempo costante. Senza questo passo l'import MIDI di MuseScore produce legature, punti e gruppi irregolari.
4. Pulizia: armonici (ottava, dodicesima) più deboli di una nota contemporanea, doppi attacchi. Se poche note
   partono insieme la linea è trattata come melodia singola (`mono`): note tagliate dove inizia la successiva,
   piccoli vuoti chiusi.

Misurato sui test (melodia nota, resa audio e riletta): 32/32 note giuste su flauto pulito e su MP3 con rumore
di fondo; pianoforte con basso 48/48 giuste e 3 note spurie. Rumori ambientali senza altezza (traffico, voci
parlate) non diventano note sensate: il servizio lo segnala negli avvisi.

Limiti: la prima nota è presa come primo tempo della battuta, quindi le battute possono risultare spostate su un
brano in levare; un coro o una band registrati insieme finiscono in un solo pentagramma.

## API

```
POST /v1/jobs                 multipart: file, sheet=auto|printed|handwritten,
                              audio_profile=auto|clean|noisy, mono=auto|1|0, tempo=<bpm>   -> 202 {"id"}
GET  /v1/jobs/{id}            {"state": queued|running|done|failed, "stage", "error", "result"}
GET  /v1/jobs/{id}/score      .mscz
GET  /v1/jobs/{id}/midi       .mid
GET  /healthz
```

`result` contiene `engine`, `experimental`, `warnings` (in italiano, da mostrare all'utente) e `details`
(tempo, numero di note, profilo audio, pagine). Autenticazione: `Authorization: Bearer <token>`, token in
`SCORECONVERT_TOKENS` separati da virgola.

## Esecuzione

```
docker run -d --name scoreconvert -p 8080:8080 -v scoreconvert:/data \
  -e SCORECONVERT_TOKENS=<token> -e ANTHROPIC_API_KEY=<facoltativa> \
  ghcr.io/scaraemi/scoreconvert:latest
```

| Variabile | Default | |
|---|---|---|
| `SCORECONVERT_TOKENS` | (vuoto = nessun controllo) | token dei client |
| `SCORECONVERT_WORKERS` | 1 | conversioni in parallelo (OMR e audio usano la CPU) |
| `SCORECONVERT_TTL` | 86400 | secondi dopo cui i risultati vengono cancellati |
| `SCORECONVERT_MAX_MB` | 200 | dimensione massima del file |
| `ANTHROPIC_API_KEY` | | abilita il riconoscimento dei manoscritti |
| `SCORECONVERT_CLAUDE_MODEL` | `claude-sonnet-5-5` | modello per i manoscritti |

Da riga di comando, senza server: `python -m scoreconvert.pipeline file.pdf -o out/`.

La CI (`.github/workflows/scoreconvert.yml`) costruisce l'immagine, esegue i test dentro il container (audio,
OMR su PDF e su foto, import MIDI, giro completo via HTTP) e la pubblica su ghcr.io dal branch principale.

## Licenze

Audiveris è AGPL-3.0 ed è usato come programma separato, invocato da riga di comando; il servizio va comunque
distribuito con il sorgente se offerto a terzi. Basic Pitch: Apache-2.0. MuseScore: GPL-3.0.
