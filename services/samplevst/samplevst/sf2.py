"""SoundFont 2.01 writer: what MuseScore (Windows, macOS) and VibeScore (Android) load as an instrument.

Spec: SoundFont Technical Specification 2.04, E-mu/Creative. One preset (bank 0, program 0) with one instrument;
a global zone holds the envelope, then one zone per sample (two for a stereo sample, panned left and right).
"""

from __future__ import annotations

import math
import struct
from pathlib import Path

import numpy as np

from .instrument import Instrument

GEN_PAN, GEN_ATTACK, GEN_DECAY, GEN_SUSTAIN, GEN_RELEASE = 17, 34, 36, 37, 38
GEN_INSTRUMENT, GEN_KEYRANGE, GEN_VELRANGE = 41, 43, 44
GEN_FINETUNE, GEN_SAMPLEID, GEN_SAMPLEMODES, GEN_EXCLUSIVE, GEN_ROOTKEY = 52, 53, 54, 57, 58
MONO, RIGHT, LEFT = 1, 2, 4


def _chunk(tag: bytes, data: bytes) -> bytes:
    if len(data) % 2:
        data += b"\0"
    return tag + struct.pack("<I", len(data)) + data


def _list(tag: bytes, *chunks: bytes) -> bytes:
    return _chunk(b"LIST", tag + b"".join(chunks))


def _zstr(s: str) -> bytes:
    b = s.encode("ascii", "replace")[:255] + b"\0"
    return b + (b"\0" if len(b) % 2 else b"")


def _name20(s: str) -> bytes:
    return s.encode("ascii", "replace")[:19].ljust(20, b"\0")


def timecents(seconds: float) -> int:
    return int(max(-12000, min(8000, round(1200 * math.log2(max(seconds, 0.001))))))


def _gen(op: int, amount: int) -> bytes:
    return struct.pack("<Hh", op, amount)


def _range(op: int, lo: int, hi: int) -> bytes:
    return struct.pack("<HBB", op, lo, hi)


def write(inst: Instrument, path: Path) -> None:
    # ---- samples
    pcm = []
    pos = 0
    headers = []   # (name, start, end, loopstart, loopend, rate, root, link, type)
    sample_ids: list[list[int]] = []   # per sound: [mono] or [left, right]
    root_of = {}
    for z in inst.zones:
        root_of.setdefault(z.sound, z.root)
    for i, s in enumerate(inst.sounds):
        chans = [s.data[:, 0]] if s.data.shape[1] == 1 else [s.data[:, 0], s.data[:, 1]]
        ids = []
        for c, x in enumerate(chans):
            q = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
            start, end = pos, pos + len(q)
            if s.loop:
                ls, le = start + s.loop[0], start + s.loop[1]
            else:
                ls, le = start, end - 1
            label = f"s{i + 1:03d}" + ("" if len(chans) == 1 else "LR"[c])
            ids.append(len(headers))
            headers.append([label, start, end, ls, le, s.rate, min(127, root_of.get(i, 60)), 0,
                            MONO if len(chans) == 1 else (LEFT if c == 0 else RIGHT)])
            pcm.append(q.tobytes() + b"\0\0" * 46)   # 46 zero points after each sample, as the spec asks
            pos = end + 46
        if len(ids) == 2:
            headers[ids[0]][7], headers[ids[1]][7] = ids[1], ids[0]
        sample_ids.append(ids)

    shdr = b"".join(struct.pack("<20sIIIIIBbHH", _name20(h[0]), *h[1:6], h[6], 0, h[7], h[8]) for h in headers)
    shdr += struct.pack("<20sIIIIIBbHH", _name20("EOS"), 0, 0, 0, 0, 0, 0, 0, 0, 0)

    # ---- instrument zones
    bags: list[bytes] = []   # each bag: its generators, already packed
    glob = [_gen(GEN_ATTACK, timecents(inst.attack)), _gen(GEN_RELEASE, timecents(inst.release))]
    if inst.decay > 0:
        glob.append(_gen(GEN_DECAY, timecents(inst.decay)))
        glob.append(_gen(GEN_SUSTAIN, int(round(-200 * math.log10(max(inst.sustain, 1e-5))))))
    bags.append(b"".join(glob))
    for z in inst.zones:
        s = inst.sounds[z.sound]
        ids = sample_ids[z.sound]
        for c, sid in enumerate(ids):
            g = [_range(GEN_KEYRANGE, z.lokey, z.hikey)]
            if (z.lovel, z.hivel) != (1, 127):
                g.append(_range(GEN_VELRANGE, z.lovel, z.hivel))
            if len(ids) == 2:
                g.append(_gen(GEN_PAN, -500 if c == 0 else 500))
            g.append(_gen(GEN_ROOTKEY, z.root))
            if z.tune:
                g.append(_gen(GEN_FINETUNE, max(-99, min(99, z.tune))))
            if z.mode == "loop":
                g.append(_gen(GEN_SAMPLEMODES, 1))
            if z.mode == "oneshot":
                # no one-shot in SoundFont: a release as long as the sound lets it ring to the end
                g.append(_gen(GEN_RELEASE, timecents(min(100.0, s.duration))))
            if z.group:
                g.append(_gen(GEN_EXCLUSIVE, z.group))
            g.append(_gen(GEN_SAMPLEID, sid))
            bags.append(b"".join(g))

    igen = b""
    ibag = b""
    for b in bags:
        ibag += struct.pack("<HH", len(igen) // 4, 0)
        igen += b
    ibag += struct.pack("<HH", len(igen) // 4, 0)
    igen += _gen(0, 0)
    inst_rec = struct.pack("<20sH", _name20(inst.name), 0) + struct.pack("<20sH", _name20("EOI"), len(bags))

    phdr = struct.pack("<20sHHHIII", _name20(inst.name), 0, 0, 0, 0, 0, 0) \
        + struct.pack("<20sHHHIII", _name20("EOP"), 0, 0, 1, 0, 0, 0)
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 1, 0)
    pgen = _gen(GEN_INSTRUMENT, 0) + _gen(0, 0)
    mod_end = b"\0" * 10

    info = _list(b"INFO",
                 _chunk(b"ifil", struct.pack("<HH", 2, 1)),
                 _chunk(b"isng", _zstr("EMU8000")),
                 _chunk(b"INAM", _zstr(inst.name)),
                 _chunk(b"ISFT", _zstr("samplevst")),
                 _chunk(b"ICMT", _zstr("Creato da campioni con samplevst")))
    sdta = _list(b"sdta", _chunk(b"smpl", b"".join(pcm)))
    pdta = _list(b"pdta",
                 _chunk(b"phdr", phdr), _chunk(b"pbag", pbag), _chunk(b"pmod", mod_end), _chunk(b"pgen", pgen),
                 _chunk(b"inst", inst_rec), _chunk(b"ibag", ibag), _chunk(b"imod", mod_end), _chunk(b"igen", igen),
                 _chunk(b"shdr", shdr))
    body = b"sfbk" + info + sdta + pdta
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
