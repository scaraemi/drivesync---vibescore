"""Samples -> instrument: a SoundFont (.sf2) and a VST3 plugin (.vst3, zipped), playable over MIDI.

    python -m samplevst.pipeline violino_A4.wav violino_A5.wav -n "Violino" -o out/ [--template SampleVST.vst3]
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, field
from pathlib import Path

from . import sf2, vst3
from .analyze import Sound, analyse_file
from .audio import AudioError, load
from .instrument import Instrument, Options, build, parse_note


class SampleError(Exception):
    pass


@dataclass
class Result:
    instrument: Instrument
    sf2: Path
    vst3_zip: Path | None
    warnings: list[str] = field(default_factory=list)

    @property
    def description(self) -> str:
        return "\n".join(self.instrument.summary() + self.warnings)


def options_from_manifest(data: dict, default_name: str) -> Options:
    """strumento.json, written by the apps (all keys optional):
    {"nome": "Violino", "tipo": "auto|melodico|batteria", "nota": "C4", "loop": "auto|si|no",
     "attacco": 0.01, "rilascio": 0.3, "dividi": true}"""
    kinds = {"melodico": "melodic", "melodic": "melodic", "batteria": "kit", "kit": "kit", "effetti": "kit"}
    loops = {"si": "on", "sì": "on", "on": "on", True: "on", "no": "off", "off": "off", False: "off"}
    o = Options(name=str(data.get("nome") or data.get("name") or default_name).strip() or default_name)
    o.kind = kinds.get(str(data.get("tipo", "auto")).lower(), "auto")
    if data.get("nota"):
        o.root = parse_note(str(data["nota"]))
    o.loop = loops.get(data.get("loop", "auto"), "auto")
    for key, attr in (("attacco", "attack"), ("rilascio", "release")):
        if isinstance(data.get(key), (int, float)):
            setattr(o, attr, float(data[key]))
    if data.get("dividi") is False:
        o.split = False
    return o


def create(files: list[Path], opt: Options, out_dir: Path, template: Path | None = None) -> Result:
    out_dir.mkdir(parents=True, exist_ok=True)
    sounds: list[Sound] = []
    warnings: list[str] = []
    for f in sorted(files, key=lambda p: p.name.lower()):
        try:
            data, rate = load(f)
        except AudioError as e:
            warnings.append(f"'{f.name}' ignorato: {e}.")
            continue
        found = analyse_file(data, rate, f.name, split=opt.split)
        if len(found) > 1:
            warnings.append(f"'{f.name}' contiene {len(found)} suoni: usati separatamente.")
        for s in found:
            warnings += [f"'{f.name}': {n}" for n in s.notes]
        sounds += found
    if not sounds:
        raise SampleError("nessun file audio utilizzabile" + (": " + " ".join(warnings) if warnings else ""))
    if len(sounds) > 128:
        raise SampleError(f"troppi suoni ({len(sounds)}): al massimo 128 per strumento")
    try:
        inst = build(sounds, opt)
    except ValueError as e:
        raise SampleError(str(e)) from e

    name = vst3.safe_name(inst.name)
    sf2_path = out_dir / f"{name}.sf2"
    sf2.write(inst, sf2_path)
    zip_path = None
    if template is not None:
        bundle = vst3.build_bundle(inst, template, out_dir / "bundle")
        zip_path = vst3.zip_bundle(bundle, out_dir / f"{name}.vst3.zip")
    else:
        warnings.append("VST3 non creato: manca il modello del plugin (SampleVST.vst3).")
    return Result(inst, sf2_path, zip_path, warnings)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("-n", "--name", default=None)
    ap.add_argument("-o", "--out", type=Path, default=Path("out"))
    ap.add_argument("--template", type=Path, default=None, help="SampleVST.vst3 bundle")
    ap.add_argument("--manifest", type=Path, default=None, help="strumento.json")
    ap.add_argument("--kind", choices=["auto", "melodic", "kit"], default=None)
    ap.add_argument("--root", default=None, help="note of a single sample, e.g. C4")
    a = ap.parse_args()
    data = json.loads(a.manifest.read_text(encoding="utf-8")) if a.manifest else {}
    opt = options_from_manifest(data, a.name or a.files[0].stem)
    if a.name:
        opt.name = a.name
    if a.kind:
        opt.kind = a.kind
    if a.root:
        opt.root = parse_note(a.root)
    r = create(a.files, opt, a.out, a.template)
    print(r.description)
    print(f"-> {r.sf2}" + (f"\n-> {r.vst3_zip}" if r.vst3_zip else ""))


if __name__ == "__main__":
    main()
