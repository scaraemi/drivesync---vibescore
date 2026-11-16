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


def load(path: Path) -> tuple[np.ndarray, int]:
    """Any format ffmpeg reads (WAV, AIFF, FLAC, MP3, M4A, OGG, video...) -> float32 (frames, channels), rate.

    At most 2 channels and 48 kHz: a 96 kHz sample sounds the same once played, and is twice as big."""
    if not shutil.which("ffmpeg"):
        raise AudioError("ffmpeg non installato")
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
    if len(data) == 0:
        raise AudioError("il file è vuoto")
    # Two identical channels are mono: half the size, and the SoundFont needs one sample instead of two
    if data.shape[1] == 2 and np.max(np.abs(data[:, 0] - data[:, 1])) < 1e-4:
        data = data[:, :1]
    return np.ascontiguousarray(data), sr


def save_wav(path: Path, data: np.ndarray, rate: int, subtype: str = "PCM_24") -> None:
    sf.write(str(path), np.clip(data, -1.0, 1.0), rate, subtype=subtype)
