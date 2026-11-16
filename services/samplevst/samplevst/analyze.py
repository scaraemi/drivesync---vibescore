"""What is in a recording: where the sounds are, their pitch, whether they sustain, where to loop them.

A file can hold one note, several notes (a scale, one note after the other, or each separated by silence), or
sounds without pitch (drums, effects). Everything here works on numpy arrays: no heavy dependencies.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy.signal import resample_poly

HOP = 0.010          # analysis frame step, seconds
PITCH_RATE = 22050   # pitch tracking runs at this rate
FMIN, FMAX = 30.0, 3000.0


@dataclass
class Sound:
    """One sound cut out of a file, ready to be mapped on the keyboard."""
    data: np.ndarray                 # float32 (frames, channels)
    rate: int
    source: str                      # file name it came from
    index_in_source: int = 0         # 0, 1, 2... when a file held several sounds
    pitch: float | None = None       # detected pitch, MIDI note number with fraction (60.0 = C4)
    confidence: float = 0.0          # share of the sound's body with a clear pitch
    sustained: bool = False          # holds its level (organ, strings, voice) instead of dying away (piano, drum)
    loop: tuple[int, int] | None = None   # loop start/end, frames
    loudness: float = 0.0            # RMS of the body, before any gain (for velocity layers)
    notes: list[str] = field(default_factory=list)   # remarks for the user

    @property
    def duration(self) -> float:
        return len(self.data) / self.rate


def mono(x: np.ndarray) -> np.ndarray:
    return x.mean(axis=1) if x.ndim == 2 else x


def envelope_db(x: np.ndarray, rate: int, hop: float = HOP) -> np.ndarray:
    """RMS level per frame, dB re full scale."""
    h = max(1, int(rate * hop))
    n = len(x) // h
    if n == 0:
        return np.array([-120.0])
    frames = x[: n * h].reshape(n, h)
    rms = np.sqrt(np.mean(frames.astype(np.float64) ** 2, axis=1))
    return 20 * np.log10(np.maximum(rms, 1e-6))


# --------------------------------------------------------------------------------------------------------------
# Pitch: YIN (de Cheveigné & Kawahara 2002), vectorised with FFTs


def yin(x: np.ndarray, rate: int, threshold: float = 0.15) -> tuple[np.ndarray, np.ndarray]:
    """Per 10 ms frame: f0 in Hz (0 = no pitch) and clarity 0..1 (1 - the dip of the normalised difference)."""
    if rate != PITCH_RATE:
        g = np.gcd(int(rate), PITCH_RATE)
        x = resample_poly(x, PITCH_RATE // g, int(rate) // g)
    sr = PITCH_RATE
    tau_min, tau_max = int(sr / FMAX), int(sr / FMIN)
    w = 1024                                   # integration window: 46 ms
    hop = int(sr * HOP)
    n_frames = max(0, (len(x) - w - tau_max) // hop + 1)
    f0 = np.zeros(n_frames)
    clarity = np.zeros(n_frames)
    if n_frames == 0:
        return f0, clarity
    size = 1 << int(np.ceil(np.log2(w + tau_max)))
    for i in range(n_frames):
        seg = x[i * hop: i * hop + w + tau_max].astype(np.float64)
        if np.max(np.abs(seg)) < 1e-4:
            continue
        # d(tau) = sum (x_j - x_{j+tau})^2 over j < w = e0 + e_tau - 2 r(tau)
        a = np.fft.rfft(seg[:w], size)
        b = np.fft.rfft(seg, size)
        r = np.fft.irfft(np.conj(a) * b, size)[: tau_max + 1]
        sq = np.concatenate(([0.0], np.cumsum(seg ** 2)))
        e0 = sq[w]
        e_tau = sq[np.arange(tau_max + 1) + w] - sq[np.arange(tau_max + 1)]
        d = np.maximum(e0 + e_tau - 2 * r, 0.0)
        # cumulative mean normalised difference
        cm = np.ones_like(d)
        cs = np.cumsum(d[1:])
        cm[1:] = d[1:] * np.arange(1, tau_max + 1) / np.maximum(cs, 1e-12)
        below = np.nonzero(cm[tau_min:] < threshold)[0]
        if len(below):
            t = tau_min + below[0]
            while t + 1 <= tau_max and cm[t + 1] < cm[t]:
                t += 1
        else:
            t = tau_min + int(np.argmin(cm[tau_min:]))
            if cm[t] > 0.35:
                clarity[i] = 1 - min(1.0, cm[t])
                continue
        # parabolic interpolation around the dip
        if 1 <= t < tau_max:
            y0, y1, y2 = cm[t - 1], cm[t], cm[t + 1]
            den = y0 - 2 * y1 + y2
            tf = t + (0.5 * (y0 - y2) / den if abs(den) > 1e-12 else 0.0)
        else:
            tf = float(t)
        f0[i] = sr / tf
        clarity[i] = 1 - min(1.0, cm[t])
    return f0, clarity


def hz_to_midi(f: np.ndarray | float) -> np.ndarray | float:
    return 69 + 12 * np.log2(np.asarray(f) / 440.0)


def pitch_track(x: np.ndarray, rate: int) -> tuple[np.ndarray, np.ndarray]:
    """MIDI pitch per frame (nan = none) and clarity. Frame i covers about i*HOP .. i*HOP + 46 ms."""
    f0, clarity = yin(mono(x) if x.ndim == 2 else x, rate)
    midi = np.full(len(f0), np.nan)
    ok = (f0 > 0) & (clarity > 0.8)
    midi[ok] = hz_to_midi(f0[ok])
    return midi, clarity


# --------------------------------------------------------------------------------------------------------------
# Cutting a recording into sounds


def find_regions(x: np.ndarray, rate: int) -> list[tuple[int, int]]:
    """Sounds separated by silence: [(start, end)] in frames. A single sound gives one region.

    The floor follows the recording: 45 dB under its peak, or 10 dB over its background noise if that is
    higher (a room, a phone)."""
    env = envelope_db(mono(x), rate)
    peak = env.max()
    noise = np.percentile(env, 10)
    thr = max(peak - 45, noise + 10)
    if thr >= peak - 6:            # no contrast: one region (a pad, a drone, a noisy phone recording)
        return [(0, len(x))]
    on = env > thr
    h = int(rate * HOP)
    regions: list[list[int]] = []
    i = 0
    min_gap = int(0.15 / HOP)      # silences shorter than this are part of the sound (a tremolo, a flam)
    while i < len(on):
        if not on[i]:
            i += 1
            continue
        j = i
        while j < len(on):
            if on[j]:
                j += 1
                continue
            k = j
            while k < len(on) and not on[k]:
                k += 1
            if k - j >= min_gap or k == len(on):
                break
            j = k
        regions.append([i, j])
        i = j
    # drop crumbs: shorter than 40 ms or 30 dB under the loudest sound (a chair, a breath)
    keep = [r for r in regions if (r[1] - r[0]) * HOP >= 0.04 and env[r[0]:r[1]].max() > peak - 30]
    if not keep:
        return [(0, len(x))]
    # each sound keeps its tail until the next one starts
    out = []
    for n, (a, b) in enumerate(keep):
        start = max(0, a * h - int(0.005 * rate))
        end = keep[n + 1][0] * h - int(0.005 * rate) if n + 1 < len(keep) else len(x)
        out.append((start, max(start + 1, end)))
    return out


def split_by_pitch(x: np.ndarray, rate: int, start: int, end: int) -> list[tuple[int, int]]:
    """A region where the pitch changes and then holds (a legato scale, one note after the other) -> one part
    per note. Vibrato and glides stay one note: a change must be over 0.8 semitones and last 120 ms."""
    midi, _ = pitch_track(x[start:end], rate)
    env = envelope_db(mono(x[start:end]), rate)
    loud = env[: len(midi)] > env.max() - 35       # the quiet tail has octave errors: not a new note
    midi = np.where(loud[: len(midi)], midi, np.nan) if len(loud) >= len(midi) else midi
    if np.count_nonzero(~np.isnan(midi)) < 10:
        return [(start, end)]
    # notes = runs of frames within half a semitone of their running median
    runs: list[list[float]] = []      # [first frame, last frame, pitch]
    for i, m in enumerate(midi):
        if np.isnan(m):
            continue
        if runs and abs(m - runs[-1][2]) < 0.8 and i - runs[-1][1] <= 3:
            runs[-1][1] = i
            runs[-1][2] = 0.9 * runs[-1][2] + 0.1 * m
        else:
            runs.append([i, i, m])
    min_len = int(0.12 / HOP)
    runs = [r for r in runs if r[1] - r[0] >= min_len]
    # merge neighbours on the same note (a dip in clarity cut one note in two)
    merged: list[list[float]] = []
    for r in runs:
        # same note, or the same note an octave off (a pitch tracker's usual mistake)
        if merged and min(abs(r[2] - merged[-1][2]), abs(abs(r[2] - merged[-1][2]) - 12)) < 0.8:
            merged[-1][1] = r[1]
        else:
            merged.append(list(r))
    if len(merged) < 2:
        return [(start, end)]
    h = int(rate * HOP)
    cuts = [start] + [start + int(r[0]) * h for r in merged[1:]] + [end]
    return [(cuts[i], cuts[i + 1]) for i in range(len(cuts) - 1) if cuts[i + 1] - cuts[i] > int(0.05 * rate)]


# --------------------------------------------------------------------------------------------------------------
# One sound: trim, measure, loop


def trim(x: np.ndarray, rate: int) -> np.ndarray:
    """Cut the silence before the attack and the tail under -60 dB; fade the ends so nothing clicks."""
    m = np.abs(mono(x))
    peak = float(m.max())
    if peak <= 0:
        return x[:1]
    above = np.nonzero(m > peak * 10 ** (-50 / 20))[0]
    a = max(0, int(above[0]) - int(0.001 * rate))
    env = envelope_db(mono(x[a:]), rate)
    peak_db = env.max()
    live = np.nonzero(env > peak_db - 60)[0]
    b = a + min(len(x) - a, int((live[-1] + 1) * rate * HOP + 0.02 * rate)) if len(live) else len(x)
    y = x[a:b].copy()
    if a > 0:
        n = min(32, len(y))
        y[:n] *= np.linspace(0, 1, n, dtype=np.float32)[:, None]
    n = min(int(0.02 * rate), len(y) // 4)
    if n > 1:
        y[-n:] *= np.linspace(1, 0, n, dtype=np.float32)[:, None]
    return y


def measure(sound: Sound) -> None:
    """Pitch, clarity, sustain and loudness of a trimmed sound."""
    x = sound.data
    rate = sound.rate
    env = envelope_db(mono(x), rate)
    peak_db = float(env.max())
    # the body: from the end of the attack (first frame within 3 dB of the peak) to the last frame within 30 dB
    attack_end = int(np.argmax(env > peak_db - 3))
    loud = np.nonzero(env > peak_db - 30)[0]
    a = attack_end + int(0.03 / HOP)
    b = max(a + 1, int(loud[-1]) + 1)
    h = int(rate * HOP)
    seg = mono(x)[a * h: min(len(x), (a + int(0.3 / HOP)) * h)]
    sound.loudness = float(np.sqrt(np.mean(seg.astype(np.float64) ** 2))) if len(seg) else 0.0

    midi, _ = pitch_track(x, rate)
    body = midi[a: min(b, len(midi))]
    if len(body) >= 5:
        good = body[~np.isnan(body)]
        sound.confidence = len(good) / len(body)
        if len(good) >= 5:
            med = float(np.median(good))
            spread = float(np.percentile(good, 75) - np.percentile(good, 25))
            # octave errors in a few frames do not move the median; a real glide or noise has a wide spread
            if sound.confidence >= 0.5 and spread < 1.0:
                sound.pitch = med
    # sustained: at 60 % of its length the sound is still within 12 dB of the peak, and it lasts 0.6 s
    i60 = int(len(env) * 0.6)
    sound.sustained = sound.duration >= 0.6 and float(np.median(env[i60:i60 + 10])) > peak_db - 12


def find_loop(sound: Sound) -> tuple[int, int] | None:
    """Loop points for a sustained pitched sound, so a held note can last as long as the key is down.

    The loop is a whole number of periods, at the most similar spot in the steady part, and the samples before
    the loop end are crossfaded with those before the loop start: the jump is inaudible."""
    if sound.pitch is None or not sound.sustained:
        return None
    x = sound.data
    rate = sound.rate
    m = mono(x).astype(np.float64)
    period = rate / (440.0 * 2 ** ((sound.pitch - 69) / 12))
    n = len(m)
    env = envelope_db(m, rate)
    attack_end = int(np.argmax(env > env.max() - 3))      # first frame near full level
    steady_from = max(int((attack_end + int(0.1 / HOP)) * rate * HOP), int(0.25 * n))
    end_limit = int(0.88 * n)
    if end_limit - steady_from < int(0.3 * rate):
        return None
    periods = max(1, int(round(min(0.8 * rate, (end_limit - steady_from) * 0.7) / period)))
    length = int(round(periods * period))
    win = max(64, int(2 * period))
    best, best_score = None, -2.0
    # candidate ends: upward zero crossings in the last third of the steady part
    zc = np.nonzero((m[:-1] <= 0) & (m[1:] > 0))[0]
    ends = zc[(zc > steady_from + length + win) & (zc < end_limit)]
    if len(ends) == 0:
        return None
    for e in ends[:: max(1, len(ends) // 60)]:
        s_guess = e - length
        lo, hi = int(s_guess - period / 2), int(s_guess + period / 2)
        cand = zc[(zc >= max(win, lo)) & (zc <= hi)]
        for s in cand:
            a1 = m[s - win: s + win]
            a2 = m[e - win: e + win]
            if len(a1) != len(a2) or len(a1) == 0:
                continue
            den = np.sqrt(np.dot(a1, a1) * np.dot(a2, a2))
            score = float(np.dot(a1, a2) / den) if den > 0 else -1
            # similar level at both ends, or the loop pumps
            score -= abs(float(np.sqrt(np.mean(a1 ** 2)) - np.sqrt(np.mean(a2 ** 2)))) / (np.sqrt(np.mean(a2 ** 2)) + 1e-9)
            if score > best_score:
                best, best_score = (int(s), int(e)), score
    if best is None or best_score < 0.5:
        return None
    s, e = best
    cf = min(int(0.1 * rate), (e - s) // 2, s)
    if cf > 8:
        fade = np.linspace(0, 1, cf, dtype=np.float32)[:, None]
        x[e - cf: e] = x[e - cf: e] * (1 - fade) + x[s - cf: s] * fade
    # keep a little after the loop end (SoundFont and interpolation read past it), drop the rest
    keep = min(len(x), e + 64)
    sound.data = x[:keep].copy()
    if best_score < 0.85:
        sound.notes.append("loop non perfetto: una nota tenuta a lungo può pulsare leggermente")
    return s, e


def analyse_file(data: np.ndarray, rate: int, name: str, split: bool = True) -> list[Sound]:
    """A decoded file -> the sounds in it, trimmed and measured, loops found."""
    regions = find_regions(data, rate) if split else [(0, len(data))]
    parts: list[tuple[int, int]] = []
    for a, b in regions:
        parts += split_by_pitch(data, rate, a, b) if split else [(a, b)]
    sounds = []
    for k, (a, b) in enumerate(parts):
        y = trim(data[a:b], rate)
        if len(y) < int(0.02 * rate):
            continue
        s = Sound(data=y.astype(np.float32), rate=rate, source=name, index_in_source=k)
        measure(s)
        s.loop = find_loop(s)
        sounds.append(s)
    return sounds
