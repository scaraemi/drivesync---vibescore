"""Converts the files people drop in the Drive folder "Da convertire", run by GitHub Actions on a schedule.

Layout, inside the scores folder the apps sync (DRIVE_FOLDER_ID):

    Partiture/                  <- the .mscz results land here, so the apps receive them with the next sync
      Da convertire/            <- the apps upload PDFs, photos, recordings here
        Convertiti/             <- originals after conversion, with the .mid
        Errori/                 <- originals that could not be converted; the reason is in the file description

Warnings about the result (experimental, noisy recording...) go into the .mscz file's Drive description.

Standard library only, so `--check` runs on the bare runner before the conversion image is pulled.
Environment: GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, GOOGLE_REFRESH_TOKEN, DRIVE_FOLDER_ID.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

API = "https://www.googleapis.com/drive/v3"
UPLOAD = "https://www.googleapis.com/upload/drive/v3/files"
FOLDER = "application/vnd.google-apps.folder"
INBOX, DONE, FAILED = "Da convertire", "Convertiti", "Errori"
MAX_FILES_PER_RUN = int(os.environ.get("SCORECONVERT_MAX_FILES", "10"))


class Drive:
    def __init__(self) -> None:
        body = urllib.parse.urlencode({
            "client_id": os.environ["GOOGLE_CLIENT_ID"],
            "client_secret": os.environ["GOOGLE_CLIENT_SECRET"],
            "refresh_token": os.environ["GOOGLE_REFRESH_TOKEN"],
            "grant_type": "refresh_token",
        }).encode()
        try:
            with urllib.request.urlopen("https://oauth2.googleapis.com/token", body, timeout=30) as r:
                self.token = json.load(r)["access_token"]
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")
            raise SystemExit(f"Google sign-in failed ({e.code}): {detail}\n"
                             "If it says invalid_grant, the refresh token expired or was revoked: create a new one.")

    def _req(self, method: str, url: str, data: bytes | None = None, headers: dict | None = None):
        h = {"Authorization": f"Bearer {self.token}", **(headers or {})}
        req = urllib.request.Request(url, data=data, method=method, headers=h)
        try:
            return urllib.request.urlopen(req, timeout=300)
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"Drive {method} {url.split('?')[0]}: {e.code} {e.read().decode(errors='replace')[:300]}")

    def json(self, method: str, path: str, params: dict | None = None, body: dict | None = None) -> dict:
        q = urllib.parse.urlencode({"supportsAllDrives": "true", **(params or {})})
        data = json.dumps(body).encode() if body is not None else None
        with self._req(method, f"{API}/{path}?{q}", data, {"Content-Type": "application/json"} if data else None) as r:
            raw = r.read()
            return json.loads(raw) if raw else {}

    def children(self, folder: str, folders: bool | None = None) -> list[dict]:
        q = f"'{folder}' in parents and trashed = false"
        if folders is True:
            q += f" and mimeType = '{FOLDER}'"
        elif folders is False:
            q += f" and mimeType != '{FOLDER}' and not mimeType contains 'application/vnd.google-apps'"
        out, page = [], None
        while True:
            p = {"q": q, "fields": "nextPageToken,files(id,name,mimeType,size,description)", "pageSize": "200",
                 "includeItemsFromAllDrives": "true", "orderBy": "createdTime"}
            if page:
                p["pageToken"] = page
            res = self.json("GET", "files", p)
            out += res.get("files", [])
            page = res.get("nextPageToken")
            if not page:
                return out

    def subfolder(self, parent: str, name: str, create: bool = True) -> str | None:
        for f in self.children(parent, folders=True):
            if f["name"] == name:
                return f["id"]
        if not create:
            return None
        return self.json("POST", "files", body={"name": name, "mimeType": FOLDER, "parents": [parent]})["id"]

    def download(self, file_id: str, dst: Path) -> None:
        with self._req("GET", f"{API}/files/{file_id}?alt=media&supportsAllDrives=true") as r, dst.open("wb") as f:
            while chunk := r.read(1 << 20):
                f.write(chunk)

    def upload(self, parent: str, name: str, src: Path, mime: str, description: str = "") -> dict:
        boundary = uuid.uuid4().hex
        meta = {"name": name, "parents": [parent], "mimeType": mime}
        if description:
            meta["description"] = description
        body = (f"--{boundary}\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{json.dumps(meta)}\r\n"
                f"--{boundary}\r\nContent-Type: {mime}\r\n\r\n").encode() + src.read_bytes() + f"\r\n--{boundary}--".encode()
        with self._req("POST", f"{UPLOAD}?uploadType=multipart&supportsAllDrives=true&fields=id,name", body,
                       {"Content-Type": f"multipart/related; boundary={boundary}"}) as r:
            return json.load(r)

    def move(self, file_id: str, src_parent: str, dst_parent: str, description: str | None = None) -> None:
        body = {"description": description} if description is not None else {}
        self.json("PATCH", f"files/{file_id}", {"addParents": dst_parent, "removeParents": src_parent}, body)


def free_name(names: set[str], wanted: str) -> str:
    """'Brano.mscz' -> 'Brano (2).mscz' when the name is taken: never overwrite a score someone made."""
    if wanted not in names:
        return wanted
    stem, ext = os.path.splitext(wanted)
    n = 2
    while f"{stem} ({n}){ext}" in names:
        n += 1
    return f"{stem} ({n}){ext}"


def pending(drive: Drive, root: str) -> list[dict]:
    inbox = drive.subfolder(root, INBOX, create=False)
    return [] if not inbox else drive.children(inbox, folders=False)


def run(drive: Drive, root: str) -> int:
    from .pipeline import ConversionError, Options, convert  # heavy: only inside the image

    inbox = drive.subfolder(root, INBOX)
    files = drive.children(inbox, folders=False)
    if not files:
        print("Nothing to convert.")
        return 0
    done = drive.subfolder(inbox, DONE)
    failed = drive.subfolder(inbox, FAILED)
    taken = {f["name"] for f in drive.children(root, folders=False)}
    errors = 0

    for f in files[:MAX_FILES_PER_RUN]:
        name = f["name"]
        print(f"== {name}")
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / Path(name).name
            try:
                drive.download(f["id"], src)
                r = convert(src, Path(tmp) / "out", Options(title=Path(name).stem))
            except ConversionError as e:
                print(f"   failed: {e}")
                drive.move(f["id"], inbox, failed, f"Conversione non riuscita: {e}")
                errors += 1
                continue
            except Exception as e:  # noqa: BLE001 - one bad file must not stop the others
                print(f"   error: {e!r}")
                drive.move(f["id"], inbox, failed, "Errore interno durante la conversione.")
                errors += 1
                continue

            notes = []
            if r.experimental:
                notes.append("Risultato sperimentale: da ricontrollare.")
            notes += r.warnings
            notes.append(f"Convertito da '{name}' con {r.engine}.")
            score_name = free_name(taken, r.mscz.name)
            taken.add(score_name)
            drive.upload(root, score_name, r.mscz, "application/x-musescore", "\n".join(notes))
            if r.mid:
                drive.upload(done, f"{Path(name).stem}.mid", r.mid, "audio/midi")
            drive.move(f["id"], inbox, done, "\n".join(notes))
            print(f"   -> {score_name} ({r.engine}{', experimental' if r.experimental else ''}) {r.details}")

    left = len(files) - MAX_FILES_PER_RUN
    if left > 0:
        print(f"{left} more file(s) at the next run.")
    return 1 if errors and errors == min(len(files), MAX_FILES_PER_RUN) else 0


def main() -> None:
    missing = [v for v in ("GOOGLE_CLIENT_ID", "GOOGLE_CLIENT_SECRET", "GOOGLE_REFRESH_TOKEN", "DRIVE_FOLDER_ID")
               if not os.environ.get(v)]
    if missing:
        print(f"Not configured (missing {', '.join(missing)}): nothing to do.")
        if "--check" in sys.argv:
            print("pending=0", file=open(os.environ.get("GITHUB_OUTPUT", os.devnull), "a"))
        return
    drive = Drive()
    root = os.environ["DRIVE_FOLDER_ID"]
    if "--check" in sys.argv:
        n = len(pending(drive, root))
        print(f"{n} file(s) waiting in '{INBOX}'.")
        with open(os.environ.get("GITHUB_OUTPUT", os.devnull), "a") as out:
            print(f"pending={n}", file=out)
        return
    import logging

    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    sys.exit(run(drive, root))


if __name__ == "__main__":
    main()
