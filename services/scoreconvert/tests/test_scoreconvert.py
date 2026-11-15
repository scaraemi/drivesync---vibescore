"""Run with: MSCORE=mscore pytest tests  (the audio tests need ffmpeg and MuseScore to render audio)."""

import os
import shutil
import subprocess
from pathlib import Path

import pretty_midi
import pytest

from scoreconvert.detect import Kind, detect
from scoreconvert.pipeline import ConversionError, Options, convert

MSCORE = os.environ.get("MSCORE", "mscore")
needs_mscore = pytest.mark.skipif(not shutil.which(MSCORE), reason="MuseScore not installed")

# "Fra Martino" in F, 92 BPM: (pitch, beats)
MELODY = [(65, 1), (67, 1), (69, 1), (65, 1)] * 2 + [(69, 1), (70, 1), (72, 2)] * 2 \
    + [(72, .5), (74, .5), (72, .5), (70, .5), (69, 1), (65, 1)] * 2 + [(65, 1), (60, 1), (65, 2)] * 2


def melody_midi(path: Path, program: int = 73, tempo: float = 92) -> None:
    pm = pretty_midi.PrettyMIDI(initial_tempo=tempo)
    inst = pretty_midi.Instrument(program=program)
    t, spb = 0.0, 60 / tempo
    for p, d in MELODY:
        inst.notes.append(pretty_midi.Note(velocity=90, pitch=p, start=t * spb, end=(t + d) * spb - 0.02))
        t += d
    pm.instruments.append(inst)
    pm.write(str(path))


def grid_notes(path: Path) -> list[tuple[float, int]]:
    pm = pretty_midi.PrettyMIDI(str(path))
    spb = 60 / pm.get_tempo_changes()[1][0]
    notes = sorted((round(n.start / spb * 4) / 4, n.pitch) for i in pm.instruments for n in i.notes)
    t0 = notes[0][0]
    return [(s - t0, p) for s, p in notes]


def test_detect_by_content(tmp_path: Path) -> None:
    melody_midi(tmp_path / "a.mid")
    shutil.copy(tmp_path / "a.mid", tmp_path / "whatsapp.bin")
    (tmp_path / "doc.pdf").write_bytes(b"%PDF-1.7\n...")
    (tmp_path / "photo.dat").write_bytes(b"\xff\xd8\xff\xe0" + b"\0" * 20)
    (tmp_path / "x.abc").write_text("X:1\nT:t\nK:C\nCDEF|\n")
    (tmp_path / "voice.m4a").write_bytes(b"\0\0\0\x20ftypM4A " + b"\0" * 20)
    assert detect(tmp_path / "whatsapp.bin") is Kind.NOTATION
    assert detect(tmp_path / "doc.pdf") is Kind.PDF
    assert detect(tmp_path / "photo.dat") is Kind.IMAGE
    assert detect(tmp_path / "x.abc") is Kind.ABC
    assert detect(tmp_path / "voice.m4a") is Kind.AUDIO


@needs_mscore
def test_midi_under_wrong_name(tmp_path: Path) -> None:
    melody_midi(tmp_path / "a.mid")
    shutil.copy(tmp_path / "a.mid", tmp_path / "Canone.txt")
    r = convert(tmp_path / "Canone.txt", tmp_path / "out")
    assert r.engine == "musescore" and r.mscz.stat().st_size > 0 and r.mscz.name == "Canone.mscz"


def test_unknown_file(tmp_path: Path) -> None:
    (tmp_path / "x.bin").write_bytes(b"nothing musical here")
    with pytest.raises(ConversionError):
        convert(tmp_path / "x.bin", tmp_path / "out")


@needs_mscore
@pytest.mark.parametrize("noisy", [False, True])
def test_recording_to_score(tmp_path: Path, noisy: bool) -> None:
    ref = tmp_path / "ref.mid"
    melody_midi(ref, program=0)  # piano: clear attacks. Legato repeated notes are a known weak spot.
    wav = tmp_path / "piano.wav"
    subprocess.run([MSCORE, "-o", str(wav), str(ref)], check=True, capture_output=True,
                   env=dict(os.environ, QT_QPA_PLATFORM="offscreen"))
    src = wav
    if noisy:  # a phone in a room: brown + pink noise, MP3
        src = tmp_path / "room.mp3"
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(wav),
                        "-f", "lavfi", "-i", "anoisesrc=color=brown:amplitude=0.25:d=60",
                        "-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.05:d=60",
                        "-filter_complex", "[0]aresample=44100[a];[a][1][2]amix=inputs=3:duration=first",
                        str(src)], check=True)

    r = convert(src, tmp_path / "out")
    print(f"audio {'noisy' if noisy else 'clean'}: {r.details}")
    assert r.kind == "audio" and r.experimental
    assert r.details["profile"] == ("noisy" if noisy else "clean")
    assert abs(r.details["tempo"] - 92) < 3
    expected = set(grid_notes(ref))
    got = set(grid_notes(r.mid))
    print("  missing:", sorted(expected - got), " extra:", sorted(got - expected))
    assert len(expected & got) >= 0.9 * len(expected)
    assert len(got - expected) <= 0.1 * len(expected)


needs_audiveris = pytest.mark.skipif(not shutil.which(os.environ.get("AUDIVERIS", "audiveris")),
                                     reason="Audiveris not installed")


@needs_mscore
@needs_audiveris
@pytest.mark.parametrize("as_photo", [False, True])
def test_printed_sheet_to_score(tmp_path: Path, as_photo: bool) -> None:
    """Engrave a known melody to PDF with MuseScore, read it back with OMR, compare the notes."""
    ref = tmp_path / "ref.mid"
    melody_midi(ref, program=0)
    pdf = tmp_path / "Fra Martino.pdf"
    subprocess.run([MSCORE, "-o", str(pdf), str(ref)], check=True, capture_output=True,
                   env=dict(os.environ, QT_QPA_PLATFORM="offscreen"))
    src = pdf
    if as_photo:  # uneven light, a slight tilt, JPEG
        import cv2
        import numpy as np

        subprocess.run(["pdftoppm", "-r", "200", "-png", "-singlefile", str(pdf), str(tmp_path / "page")], check=True)
        img = cv2.imread(str(tmp_path / "page.png"))
        h, w = img.shape[:2]
        img = (img * np.tile(np.linspace(0.5, 1.0, w), (h, 1))[..., None] * 0.85 + 15).astype(np.uint8)
        img = cv2.warpAffine(img, cv2.getRotationMatrix2D((w / 2, h / 2), 1.0, 1.0), (w, h),
                             borderValue=(120, 120, 120))
        src = tmp_path / "photo.jpg"
        cv2.imwrite(str(src), img, [cv2.IMWRITE_JPEG_QUALITY, 85])

    r = convert(src, tmp_path / "out", Options(sheet="printed"))
    assert r.engine == "audiveris"
    expected = [p for _, p in grid_notes(ref)]
    got = [p for _, p in grid_notes(r.mid)]
    import difflib

    match = difflib.SequenceMatcher(a=expected, b=got).ratio()
    print(f"OMR {'photo' if as_photo else 'pdf'}: {len(got)} notes, sequence match {match:.2f}")
    assert match > 0.85


class FakeDrive:
    """In-memory stand-in for drive_worker.Drive: folders and files by id."""

    def __init__(self) -> None:
        self.items: dict[str, dict] = {"root": {"name": "Partiture", "folder": True, "parent": None}}
        self.n = 0

    def _add(self, parent: str, name: str, folder: bool, data: bytes = b"", description: str = "") -> str:
        self.n += 1
        fid = f"id{self.n}"
        self.items[fid] = {"name": name, "folder": folder, "parent": parent, "data": data, "description": description}
        return fid

    def children(self, folder: str, folders=None) -> list[dict]:
        return [{"id": k, "name": v["name"]} for k, v in self.items.items()
                if v["parent"] == folder and (folders is None or v["folder"] == folders)]

    def subfolder(self, parent: str, name: str, create: bool = True):
        for f in self.children(parent, folders=True):
            if f["name"] == name:
                return f["id"]
        return self._add(parent, name, True) if create else None

    def download(self, fid: str, dst: Path) -> None:
        dst.write_bytes(self.items[fid]["data"])

    def upload(self, parent: str, name: str, src: Path, mime: str, description: str = "") -> dict:
        return {"id": self._add(parent, name, False, src.read_bytes(), description)}

    def move(self, fid: str, src: str, dst: str, description=None) -> None:
        self.items[fid]["parent"] = dst
        if description is not None:
            self.items[fid]["description"] = description

    def names(self, folder: str) -> list[str]:
        return sorted(f["name"] for f in self.children(folder, folders=False))


@needs_mscore
def test_drive_worker(tmp_path: Path) -> None:
    from scoreconvert.drive_worker import run

    melody_midi(tmp_path / "a.mid")
    d = FakeDrive()
    d._add("root", "Canone.mscz", False, b"existing score")
    inbox = d.subfolder("root", "Da convertire")
    d._add(inbox, "Canone.mid", False, (tmp_path / "a.mid").read_bytes())
    d._add(inbox, "rotto.pdf", False, b"%PDF-1.4 not really")

    run(d, "root")

    assert d.names("root") == ["Canone (2).mscz", "Canone.mscz"]  # never overwrites
    done, failed = d.subfolder(inbox, "Convertiti"), d.subfolder(inbox, "Errori")
    assert d.names(inbox) == []
    assert d.names(done) == ["Canone.mid", "Canone.mid"]  # original and exported MIDI
    assert d.names(failed) == ["rotto.pdf"]
    bad = next(v for v in d.items.values() if v["name"] == "rotto.pdf")
    assert bad["description"].startswith("Conversione non riuscita")
