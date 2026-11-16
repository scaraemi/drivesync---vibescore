"""Makes the instruments people ask for in the Drive folder "Nuovo strumento", run by GitHub Actions on a schedule.

Layout, inside the scores folder the apps sync (DRIVE_FOLDER_ID):

    Partiture/
      Nuovo strumento/          <- the apps upload here, one folder per instrument:
        Violino/                   the samples, then strumento.json (name, options; written last = ready)
        tromba.wav              <- a loose file is an instrument of its own, named after it
        Fatti/                  <- requests done (the samples stay there)
        Errori/                 <- requests that failed; the reason is in the description
      Strumenti/                <- the results: <Name>.sf2 and <Name>.vst3.zip, a summary in their description.
                                   The apps install them from here. The same name again replaces the instrument.

A folder put there by hand (from the Drive web site), without strumento.json, is taken once nothing in it changed
for 10 minutes: its name is the instrument's name.

Standard library only up to the work itself, so `--check` runs on the bare runner.
Environment: GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, GOOGLE_REFRESH_TOKEN, DRIVE_FOLDER_ID, SAMPLEVST_TEMPLATE.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

# The Drive client of the score converter, standard library only
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scoreconvert"))
from scoreconvert.drive_worker import FOLDER, Drive as _Drive  # noqa: E402

INBOX, DONE, FAILED, OUT = "Nuovo strumento", "Fatti", "Errori", "Strumenti"
MANIFEST = "strumento.json"
SETTLE = timedelta(minutes=10)
MAX_JOBS_PER_RUN = int(os.environ.get("SAMPLEVST_MAX_JOBS", "5"))


class Drive(_Drive):
    def list(self, folder: str) -> list[dict]:
        q = f"'{folder}' in parents and trashed = false"
        out, page = [], None
        while True:
            p = {"q": q, "fields": "nextPageToken,files(id,name,mimeType,size,modifiedTime,description)",
                 "pageSize": "200", "includeItemsFromAllDrives": "true", "orderBy": "createdTime"}
            if page:
                p["pageToken"] = page
            res = self.json("GET", "files", p)
            out += res.get("files", [])
            page = res.get("nextPageToken")
            if not page:
                return out

    def update(self, file_id: str, src: Path, description: str) -> None:
        """New content for an existing file (same ID: the apps see it changed and install it again)."""
        self.json("PATCH", f"files/{file_id}", body={"description": description})
        url = f"https://www.googleapis.com/upload/drive/v3/files/{file_id}?uploadType=media&supportsAllDrives=true"
        with self._req("PATCH", url, src.read_bytes(), {"Content-Type": "application/octet-stream"}):
            pass

    def put(self, parent: str, name: str, src: Path, mime: str, description: str, existing: dict[str, str]) -> None:
        if name in existing:
            self.update(existing[name], src, description)
        else:
            existing[name] = self.upload(parent, name, src, mime, description)["id"]


def _time(f: dict) -> datetime:
    return datetime.fromisoformat(f["modifiedTime"].replace("Z", "+00:00"))


def jobs(drive: Drive, inbox: str) -> list[dict]:
    """Requests ready to be made: {"name", "folder" (id or None), "files": [...], "manifest": file or None}."""
    now = datetime.now(timezone.utc)
    out = []
    for f in drive.list(inbox):
        if f["mimeType"] == FOLDER:
            if f["name"] in (DONE, FAILED):
                continue
            files = [c for c in drive.list(f["id"]) if c["mimeType"] != FOLDER]
            manifest = next((c for c in files if c["name"] == MANIFEST), None)
            samples = [c for c in files if c["name"] != MANIFEST and not c["mimeType"].startswith("application/vnd.google-apps")]
            if not samples:
                continue
            if manifest is None and now - max(_time(c) for c in files) < SETTLE:
                continue   # still being filled by hand
            out.append({"name": f["name"], "folder": f["id"], "files": samples, "manifest": manifest})
        elif not f["mimeType"].startswith("application/vnd.google-apps"):
            out.append({"name": f["name"].rsplit(".", 1)[0], "folder": None, "files": [f], "manifest": None})
    return out


def run(drive: Drive, root: str) -> int:
    from .instrument import Options
    from .pipeline import SampleError, create, options_from_manifest

    inbox = drive.subfolder(root, INBOX)
    todo = jobs(drive, inbox)
    if not todo:
        print("Nothing to make.")
        return 0
    done = drive.subfolder(inbox, DONE)
    failed = drive.subfolder(inbox, FAILED)
    out = drive.subfolder(root, OUT)
    existing = {f["name"]: f["id"] for f in drive.list(out) if f["mimeType"] != FOLDER}
    template = Path(os.environ["SAMPLEVST_TEMPLATE"]) if os.environ.get("SAMPLEVST_TEMPLATE") else None
    if template is not None and not template.exists():
        print(f"Template {template} missing: SoundFont only.")
        template = None
    errors = 0

    for job in todo[:MAX_JOBS_PER_RUN]:
        print(f"== {job['name']} ({len(job['files'])} file(s))")
        target = job["folder"] or job["files"][0]["id"]
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "in"
            src.mkdir()
            try:
                data = {}
                if job["manifest"]:
                    drive.download(job["manifest"]["id"], Path(tmp) / MANIFEST)
                    try:
                        data = json.loads((Path(tmp) / MANIFEST).read_text(encoding="utf-8-sig"))
                    except ValueError:
                        data = {}
                files = []
                for i, f in enumerate(job["files"]):
                    p = src / f"{i:03d}" / Path(f["name"]).name   # same name twice in one folder is possible on Drive
                    p.parent.mkdir()
                    drive.download(f["id"], p)
                    files.append(p)
                opt = options_from_manifest(data, job["name"]) if data else Options(name=job["name"])
                r = create(files, opt, Path(tmp) / "out", template)
            except SampleError as e:
                print(f"   failed: {e}")
                _file_away(drive, target, inbox, failed, f"Strumento non creato: {e}")
                errors += 1
                continue
            except Exception as e:  # noqa: BLE001 - one bad request must not stop the others
                print(f"   error: {e!r}")
                _file_away(drive, target, inbox, failed, "Errore interno durante la creazione dello strumento.")
                errors += 1
                continue

            desc = r.description
            drive.put(out, r.sf2.name, r.sf2, "audio/x-soundfont", desc, existing)
            if r.vst3_zip:
                drive.put(out, r.vst3_zip.name, r.vst3_zip, "application/zip", desc, existing)
            _file_away(drive, target, inbox, done, desc)
            print("   -> " + r.sf2.name + (f", {r.vst3_zip.name}" if r.vst3_zip else "") + "\n   " + desc.replace("\n", "\n   "))

    left = len(todo) - MAX_JOBS_PER_RUN
    if left > 0:
        print(f"{left} more request(s) at the next run.")
    return 1 if errors and errors == min(len(todo), MAX_JOBS_PER_RUN) else 0


def _file_away(drive: Drive, item: str, inbox: str, dst: str, description: str) -> None:
    """Moves the request (folder or loose file) into Fatti/Errori, renamed if the name is taken there."""
    names = {f["name"] for f in drive.list(dst)}
    meta = drive.json("GET", f"files/{item}", {"fields": "name"})
    name = meta.get("name", "")
    body: dict = {"description": description[:4000]}
    if name in names:
        stem, dot, ext = name.rpartition(".") if "." in name else (name, "", "")
        stamp = datetime.now().strftime("%Y-%m-%d %H.%M")
        body["name"] = f"{stem} ({stamp}){dot}{ext}" if dot else f"{name} ({stamp})"
    drive.json("PATCH", f"files/{item}", {"addParents": dst, "removeParents": inbox}, body)


def pending(drive: Drive, root: str) -> int:
    inbox = drive.subfolder(root, INBOX, create=False)
    return 0 if not inbox else len(jobs(drive, inbox))


def main() -> None:
    missing = [v for v in ("GOOGLE_CLIENT_ID", "GOOGLE_CLIENT_SECRET", "GOOGLE_REFRESH_TOKEN", "DRIVE_FOLDER_ID")
               if not os.environ.get(v)]
    out = open(os.environ.get("GITHUB_OUTPUT", os.devnull), "a")
    if missing:
        print(f"Not configured (missing {', '.join(missing)}): nothing to do.")
        if "--check" in sys.argv:
            print("pending=0", file=out)
        return
    drive = Drive()
    root = os.environ["DRIVE_FOLDER_ID"]
    if "--check" in sys.argv:
        n = pending(drive, root)
        print(f"{n} request(s) waiting in '{INBOX}'.")
        print(f"pending={n}", file=out)
        return
    sys.exit(run(drive, root))


if __name__ == "__main__":
    main()
