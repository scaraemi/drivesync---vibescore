"""From sounds to an instrument: which key plays which sound, at which velocity, how it ends.

Three shapes, chosen from what the samples are (or forced with the options):

- melodic, one sample: a note (or any sound) transposed over the whole keyboard
- melodic, several samples: each one covers the keys nearest to its pitch; the same pitch recorded at different
  strengths becomes velocity layers
- kit: sounds without a pitch (drums, effects), one per key, never transposed; names like "kick", "snare",
  "hihat" go to their General MIDI key, so a drum part written in MuseScore plays the right sound
"""

from __future__ import annotations

import hashlib
import re
import unicodedata
from dataclasses import dataclass, field

import numpy as np

from .analyze import Sound

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
ITALIAN = {"do": 0, "re": 2, "mi": 4, "fa": 5, "sol": 7, "la": 9, "si": 11}

# General MIDI percussion keys, by words found in file names (English and Italian)
GM_DRUMS = [
    (36, ("kick", "bassdrum", "bass drum", "bd", "cassa", "grancassa")),
    (37, ("sidestick", "side stick", "rim", "rimshot", "cerchio")),
    (38, ("snare", "sd", "rullante")),
    (39, ("clap", "handclap", "battito")),
    (42, ("closed hat", "closedhat", "hihat closed", "hh closed", "chh", "hhc", "hat closed", "charleston chiuso",
          "hihat", "hi-hat", "hi hat", "hh", "charleston")),
    (46, ("open hat", "openhat", "hihat open", "hh open", "ohh", "hho", "hat open", "charleston aperto")),
    (44, ("pedal hat", "hihat pedal", "hh pedal", "phh")),
    (41, ("floor tom", "floortom", "tom 3", "tom3", "timpano")),
    (45, ("low tom", "tom 2", "tom2", "tom basso")),
    (48, ("mid tom", "tom 1", "tom1", "tom medio")),
    (50, ("high tom", "hi tom", "tom alto", "tom")),
    (49, ("crash", "piatto")),
    (51, ("ride",)),
    (53, ("ride bell", "bell", "campana")),
    (55, ("splash",)),
    (52, ("china",)),
    (54, ("tambourine", "tamburello")),
    (56, ("cowbell", "campanaccio")),
    (75, ("clave", "claves", "legnetti")),
    (69, ("cabasa",)),
    (70, ("maracas", "shaker", "maraca")),
]
CLOSED_HAT, OPEN_HAT, PEDAL_HAT = 42, 46, 44


def note_name(midi: int) -> str:
    return f"{NOTE_NAMES[midi % 12]}{midi // 12 - 1}"


def parse_note(text: str) -> int | None:
    """'A4', 'C#3', 'Eb2', 'la4', 'Sol#3', '60' -> MIDI note. C4 = 60, as in MuseScore."""
    t = text.strip()
    if re.fullmatch(r"\d{1,3}", t):
        n = int(t)
        return n if 0 <= n <= 127 else None
    m = re.fullmatch(r"(?i)(do|re|mi|fa|sol|la|si|[a-g])([#b♯♭]?)(-?\d)", t)
    if not m:
        return None
    name, acc, octave = m.group(1).lower(), m.group(2), int(m.group(3))
    pc = ITALIAN[name] if name in ITALIAN else NOTE_NAMES.index(name.upper())
    pc += 1 if acc in ("#", "♯") else -1 if acc in ("b", "♭") else 0
    n = (octave + 1) * 12 + pc
    return n if 0 <= n <= 127 else None


def note_in_name(filename: str) -> int | None:
    """A note written in the file name: 'violino_A4.wav', 'Flute C#5 ff.wav', 'tromba-sol3.mp3'."""
    stem = filename.rsplit(".", 1)[0]
    for tok in reversed(re.split(r"[\s_\-.()\[\]]+", stem)):
        n = parse_note(tok)
        if n is not None and not re.fullmatch(r"\d+", tok):   # a bare number is more often a take than a note
            return n
    return None


def drum_key(filename: str) -> int | None:
    stem = filename.rsplit(".", 1)[0].lower()
    stem = re.sub(r"[_\-.]+", " ", stem)
    words = f" {stem} "
    best = None
    for key, names in GM_DRUMS:
        for n in names:
            if re.search(rf"(?<![a-z]){re.escape(n)}(?![a-z])", words):
                if best is None or len(n) > best[1]:
                    best = (key, len(n))
    return best[0] if best else None


@dataclass
class Zone:
    sound: int            # index in Instrument.sounds
    lokey: int
    hikey: int
    lovel: int = 1
    hivel: int = 127
    root: int = 60        # the key at which the sample plays at its own pitch
    tune: int = 0         # cents, to correct a sample that is a little off its note
    mode: str = "release"  # loop: holds while the key is down | release: stops at note-off | oneshot: plays to the end
    group: int = 0        # a sound of a group stops the others of it (open hi-hat cut by the closed one)


@dataclass
class Options:
    name: str = "Strumento"
    kind: str = "auto"            # auto | melodic | kit
    root: int | None = None       # forces the note of a single sample
    attack: float | None = None   # seconds
    release: float | None = None  # seconds
    loop: str = "auto"            # auto | on | off
    split: bool = True            # cut files holding several sounds


@dataclass
class Instrument:
    name: str
    kind: str                       # melodic | kit
    sounds: list[Sound]
    zones: list[Zone]
    attack: float = 0.002
    decay: float = 0.0
    sustain: float = 1.0
    release: float = 0.25
    notes: list[str] = field(default_factory=list)

    @property
    def uid(self) -> str:
        """32 hex digits. From the name: making the instrument again with the same name replaces it, and scores
        that use it keep working."""
        key = unicodedata.normalize("NFC", self.name).strip().lower()
        return hashlib.md5(f"samplevst/{key}".encode()).hexdigest().upper()

    def summary(self) -> list[str]:
        lines = []
        if self.kind == "kit":
            keys = ", ".join(f"{note_name(z.lokey)} {self.sounds[z.sound].source}" for z in sorted(self.zones, key=lambda z: z.lokey))
            lines.append(f"Kit di {len(self.zones)} suoni: {keys}.")
        else:
            roots = sorted({z.root for z in self.zones})
            lo, hi = min(z.lokey for z in self.zones), max(z.hikey for z in self.zones)
            layers = max(sum(1 for z in self.zones if z.root == r) for r in roots)
            txt = f"Strumento melodico, {len(roots)} campion{'e' if len(roots) == 1 else 'i'} ({', '.join(note_name(r) for r in roots)})"
            if layers > 1:
                txt += f", fino a {layers} livelli di dinamica"
            txt += f", suona da {note_name(lo)} a {note_name(hi)}."
            lines.append(txt)
            looped = sum(1 for z in self.zones if z.mode == "loop")
            if looped:
                lines.append("Note tenute: il suono continua finché il tasto è premuto (loop)." if looped == len(self.zones)
                             else f"{looped} campioni su {len(self.zones)} hanno il loop per le note tenute.")
        return lines + self.notes


def _zone_ranges(roots: list[int]) -> list[tuple[int, int]]:
    """Each root covers the keys nearer to it than to its neighbours; the outer ones reach the keyboard's ends."""
    out = []
    for i, r in enumerate(roots):
        lo = 0 if i == 0 else (roots[i - 1] + r) // 2 + 1
        hi = 127 if i == len(roots) - 1 else (r + roots[i + 1]) // 2
        out.append((lo, hi))
    return out


def build(sounds: list[Sound], opt: Options) -> Instrument:
    if not sounds:
        raise ValueError("nessun suono utilizzabile nei file")
    notes: list[str] = []

    # The note of each sound: written in its file name > detected > forced for a lone sample
    roots: list[int | None] = []
    tunes: list[int] = []
    for s in sounds:
        named = note_in_name(s.source) if s.index_in_source == 0 and sum(1 for t in sounds if t.source == s.source) == 1 else None
        if named is not None:
            roots.append(named)
            # the name gives the note, the analysis the fine tuning, if it agrees within a semitone
            if s.pitch is not None and abs(s.pitch - named) < 1:
                tunes.append(int(round((s.pitch - named) * 100)))
            else:
                if s.pitch is not None and abs(s.pitch - named) % 12 > 1 and abs(s.pitch - named) % 12 < 11:
                    notes.append(f"'{s.source}': il nome dice {note_name(named)}, ma suona come {note_name(round(s.pitch))}; usato {note_name(named)}.")
                tunes.append(0)
        elif s.pitch is not None:
            r = int(round(s.pitch))
            roots.append(r)
            tunes.append(int(round((s.pitch - r) * 100)))
        else:
            roots.append(None)
            tunes.append(0)

    pitched = sum(r is not None for r in roots)
    kind = opt.kind
    if kind == "auto":
        if len(sounds) == 1:
            kind = "melodic"
        elif pitched == len(sounds):
            kind = "melodic"
        elif pitched == 0 or any(drum_key(s.source) is not None for s in sounds):
            kind = "kit"
        else:
            kind = "melodic" if pitched >= len(sounds) / 2 else "kit"

    if kind == "kit":
        zones = _kit_zones(sounds)
        inst = Instrument(opt.name, "kit", sounds, zones, release=0.05, notes=notes)
    else:
        keep = [i for i, r in enumerate(roots) if r is not None]
        if not keep:
            # a lone sound with no pitch (a bell without a clear note, a noise): transposed from the chosen key
            r = opt.root if opt.root is not None else 60
            roots[0] = r
            keep = [0]
            notes.append(f"Il suono non ha un'altezza riconoscibile: suona alla sua altezza originale sul {note_name(r)} e viene trasposto sugli altri tasti.")
        dropped = [sounds[i].source for i in range(len(sounds)) if i not in keep]
        if dropped:
            notes.append("Senza altezza riconoscibile, non usati in uno strumento melodico: " + ", ".join(sorted(set(dropped))) + ".")
        if len(keep) == 1 and opt.root is not None:
            roots[keep[0]] = opt.root
            tunes[keep[0]] = 0
        zones = _melodic_zones(sounds, keep, roots, tunes, opt)
        inst = Instrument(opt.name, "melodic", sounds, zones, notes=notes)
        if any(z.mode == "release" for z in zones) and all(not sounds[z.sound].sustained for z in zones):
            inst.release = 0.4    # a plucked or struck sound rings a little after the key is up

    if opt.attack is not None:
        inst.attack = max(0.0, opt.attack)
    if opt.release is not None:
        inst.release = max(0.005, opt.release)
    _compact(inst)
    _level(inst)
    return inst


def _compact(inst: Instrument) -> None:
    """Only the sounds a zone plays go into the instrument."""
    used = sorted({z.sound for z in inst.zones})
    index = {old: new for new, old in enumerate(used)}
    inst.sounds = [inst.sounds[i] for i in used]
    for z in inst.zones:
        z.sound = index[z.sound]


def _melodic_zones(sounds: list[Sound], keep: list[int], roots, tunes, opt: Options) -> list[Zone]:
    by_root: dict[int, list[int]] = {}
    for i in keep:
        by_root.setdefault(roots[i], []).append(i)
    ordered = sorted(by_root)
    zones = []
    for (lo, hi), r in zip(_zone_ranges(ordered), ordered):
        layer = sorted(by_root[r], key=lambda i: sounds[i].loudness)
        n = len(layer)
        for k, i in enumerate(layer):
            s = sounds[i]
            loop = s.loop if opt.loop != "off" else None
            if opt.loop == "on" and loop is None:
                loop = (int(len(s.data) * 0.5), len(s.data) - 64) if len(s.data) > 256 else None
            mode = "loop" if loop else "release"
            if loop and s.loop is None:
                s.loop = loop
            if not loop:
                s.loop = None
            zones.append(Zone(sound=i, lokey=lo, hikey=hi, lovel=1 + (127 * k) // n, hivel=(127 * (k + 1)) // n,
                              root=r, tune=-tunes[i], mode=mode))
    return zones


def _kit_zones(sounds: list[Sound]) -> list[Zone]:
    used: set[int] = set()
    keys: list[int | None] = []
    for s in sounds:
        k = drum_key(s.source) if s.index_in_source == 0 else None
        if k is not None and k in used:
            k = None
        if k is not None:
            used.add(k)
        keys.append(k)
    # the others one per key: from C2 when nothing was recognised, from C4 if some sounds went to their GM key
    nxt = 60 if used else 36
    zones = []
    for i, k in enumerate(keys):
        if k is None:
            while nxt in used and nxt < 127:
                nxt += 1
            k = nxt
            used.add(k)
        group = 1 if k in (CLOSED_HAT, OPEN_HAT, PEDAL_HAT) else 0
        sounds[i].loop = None
        zones.append(Zone(sound=i, lokey=k, hikey=k, root=k, mode="oneshot", group=group))
    return zones


def _level(inst: Instrument) -> None:
    """Even level over the keyboard, peaks at -1 dB.

    Melodic: samples recorded at different levels are brought to the same loudness (within ±12 dB), velocity
    layers of one note keep their difference. Kit: one gain for all, the user balanced the kit already."""
    gains = np.ones(len(inst.sounds))
    if inst.kind == "melodic":
        roots = sorted({z.root for z in inst.zones})
        loudest = {r: max(inst.sounds[z.sound].loudness for z in inst.zones if z.root == r) for r in roots}
        ref = float(np.median([v for v in loudest.values() if v > 0])) if any(v > 0 for v in loudest.values()) else 1
        for z in inst.zones:
            v = loudest[z.root]
            if v > 0:
                gains[z.sound] = float(np.clip(ref / v, 10 ** (-12 / 20), 10 ** (12 / 20)))
    peak = max(float(np.max(np.abs(s.data))) * g for s, g in zip(inst.sounds, gains)) or 1.0
    scale = 10 ** (-1 / 20) / peak
    for s, g in zip(inst.sounds, gains):
        s.data = (s.data * (g * scale)).astype(np.float32)
