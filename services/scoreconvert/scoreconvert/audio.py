"""Recording -> MIDI that reads well as a score.

Basic Pitch finds the notes, in seconds. A score needs beats: the notes are moved onto the beats found by
librosa's beat tracker and snapped to a grid, then written at one steady tempo. Without that step MuseScore's
MIDI import turns small timing differences into ties, dots and tuplets.
"""

from __future__ import annotations

import logging
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

log = logging.getLogger(__name__)

SR = 22050


@dataclass
class AudioOptions:
    # "auto" looks at the noise in the recording; "clean" and "noisy" force a profile.
    profile: str = "auto"
    # Keep only the strongest note at any time (voice, flute, a single line).
    # None: decide from the recording (few notes start together -> one line).
    monophonic: bool | None = None
    # Grid in fractions of a beat: 4 = sixteenths, 2 = eighths, 3 = triplet eighths.
    grid: int = 4
    tempo: float | None = None  # force a tempo instead of detecting it
    min_pitch: int = 21  # A0
    max_pitch: int = 108  # C8


@dataclass
class AudioReport:
    profile: str
    monophonic: bool
    tempo: float
    notes: int
    duration: float
    noise_db: float
    warnings: list[str] = field(default_factory=list)


def to_wav(src: Path, dst: Path, denoise: bool) -> None:
    """Any audio or video container -> mono 22.05 kHz WAV. Optional mild clean-up for room/outdoor recordings."""
    filters = ["highpass=f=50", "lowpass=f=9000"]
    if denoise:
        # FFT denoiser with noise tracking, plus a gate on the quietest parts.
        filters += ["afftdn=nr=18:nf=-35:tn=1", "agate=threshold=0.015:ratio=3:attack=5:release=120"]
    filters.append("dynaudnorm=f=200:g=11")
    subprocess.run(
        ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", str(src), "-vn", "-ac", "1", "-ar", str(SR),
         "-af", ",".join(filters), str(dst)],
        check=True,
    )


def noise_floor_db(y: np.ndarray) -> float:
    """Background level: the quietest 10% of the recording, in dB below its peak.

    A clean recording sits around -60 dB between notes; a phone in a room, outdoors or in a church is
    usually above -45 dB.
    """
    import librosa

    peak = float(np.max(np.abs(y))) or 1.0
    rms = librosa.feature.rms(y=y / peak, frame_length=2048, hop_length=512)[0]
    q = rms[rms <= np.percentile(rms, 10)]
    return float(20 * np.log10(np.median(q) + 1e-9))


def _beats(y: np.ndarray, forced_tempo: float | None) -> tuple[float, np.ndarray]:
    import librosa

    if forced_tempo:
        duration = len(y) / SR
        step = 60.0 / forced_tempo
        return forced_tempo, np.arange(0.0, duration + step, step)

    onset_env = librosa.onset.onset_strength(y=y, sr=SR)
    tempo, frames = librosa.beat.beat_track(onset_envelope=onset_env, sr=SR, units="frames", trim=False)
    tempo = float(np.atleast_1d(tempo)[0])
    beats = librosa.frames_to_time(frames, sr=SR)
    if beats.size < 4 or not 30 <= tempo <= 260:
        return 0.0, beats
    # Fold extreme tempi into a readable range (a tracker often locks on half or double time).
    while tempo > 160:
        tempo /= 2
        beats = beats[::2]
    while tempo < 55:
        tempo *= 2
        beats = np.sort(np.concatenate([beats, (beats[:-1] + beats[1:]) / 2]))
    return tempo, beats


def _time_to_beat(times: np.ndarray, beats: np.ndarray) -> np.ndarray:
    """Piecewise-linear map from seconds to beat number, extended past both ends at the local beat length."""
    idx = np.arange(beats.size, dtype=float)
    out = np.interp(times, beats, idx)
    first = beats[1] - beats[0]
    last = beats[-1] - beats[-2]
    out = np.where(times < beats[0], (times - beats[0]) / first, out)
    out = np.where(times > beats[-1], idx[-1] + (times - beats[-1]) / last, out)
    return out


def transcribe(src: Path, work: Path, out_mid: Path, opt: AudioOptions) -> AudioReport:
    import librosa
    import pretty_midi
    from basic_pitch import ICASSP_2022_MODEL_PATH
    from basic_pitch.inference import predict

    warnings: list[str] = []

    raw = work / "raw.wav"
    to_wav(src, raw, denoise=False)
    y_raw, _ = librosa.load(raw, sr=SR, mono=True)
    duration = len(y_raw) / SR
    if duration < 1.0:
        raise ValueError("La registrazione dura meno di un secondo.")
    noise = noise_floor_db(y_raw)

    profile = opt.profile
    if profile == "auto":
        profile = "noisy" if noise > -45 else "clean"

    wav = raw
    if profile == "noisy":
        wav = work / "clean.wav"
        to_wav(src, wav, denoise=True)
    y, _ = librosa.load(wav, sr=SR, mono=True)

    # Noisy recordings: higher thresholds and longer minimum notes, or the noise becomes a cloud of short notes.
    onset, frame, min_ms = (0.5, 0.3, 80) if profile == "clean" else (0.62, 0.42, 130)
    _, _, events = predict(
        str(wav),
        ICASSP_2022_MODEL_PATH,
        onset_threshold=onset,
        frame_threshold=frame,
        minimum_note_length=min_ms,
        minimum_frequency=pretty_midi.note_number_to_hz(opt.min_pitch),
        maximum_frequency=pretty_midi.note_number_to_hz(opt.max_pitch),
        multiple_pitch_bends=False,
        melodia_trick=True,
    )
    notes = [(float(s), float(e), int(p), float(a)) for s, e, p, a, *_ in events]
    if not notes:
        raise ValueError("Nella registrazione non sono state trovate note.")

    tempo, beats = _beats(y, opt.tempo)
    if tempo == 0.0:
        tempo = 100.0
        beats = np.arange(0.0, duration + 0.6, 0.6)
        warnings.append("Tempo non riconosciuto: usato 100 BPM, il ritmo andrà controllato.")

    starts = _time_to_beat(np.array([n[0] for n in notes]), beats)
    ends = _time_to_beat(np.array([n[1] for n in notes]), beats)
    # Put the first note on the first downbeat. The beat tracker does not know where bars start; the first
    # note is the best guess, and a wrong guess only moves the barlines.
    shift = np.round(starts.min() * opt.grid) / opt.grid
    starts -= shift
    ends -= shift

    g = opt.grid
    qs = np.maximum(np.round(starts * g) / g, 0.0)
    qe = np.maximum(np.round(ends * g) / g, qs + 1 / g)

    items = sorted(zip(qs, qe, (n[2] for n in notes), (n[3] for n in notes)), key=lambda t: (t[0], -t[3]))
    monophonic = opt.monophonic
    if monophonic is None:
        onsets = [t[0] for t in items]
        together = len(onsets) - len(set(onsets))
        # Reverb tails and overtones make a few stray chords even in a single line.
        monophonic = together < 0.12 * len(onsets)
    if monophonic:
        mono: list[tuple] = []
        for s, e, p, a in items:
            if mono and s < mono[-1][1]:
                ps, pe, pp, pa = mono[-1]
                if s == ps:
                    if a > pa:
                        mono[-1] = (s, e, p, a)
                    continue
                mono[-1] = (ps, s, pp, pa)  # cut the previous note where the new one starts
            mono.append((s, e, p, a))
        # Close gaps of up to a beat's fourth: notes released early read as notes plus rests otherwise.
        for i in range(len(mono) - 1):
            s, e, p, a = mono[i]
            nxt = mono[i + 1][0]
            if 0 < nxt - e <= 0.25 + 1e-9:
                mono[i] = (s, nxt, p, a)
        items = mono
    else:
        # Same pitch twice at the same time: keep one, the longest.
        seen: dict[tuple, tuple] = {}
        for s, e, p, a in items:
            k = (s, p)
            if k not in seen or e > seen[k][1]:
                seen[k] = (s, e, p, a)
        items = sorted(seen.values())
        # Overtones: a weak note an octave, a twelfth or two octaves above a stronger one starting together.
        by_start: dict[float, list[tuple]] = {}
        for it in items:
            by_start.setdefault(it[0], []).append(it)
        onset_set = {(it[0], it[2]) for it in items}
        kept = []
        for s, e, p, a in items:
            if any(q[2] in (p - 12, p - 19, p - 24) and a < 0.7 * q[3] for q in by_start[s]):
                continue
            # A one-step blip just before the same pitch starts again: the attack was seen twice.
            if e - s <= 1 / g + 1e-9 and (s + 1 / g, p) in onset_set:
                continue
            kept.append((s, e, p, a))
        items = kept

    spb = 60.0 / tempo
    pm = pretty_midi.PrettyMIDI(initial_tempo=tempo, resolution=480)
    inst = pretty_midi.Instrument(program=0, name="Trascrizione")
    for s, e, p, a in items:
        vel = int(np.clip(40 + a * 87, 30, 127))
        inst.notes.append(pretty_midi.Note(velocity=vel, pitch=p, start=s * spb, end=e * spb))
    pm.instruments.append(inst)
    pm.write(str(out_mid))

    density = len(items) / max(duration, 1.0)
    if profile == "noisy":
        warnings.append("Registrazione rumorosa: applicata riduzione del rumore, alcune note potrebbero mancare o essere spurie.")
    if density > 12:
        warnings.append("Molte note al secondo: probabilmente la registrazione contiene rumore o più strumenti insieme.")
    if noise > -15:
        warnings.append("Il segnale è quasi tutto rumore di fondo: la trascrizione è poco affidabile.")

    log.info("audio: %s, tempo %.1f, %d notes, noise %.1f dB", profile, tempo, len(items), noise)
    return AudioReport(profile=profile, monophonic=monophonic, tempo=round(tempo, 1), notes=len(items), duration=round(duration, 2),
                       noise_db=round(noise, 1), warnings=warnings)
