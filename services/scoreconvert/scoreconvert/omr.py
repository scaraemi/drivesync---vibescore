"""Sheet music images and PDFs -> MusicXML.

Printed music goes to Audiveris. Audiveris gives up on handwriting and on many phone photos; then, if an
Anthropic API key is configured, the pages go to Claude with vision, which writes MusicXML directly. That
fallback is labelled experimental in the result: it reads simple handwritten lines reasonably, dense piano or
choir pages poorly.
"""

from __future__ import annotations

import base64
import logging
import os
import re
import shutil
import subprocess
import zipfile
from pathlib import Path

log = logging.getLogger(__name__)

AUDIVERIS = os.environ.get("AUDIVERIS", "audiveris")
AUDIVERIS_TIMEOUT = int(os.environ.get("AUDIVERIS_TIMEOUT", "900"))


class OmrFailed(Exception):
    pass


def pdf_to_images(pdf: Path, out_dir: Path, dpi: int = 300) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(["pdftoppm", "-r", str(dpi), "-gray", "-png", str(pdf), str(out_dir / "p")], check=True)
    return sorted(out_dir.glob("p*.png"))


def prepare_photo(src: Path, dst: Path) -> Path:
    """Phone photo -> flat grey page. Uneven light is the main reason Audiveris misses staves in photos."""
    import cv2
    import numpy as np

    img = cv2.imread(str(src), cv2.IMREAD_COLOR)
    if img is None:  # HEIC and other formats OpenCV cannot read
        subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", str(src), str(dst)], check=True)
        img = cv2.imread(str(dst), cv2.IMREAD_COLOR)
        if img is None:
            raise OmrFailed("Immagine non leggibile.")
    grey = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

    # Audiveris wants staff lines about 20 px apart; a phone photo of an A4 page is often too small or too large.
    h, w = grey.shape
    scale = 2480 / max(w, 1)  # A4 width at 300 dpi
    if not 0.8 < scale < 1.25:
        grey = cv2.resize(grey, None, fx=scale, fy=scale,
                          interpolation=cv2.INTER_CUBIC if scale > 1 else cv2.INTER_AREA)

    # Divide by a blurred copy to remove shadows and light gradients, then stretch the contrast.
    bg = cv2.medianBlur(cv2.dilate(grey, np.ones((7, 7), np.uint8)), 41)
    flat = cv2.divide(grey, bg, scale=255)
    flat = cv2.normalize(flat, None, 0, 255, cv2.NORM_MINMAX)
    cv2.imwrite(str(dst), flat)
    return dst


def audiveris(inputs: list[Path], out_dir: Path) -> list[Path]:
    """Run Audiveris in batch mode. Returns the .mxl files, one per movement found."""
    if not shutil.which(AUDIVERIS) and not Path(AUDIVERIS).exists():
        raise OmrFailed("Audiveris non è installato sul server.")
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [AUDIVERIS, "-batch", "-export", "-output", str(out_dir), "--", *map(str, inputs)]
    log.info("audiveris: %s", " ".join(cmd))
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=AUDIVERIS_TIMEOUT)
    except subprocess.TimeoutExpired as e:
        raise OmrFailed("Audiveris ha impiegato troppo tempo.") from e
    results = sorted(out_dir.rglob("*.mxl"))
    if not results:
        tail = (proc.stdout + proc.stderr)[-1500:]
        log.warning("audiveris produced nothing:\n%s", tail)
        raise OmrFailed("Audiveris non ha riconosciuto pentagrammi.")
    return results


def mxl_note_count(mxl: Path) -> int:
    with zipfile.ZipFile(mxl) as z:
        name = next((n for n in z.namelist() if n.endswith((".xml", ".musicxml")) and not n.startswith("META-INF")), None)
        if not name:
            return 0
        return z.read(name).count(b"<note")


# --- Claude vision fallback -------------------------------------------------------------------------

CLAUDE_MODEL = os.environ.get("SCORECONVERT_CLAUDE_MODEL", "claude-sonnet-5-5")

PROMPT = """You are an expert music engraver. Transcribe the sheet music in the attached page images \
(they are consecutive pages of one piece, possibly handwritten) into a single MusicXML 4.0 document \
(score-partwise).

Rules:
- One <part> per staff system line-up as written (e.g. Soprano, Alto, Tenor, Bass, or Piano with two staves).
- Keep clefs, key and time signatures, every note's pitch, duration, accidentals, ties, rests, tuplets, \
dynamics, tempo text and lyrics if present.
- Use <divisions>4</divisions> unless the music needs finer values; every measure must add up to the time signature.
- If a symbol is illegible, choose the most musically plausible reading.
- Output ONLY the MusicXML, starting with <?xml, no commentary, no code fences."""


def claude_vision(images: list[Path], out_xml: Path) -> Path:
    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        raise OmrFailed("Riconoscimento con Claude non configurato (ANTHROPIC_API_KEY).")
    import anthropic

    import cv2

    content: list[dict] = []
    for img in images[:20]:
        pix = cv2.imread(str(img), cv2.IMREAD_GRAYSCALE)
        if pix is None:
            continue
        s = 2000 / max(pix.shape)  # the API rescales anything larger; send less
        if s < 1:
            pix = cv2.resize(pix, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)
        ok, jpg = cv2.imencode(".jpg", pix, [cv2.IMWRITE_JPEG_QUALITY, 88])
        content.append({"type": "image", "source": {"type": "base64", "media_type": "image/jpeg",
                                                     "data": base64.b64encode(jpg.tobytes()).decode()}})
    if not content:
        raise OmrFailed("Immagini non leggibili.")
    content.append({"type": "text", "text": PROMPT})

    client = anthropic.Anthropic(api_key=key)
    text = ""
    with client.messages.stream(model=CLAUDE_MODEL, max_tokens=64000,
                                messages=[{"role": "user", "content": content}]) as stream:
        for chunk in stream.text_stream:
            text += chunk
    m = re.search(r"<\?xml.*</score-partwise>", text, re.S)
    if not m:
        raise OmrFailed("Claude non ha restituito una partitura.")
    out_xml.write_text(m.group(0), encoding="utf-8")
    return out_xml
