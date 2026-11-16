"""Reading any audio file (through ffmpeg) and writing WAV."""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

MAX_RATE = 48000
MAX_SECONDS = 120  # per file: a sample, not an album


class AudioError(Exception):
    pass


def probe(path: Path) -> dict:
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                          "stream=sample_rate,channels:format=duration", "-of", "json", str(path)],
                         capture_output=True, text=True)
    info = json.loads(out.stdout or "{}")
    streams = info.get("streams") or []
    if out.returncode != 0 or not streams:
        raise AudioError("non è un file audio leggibile")
    return {"rate": int(streams[0].get("sample_rate") or 44100), "channels": int(streams[0].get("channels") or 1),
            "duration": float((info.get("format") or {}).get("duration") or 0)}


def _load_direct(path: Path) -> tuple[np.ndarray, int] | None:
    """WAV, AIFF, FLAC, OGG through libsndfile, without ffmpeg; None if it cannot read the file."""
    try:
        data, sr = sf.read(str(path), dtype="float32", always_2d=True, frames=MAX_SECONDS * 192000)
    except Exception:  # noqa: BLE001 - not a format libsndfile knows
        return None
    data = data[: MAX_SECONDS * sr]
    if data.shape[1] > 2:
        data = np.stack([data[:, 0::2].mean(axis=1), data[:, 1::2].mean(axis=1)], axis=1)
    if sr > MAX_RATE:
        from scipy.signal import resample_poly
        g = np.gcd(sr, MAX_RATE)
        data = resample_poly(data, MAX_RATE // g, sr // g, axis=0).astype(np.float32)
        sr = MAX_RATE
    return data, sr


def load(path: Path) -> tuple[np.ndarray, int]:
    """Any format ffmpeg reads (WAV, AIFF, FLAC, MP3, M4A, OGG, video...) -> float32 (frames, channels), rate.

    At most 2 channels and 48 kHz: a 96 kHz sample sounds the same once played, and is twice as big."""
    if not shutil.which("ffmpeg"):
        direct = _load_direct(path)
        if direct is None:
            raise AudioError("formato non leggibile senza ffmpeg")
        return _finish(*direct)
    info = probe(path)
    rate = min(info["rate"], MAX_RATE)
    channels = min(info["channels"], 2)
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / "a.wav"
        cmd = ["ffmpeg", "-v", "error", "-nostdin", "-y", "-i", str(path), "-vn", "-t", str(MAX_SECONDS),
               "-ac", str(channels), "-ar", str(rate), "-c:a", "pcm_f32le", str(wav)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not wav.exists():
            raise AudioError(f"decodifica non riuscita: {r.stderr.strip()[:200]}")
        data, sr = sf.read(str(wav), dtype="float32", always_2d=True)
    return _finish(data, sr)


def _finish(data: np.ndarray, sr: int) -> tuple[np.ndarray, int]:
    if len(data) == 0:
        raise AudioError("il file è vuoto")
    # Two identical channels are mono: half the size, and the SoundFont needs one sample instead of two
    if data.shape[1] == 2 and np.max(np.abs(data[:, 0] - data[:, 1])) < 1e-4:
        data = data[:, :1]
    return np.ascontiguousarray(data), sr


def save_wav(path: Path, data: np.ndarray, rate: int, subtype: str = "PCM_24") -> None:
    sf.write(str(path), np.clip(data, -1.0, 1.0), rate, subtype=subtype)
