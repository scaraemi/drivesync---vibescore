"""Decide what a file is from its content, not only its name."""

from __future__ import annotations

import zipfile
from enum import Enum
from pathlib import Path


class Kind(str, Enum):
    NOTATION = "notation"  # MuseScore imports it as is (MIDI, MusicXML, Guitar Pro, ...)
    ABC = "abc"            # text notation MuseScore 4 no longer imports: converted with music21
    PDF = "pdf"            # printed or scanned sheet music
    IMAGE = "image"        # photo or scan of one page
    AUDIO = "audio"        # recording
    UNKNOWN = "unknown"


# Formats MuseScore 4 opens directly (src/importexport in the fork).
NOTATION_EXT = {
    ".mid", ".midi", ".kar",
    ".musicxml", ".xml", ".mxl",
    ".gp", ".gp3", ".gp4", ".gp5", ".gpx", ".gtp", ".gp6", ".gp7",
    ".ptb", ".cap", ".capx", ".bww", ".mei", ".ove", ".scw", ".bmw", ".bmx",
    ".mscz", ".mscx", ".mscs", ".mss",
}
IMAGE_EXT = {".png", ".jpg", ".jpeg", ".tif", ".tiff", ".bmp", ".webp", ".heic", ".heif"}
AUDIO_EXT = {
    ".wav", ".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".wma", ".aif", ".aiff",
    ".amr", ".3gp", ".webm", ".mp4", ".mov", ".mkv",
}


def detect(path: Path) -> Kind:
    head = path.read_bytes()[:64] if path.stat().st_size else b""
    ext = path.suffix.lower()

    if head.startswith(b"%PDF"):
        return Kind.PDF
    if head.startswith(b"MThd") or head[8:12] == b"RMID":
        return Kind.NOTATION
    if (head.startswith(b"\x89PNG") or head.startswith(b"\xff\xd8\xff") or head[:4] in (b"II*\x00", b"MM\x00*")
            or head.startswith(b"BM") or (head[:4] == b"RIFF" and head[8:12] == b"WEBP")
            or head[4:12] in (b"ftypheic", b"ftypheix", b"ftypmif1", b"ftypheif")):
        return Kind.IMAGE
    if (head[:4] == b"RIFF" and head[8:12] == b"WAVE") or head.startswith(b"ID3") or head[:2] in (b"\xff\xfb", b"\xff\xf3", b"\xff\xf2") \
            or head.startswith(b"fLaC") or head.startswith(b"OggS") or head.startswith(b"FORM") or head.startswith(b"#!AMR") \
            or head.startswith(b"\x1aE\xdf\xa3") or head.startswith(b"0&\xb2u"):
        return Kind.AUDIO
    if head[4:8] == b"ftyp":  # MP4 family: m4a, mp4, mov, 3gp
        return Kind.AUDIO
    if head.startswith(b"PK"):
        try:
            with zipfile.ZipFile(path) as z:
                names = z.namelist()
            if "META-INF/container.xml" in names or any(n.endswith((".musicxml", ".xml")) for n in names) and ext == ".mxl":
                return Kind.NOTATION
            if any(n.endswith(".mscx") for n in names):
                return Kind.NOTATION
        except zipfile.BadZipFile:
            pass

    if ext in NOTATION_EXT:
        return Kind.NOTATION
    if ext == ".abc":
        return Kind.ABC
    if ext in IMAGE_EXT:
        return Kind.IMAGE
    if ext in AUDIO_EXT:
        return Kind.AUDIO

    text = head.lstrip()
    if text.startswith(b"<?xml") or text.startswith(b"<score-partwise") or text.startswith(b"<mei"):
        return Kind.NOTATION
    if text.startswith(b"X:"):
        return Kind.ABC
    return Kind.UNKNOWN
