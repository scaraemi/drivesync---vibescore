"""HTTP service used by MuseScore and VibeScore ("Importa da PDF / foto / audio").

    POST /v1/jobs              multipart: file, [sheet], [audio_profile], [mono], [tempo]  -> 202 {id}
    GET  /v1/jobs/{id}         -> {state: queued|running|done|failed, stage, result|error}
    GET  /v1/jobs/{id}/score   -> the .mscz
    GET  /v1/jobs/{id}/midi    -> the .mid
    GET  /healthz

Clients authenticate with "Authorization: Bearer <token>"; tokens are listed in SCORECONVERT_TOKENS
(comma separated). Jobs run one or two at a time (OMR and transcription are CPU-heavy) and are
deleted after SCORECONVERT_TTL seconds.
"""

from __future__ import annotations

import asyncio
import hmac
import logging
import os
import shutil
import tempfile
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

from fastapi import Depends, FastAPI, File, Form, Header, HTTPException, UploadFile
from fastapi.responses import FileResponse

from .audio import AudioOptions
from .pipeline import ConversionError, Options, convert

log = logging.getLogger("scoreconvert")

DATA = Path(os.environ.get("SCORECONVERT_DATA", tempfile.gettempdir())) / "scoreconvert-jobs"
TOKENS = [t.strip() for t in os.environ.get("SCORECONVERT_TOKENS", "").split(",") if t.strip()]
WORKERS = int(os.environ.get("SCORECONVERT_WORKERS", "1"))
TTL = int(os.environ.get("SCORECONVERT_TTL", "86400"))
MAX_BYTES = int(os.environ.get("SCORECONVERT_MAX_MB", "200")) * 1024 * 1024


@dataclass
class Job:
    id: str
    name: str
    created: float = field(default_factory=time.time)
    state: str = "queued"
    stage: str = ""
    error: str | None = None
    result: dict | None = None

    @property
    def dir(self) -> Path:
        return DATA / self.id


app = FastAPI(title="scoreconvert")
jobs: dict[str, Job] = {}
pool = ThreadPoolExecutor(max_workers=WORKERS)


def auth(authorization: str = Header(default="")) -> None:
    if not TOKENS:
        return  # development: no tokens configured
    token = authorization.removeprefix("Bearer ").strip()
    if not any(hmac.compare_digest(token, t) for t in TOKENS):
        raise HTTPException(401, "Token non valido")


def _clean_name(name: str) -> str:
    base = Path(name or "file").name
    keep = "".join(c for c in base if c.isalnum() or c in " ._-()'àèéìòùÀÈÉÌÒÙ").strip(" .")
    return keep[:120] or "file"


def _run(job: Job, src: Path, opt: Options) -> None:
    job.state = "running"

    def progress(stage: str) -> None:
        job.stage = stage

    try:
        r = convert(src, job.dir / "out", opt, progress)
        job.result = r.to_json()
        job.state = "done"
    except ConversionError as e:
        job.error, job.state = str(e), "failed"
    except Exception:  # noqa: BLE001 - report, keep the worker alive
        log.exception("job %s", job.id)
        job.error, job.state = "Errore interno durante la conversione.", "failed"
    finally:
        src.unlink(missing_ok=True)


@app.on_event("startup")
async def _startup() -> None:
    DATA.mkdir(parents=True, exist_ok=True)

    async def sweep() -> None:
        while True:
            now = time.time()
            for jid, job in list(jobs.items()):
                if now - job.created > TTL and job.state in ("done", "failed"):
                    shutil.rmtree(job.dir, ignore_errors=True)
                    jobs.pop(jid, None)
            await asyncio.sleep(600)

    asyncio.create_task(sweep())


@app.get("/healthz")
def healthz() -> dict:
    return {"ok": True, "queued": sum(j.state == "queued" for j in jobs.values())}


@app.post("/v1/jobs", status_code=202, dependencies=[Depends(auth)])
async def create_job(
    file: UploadFile = File(...),
    sheet: str = Form("auto"),
    audio_profile: str = Form("auto"),
    mono: str = Form("auto"),
    tempo: float | None = Form(None),
) -> dict:
    if sheet not in ("auto", "printed", "handwritten") or audio_profile not in ("auto", "clean", "noisy"):
        raise HTTPException(400, "Opzione non valida")
    name = _clean_name(file.filename or "")
    job = Job(id=uuid.uuid4().hex, name=name)
    job.dir.mkdir(parents=True)
    src = job.dir / name
    size = 0
    with src.open("wb") as f:
        while chunk := await file.read(1 << 20):
            size += len(chunk)
            if size > MAX_BYTES:
                shutil.rmtree(job.dir, ignore_errors=True)
                raise HTTPException(413, "File troppo grande")
            f.write(chunk)

    opt = Options(
        sheet=sheet,
        title=Path(name).stem,
        audio=AudioOptions(profile=audio_profile, monophonic=None if mono == "auto" else mono in ("1", "true", "yes"),
                           tempo=tempo or None),
    )
    jobs[job.id] = job
    pool.submit(_run, job, src, opt)
    return {"id": job.id}


def _job(job_id: str) -> Job:
    job = jobs.get(job_id)
    if not job:
        raise HTTPException(404, "Conversione non trovata")
    return job


@app.get("/v1/jobs/{job_id}", dependencies=[Depends(auth)])
def get_job(job_id: str) -> dict:
    j = _job(job_id)
    return {"id": j.id, "name": j.name, "state": j.state, "stage": j.stage, "error": j.error, "result": j.result}


def _file(job_id: str, key: str, media: str) -> FileResponse:
    j = _job(job_id)
    if j.state != "done" or not j.result or not j.result.get(key):
        raise HTTPException(409, "Risultato non disponibile")
    path = j.dir / "out" / j.result[key]
    return FileResponse(path, media_type=media, filename=path.name)


@app.get("/v1/jobs/{job_id}/score", dependencies=[Depends(auth)])
def get_score(job_id: str) -> FileResponse:
    return _file(job_id, "mscz", "application/x-musescore")


@app.get("/v1/jobs/{job_id}/midi", dependencies=[Depends(auth)])
def get_midi(job_id: str) -> FileResponse:
    return _file(job_id, "mid", "audio/midi")
