"""One entry point: any music file -> .mscz and .mid, plus a report of what was done and how reliable it is."""

from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable

from . import omr
from .audio import AudioOptions, transcribe
from .detect import Kind, detect

log = logging.getLogger(__name__)

MSCORE = os.environ.get("MSCORE", "mscore")
MSCORE_TIMEOUT = int(os.environ.get("MSCORE_TIMEOUT", "300"))


class ConversionError(Exception):
    """A failure the user should read (Italian message)."""


@dataclass
class Options:
    # "auto" | "printed" | "handwritten": how to read images and PDFs
    sheet: str = "auto"
    audio: AudioOptions = field(default_factory=AudioOptions)
    title: str | None = None


@dataclass
class Result:
    kind: str
    engine: str
    mscz: Path
    mid: Path | None
    experimental: bool = False
    warnings: list[str] = field(default_factory=list)
    details: dict = field(default_factory=dict)

    def to_json(self) -> dict:
        d = asdict(self)
        d["mscz"] = self.mscz.name
        d["mid"] = self.mid.name if self.mid else None
        return d


Progress = Callable[[str], None]


def mscore(src: Path, dst: Path) -> None:
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_RUNTIME_DIR=tempfile.gettempdir())
    try:
        proc = subprocess.run([MSCORE, "-o", str(dst), str(src)], capture_output=True, text=True,
                              timeout=MSCORE_TIMEOUT, env=env)
    except subprocess.TimeoutExpired as e:
        raise ConversionError("MuseScore non ha completato la conversione in tempo.") from e
    if not dst.exists() or dst.stat().st_size == 0:
        log.warning("mscore failed (%s):\n%s", proc.returncode, (proc.stdout + proc.stderr)[-2000:])
        raise ConversionError(f"MuseScore non è riuscito a leggere il file ({src.suffix}).")


def _abc_to_musicxml(src: Path, dst: Path) -> Path:
    import music21

    music21.converter.parse(str(src)).write("musicxml", fp=str(dst))
    return dst


def _sheet(src: Path, kind: Kind, work: Path, opt: Options, progress: Progress) -> tuple[Path, str, bool, list[str], dict]:
    """Returns (musicxml, engine, experimental, warnings, details)."""
    warnings: list[str] = []
    first_error = ""
    pages: list[Path]
    if kind is Kind.PDF:
        progress("pdf")
        pages = omr.pdf_to_images(src, work / "pages")
        if not pages:
            raise ConversionError("Il PDF non contiene pagine.")
        audiveris_in = [src]  # Audiveris reads PDFs itself, at their native resolution
    else:
        progress("photo")
        pages = [omr.prepare_photo(src, work / "page.png")]
        audiveris_in = pages

    if opt.sheet != "handwritten":
        progress("omr")
        try:
            mxls = omr.audiveris(audiveris_in, work / "omr")
            best = max(mxls, key=omr.mxl_note_count)
            notes = omr.mxl_note_count(best)
            if len(mxls) > 1:
                warnings.append(f"Trovati {len(mxls)} movimenti: convertito quello con più note.")
            # A handful of notes on a full page means Audiveris did not really read it.
            if notes >= 8 * len(pages) or opt.sheet == "printed":
                return best, "audiveris", False, warnings, {"pages": len(pages), "notes": notes}
            warnings.append("Audiveris ha riconosciuto poche note: provo il riconoscimento per manoscritti.")
        except omr.OmrFailed as e:
            if opt.sheet == "printed":
                raise ConversionError(str(e)) from e
            warnings.append(f"{e} Provo il riconoscimento per manoscritti.")
            first_error = str(e)

    progress("handwriting")
    try:
        xml = omr.claude_vision(pages, work / "vision.musicxml")
    except omr.OmrFailed as e:
        raise ConversionError(" ".join(x for x in (first_error, str(e)) if x)) from e
    warnings.append("Riconoscimento sperimentale (manoscritto o foto difficile): controlla tutte le note.")
    return xml, "claude-vision", True, warnings, {"pages": len(pages)}


def convert(src: Path, out_dir: Path, opt: Options | None = None, progress: Progress = lambda s: None) -> Result:
    opt = opt or Options()
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = opt.title or src.stem
    mscz = out_dir / f"{stem}.mscz"
    mid = out_dir / f"{stem}.mid"

    kind = detect(src)
    log.info("convert %s: %s", src.name, kind.value)
    with tempfile.TemporaryDirectory(prefix="scoreconvert-") as tmp:
        work = Path(tmp)
        # Keep the original extension: MuseScore picks the importer from it.
        suffix = src.suffix.lower()
        if kind is Kind.NOTATION and src.read_bytes()[:4] == b"MThd" and suffix not in (".mid", ".midi", ".kar"):
            suffix = ".mid"  # MIDI under another name (WhatsApp, e-mail attachments)
        local = work / f"input{suffix}"
        shutil.copyfile(src, local)

        experimental = False
        warnings: list[str] = []
        details: dict = {}

        if kind is Kind.NOTATION:
            engine, intermediate = "musescore", local
        elif kind is Kind.ABC:
            progress("abc")
            engine, intermediate = "music21", _abc_to_musicxml(local, work / "abc.musicxml")
        elif kind in (Kind.PDF, Kind.IMAGE):
            intermediate, engine, experimental, warnings, details = _sheet(local, kind, work, opt, progress)
        elif kind is Kind.AUDIO:
            progress("transcribe")
            try:
                rep = transcribe(local, work, work / "audio.mid", opt.audio)
            except ValueError as e:
                raise ConversionError(str(e)) from e
            except subprocess.CalledProcessError as e:
                raise ConversionError("File audio non leggibile.") from e
            engine, intermediate, experimental = "basic-pitch", work / "audio.mid", True
            warnings = ["Trascrizione automatica da audio: ritmo e voci vanno ricontrollati."] + rep.warnings
            details = {"tempo": rep.tempo, "notes": rep.notes, "seconds": rep.duration, "profile": rep.profile, "monophonic": rep.monophonic,
                       "noise_db": rep.noise_db}
        else:
            raise ConversionError("Tipo di file non riconosciuto.")

        progress("engrave")
        mscore(intermediate, mscz)
        progress("midi")
        try:
            mscore(mscz, mid)
        except ConversionError:
            mid = None
            warnings.append("Esportazione MIDI non riuscita.")

    return Result(kind=kind.value, engine=engine, mscz=mscz, mid=mid, experimental=experimental,
                  warnings=warnings, details=details)


def main() -> None:
    import argparse

    p = argparse.ArgumentParser(description="Converte PDF, foto, audio e altri formati in .mscz e .mid")
    p.add_argument("input", type=Path)
    p.add_argument("-o", "--out", type=Path, default=Path("."))
    p.add_argument("--sheet", choices=["auto", "printed", "handwritten"], default="auto")
    p.add_argument("--audio-profile", choices=["auto", "clean", "noisy"], default="auto")
    p.add_argument("--mono", action=argparse.BooleanOptionalAction, default=None, help="una sola linea melodica (default: automatico)")
    p.add_argument("--tempo", type=float)
    p.add_argument("--grid", type=int, default=4)
    a = p.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    opt = Options(sheet=a.sheet, audio=AudioOptions(profile=a.audio_profile, monophonic=a.mono, tempo=a.tempo, grid=a.grid))
    try:
        r = convert(a.input, a.out, opt, progress=lambda s: log.info("stage: %s", s))
    except ConversionError as e:
        raise SystemExit(f"Errore: {e}")
    print(json.dumps(r.to_json(), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
