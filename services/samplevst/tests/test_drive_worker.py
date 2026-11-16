"""The Drive worker against an in-memory Drive: which requests it takes, where results and requests end up."""

from __future__ import annotations

import io
import itertools
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
import soundfile as sf

from samplevst import drive_worker as w

FOLDER = w.FOLDER


class FakeDrive:
    def __init__(self):
        self.files: dict[str, dict] = {}
        self.ids = (f"id{i}" for i in itertools.count())
        self.root = self.add("Partiture", None, folder=True)

    def add(self, name, parent, data=b"", folder=False, age_min=60, mime="audio/wav"):
        fid = next(self.ids)
        t = (datetime.now(timezone.utc) - timedelta(minutes=age_min)).isoformat().replace("+00:00", "Z")
        self.files[fid] = {"id": fid, "name": name, "parents": [parent] if parent else [], "data": data,
                           "mimeType": FOLDER if folder else mime, "modifiedTime": t, "description": ""}
        return fid

    def list(self, folder):
        return [dict(f) for f in self.files.values() if folder in f["parents"]]

    def subfolder(self, parent, name, create=True):
        for f in self.list(parent):
            if f["mimeType"] == FOLDER and f["name"] == name:
                return f["id"]
        return self.add(name, parent, folder=True) if create else None

    def download(self, fid, dst: Path):
        dst.write_bytes(self.files[fid]["data"])

    def upload(self, parent, name, src: Path, mime, description=""):
        fid = self.add(name, parent, src.read_bytes(), mime=mime, age_min=0)
        self.files[fid]["description"] = description
        return {"id": fid}

    def update(self, fid, src: Path, description):
        self.files[fid]["data"] = src.read_bytes()
        self.files[fid]["description"] = description

    def put(self, parent, name, src, mime, description, existing):
        w.Drive.put(self, parent, name, src, mime, description, existing)

    def json(self, method, path, params=None, body=None):
        fid = path.split("/")[1]
        f = self.files[fid]
        if method == "PATCH":
            if params and "addParents" in params:
                f["parents"] = [params["addParents"]]
            f.update(body or {})
        return dict(f)


def wav(freq: float, dur: float = 1.5) -> bytes:
    t = np.arange(int(44100 * dur)) / 44100
    x = 0.3 * sum(np.sin(2 * np.pi * freq * k * t) / k for k in range(1, 8))
    buf = io.BytesIO()
    sf.write(buf, x, 44100, format="WAV")
    return buf.getvalue()


def names_in(d: FakeDrive, folder):
    return sorted(f["name"] for f in d.list(folder))


def test_requests_results_and_replacing(monkeypatch):
    monkeypatch.delenv("SAMPLEVST_TEMPLATE", raising=False)
    d = FakeDrive()
    inbox = d.subfolder(d.root, w.INBOX)
    # from an app: samples, then strumento.json
    vio = d.add("Violino", inbox, folder=True, age_min=0)
    d.add("violino_A4.wav", vio, wav(440), age_min=0)
    d.add("violino_A5.wav", vio, wav(880), age_min=0)
    d.add(w.MANIFEST, vio, json.dumps({"nome": "Violino solista", "rilascio": 0.5}).encode(), mime="application/json", age_min=0)
    # by hand, still being filled: waits
    d.add("Fiati", inbox, folder=True, age_min=0)
    d.add("flauto.wav", d.subfolder(inbox, "Fiati"), wav(523), age_min=1)
    # a loose file
    d.add("campana.wav", inbox, wav(660), age_min=0)

    assert sorted(j["name"] for j in w.jobs(d, inbox)) == ["Violino", "campana"]
    assert w.run(d, d.root) == 0
    out = d.subfolder(d.root, w.OUT)
    assert names_in(d, out) == ["Violino solista.sf2", "campana.sf2"]
    sf2 = next(f for f in d.list(out) if f["name"] == "Violino solista.sf2")
    assert "A4, A5" in sf2["description"] and sf2["data"][:4] == b"RIFF"
    done = d.subfolder(inbox, w.DONE)
    assert names_in(d, done) == ["Violino", "campana.wav"]
    assert names_in(d, inbox) == ["Errori", "Fatti", "Fiati"], "the folder being filled stays"

    # the same instrument again: the file is replaced, not duplicated
    vio2 = d.add("Violino", inbox, folder=True, age_min=0)
    d.add("violino_A4.wav", vio2, wav(440), age_min=0)
    d.add(w.MANIFEST, vio2, json.dumps({"nome": "Violino solista"}).encode(), mime="application/json", age_min=0)
    old_id = sf2["id"]
    w.run(d, d.root)
    assert names_in(d, out) == ["Violino solista.sf2", "campana.sf2"]
    assert "A4)" in d.files[old_id]["description"] and "A5" not in d.files[old_id]["description"]
    assert len([n for n in names_in(d, done) if n.startswith("Violino")]) == 2, "renamed, not lost"


def test_bad_request_goes_to_errors(monkeypatch):
    monkeypatch.delenv("SAMPLEVST_TEMPLATE", raising=False)
    d = FakeDrive()
    inbox = d.subfolder(d.root, w.INBOX)
    d.add("appunti.txt", inbox, b"not audio", mime="text/plain")
    assert w.run(d, d.root) == 1
    err = d.subfolder(inbox, w.FAILED)
    [f] = d.list(err)
    assert f["name"] == "appunti.txt" and f["description"].startswith("Strumento non creato")
