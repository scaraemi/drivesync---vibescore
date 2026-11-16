"""The VST3 instrument: a copy of the generic sampler plugin (plugin/ in this folder) with the samples inside.

The plugin binary is the same for every instrument. It reads its name, its class ID and its samples from its own
bundle, Contents/Resources/instrument.txt: so making an instrument needs no compiler, only the prebuilt bundle
(built by CI for Windows, macOS and Linux and published as a release, see .github/workflows/samplevst.yml).

instrument.txt, one record per line, fields separated by one space:

    samplevst 1
    name <rest of the line>
    uid <32 hex digits>
    kind melodic|kit
    envelope <attack s> <decay s> <sustain 0..1> <release s>
    sample <id> <file under Resources> <loop start frame> <loop end frame>      (-1 -1: no loop)
    zone <sample id> <lokey> <hikey> <lovel> <hivel> <root key> <tune cents> <loop|release|oneshot> <group>
"""

from __future__ import annotations

import plistlib
import re
import shutil
import zipfile
from pathlib import Path

from .audio import save_wav
from .instrument import Instrument

BINARY = "SampleVST"   # the template bundle's name


def safe_name(name: str) -> str:
    """A name every file system accepts (Windows is the strictest)."""
    s = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "-", name).strip().rstrip(".")
    return s[:60] or "Strumento"


def write_resources(inst: Instrument, resources: Path) -> None:
    (resources / "samples").mkdir(parents=True, exist_ok=True)
    lines = ["samplevst 1", f"name {inst.name.replace(chr(10), ' ')}", f"uid {inst.uid}", f"kind {inst.kind}",
             f"envelope {inst.attack:.4f} {inst.decay:.4f} {inst.sustain:.4f} {inst.release:.4f}"]
    for i, s in enumerate(inst.sounds):
        rel = f"samples/s{i + 1:03d}.wav"
        save_wav(resources / rel, s.data, s.rate)
        ls, le = s.loop if s.loop else (-1, -1)
        lines.append(f"sample {i} {rel} {ls} {le}")
    for z in inst.zones:
        lines.append(f"zone {z.sound} {z.lokey} {z.hikey} {z.lovel} {z.hivel} {z.root} {z.tune} {z.mode} {z.group}")
    (resources / "instrument.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_bundle(inst: Instrument, template: Path, out_dir: Path) -> Path:
    """template = SampleVST.vst3 (the folder). Returns <Name>.vst3 in out_dir."""
    name = safe_name(inst.name)
    dst = out_dir / f"{name}.vst3"
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(template, dst, symlinks=True)
    contents = dst / "Contents"
    # The module file is named like the bundle: hosts look for Contents/<arch>/<Name>.vst3 (.so on Linux)
    for arch in contents.iterdir():
        if not arch.is_dir() or arch.name in ("Resources", "_CodeSignature"):
            continue
        for f in arch.iterdir():
            if f.stem == BINARY:
                f.rename(f.with_name(name + f.suffix))
    # macOS: the binary carries its own (linker) signature; the bundle seal would cover Resources, which change
    shutil.rmtree(contents / "_CodeSignature", ignore_errors=True)
    plist = contents / "Info.plist"
    if plist.exists():
        info = plistlib.loads(plist.read_bytes())
        info["CFBundleExecutable"] = name
        info["CFBundleName"] = inst.name
        info["CFBundleIdentifier"] = f"work.appgate.samplevst.{inst.uid.lower()}"
        plist.write_bytes(plistlib.dumps(info))
    resources = contents / "Resources"
    for old in ("instrument.txt", "samples", "moduleinfo.json"):   # a moduleinfo.json would hold the template's ID
        p = resources / old
        if p.is_dir():
            shutil.rmtree(p)
        elif p.exists():
            p.unlink()
    write_resources(inst, resources)
    return dst


def zip_bundle(bundle: Path, dst: Path) -> Path:
    """A bundle is a folder: it travels through Drive as <Name>.vst3.zip, the apps unpack it."""
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(bundle.rglob("*")):
            if p.is_file():
                info = zipfile.ZipInfo.from_file(p, Path(bundle.name) / p.relative_to(bundle))
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, p.read_bytes())
    return dst
