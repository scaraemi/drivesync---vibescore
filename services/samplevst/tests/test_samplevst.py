"""pytest services/samplevst/tests

The SoundFont tests play the result with FluidSynth (the synthesizer inside MuseScore), the VST3 tests with
pedalboard (a JUCE host) and the Steinberg validator; each is skipped when its tool is missing.
SAMPLEVST_TEMPLATE: the SampleVST.vst3 bundle. SAMPLEVST_VALIDATOR: the SDK's validator.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from samplevst.analyze import envelope_db, pitch_track
from samplevst.instrument import Options, drum_key, note_in_name, parse_note
from samplevst.pipeline import SampleError, create, options_from_manifest

SR = 44100
TEMPLATE = Path(os.environ.get("SAMPLEVST_TEMPLATE", "")) if os.environ.get("SAMPLEVST_TEMPLATE") else None
VALIDATOR = os.environ.get("SAMPLEVST_VALIDATOR", "")
needs_fluid = pytest.mark.skipif(not shutil.which("fluidsynth"), reason="FluidSynth not installed")
needs_vst = pytest.mark.skipif(TEMPLATE is None or not TEMPLATE.exists(), reason="SAMPLEVST_TEMPLATE not set")

try:
    import mido
except ImportError:  # pragma: no cover
    mido = None


# ---- test sounds -------------------------------------------------------------------------------------------------

def bowed(f: float, dur: float, rate: int = SR) -> np.ndarray:
    """A string-like sustained tone with vibrato."""
    t = np.arange(int(rate * dur)) / rate
    ph = 2 * np.pi * np.cumsum(f * (1 + 0.004 * np.sin(2 * np.pi * 5.5 * t))) / rate
    x = sum(np.sin(k * ph) / k for k in range(1, 15))
    return 0.3 * x * np.minimum(1, t / 0.05) * np.minimum(1, (dur - t) / 0.1)


def plucked(f: float, dur: float) -> np.ndarray:
    t = np.arange(int(SR * dur)) / SR
    return 0.5 * sum(np.sin(2 * np.pi * f * k * t) / k * np.exp(-t * (2 + k)) for k in range(1, 8))


def kick() -> np.ndarray:
    t = np.arange(int(SR * 0.5)) / SR
    return np.sin(2 * np.pi * np.cumsum(50 + 100 * np.exp(-t * 30)) / SR) * np.exp(-t * 8)


def noise_hit(seed: int, decay: float = 20) -> np.ndarray:
    t = np.arange(int(SR * 0.4)) / SR
    return np.random.default_rng(seed).standard_normal(len(t)) * np.exp(-t * decay) * 0.5


def hz(midi: int) -> float:
    return 440 * 2 ** ((midi - 69) / 12)


def write(path: Path, x: np.ndarray, rate: int = SR) -> Path:
    sf.write(str(path), x, rate)
    return path


# ---- helpers -------------------------------------------------------------------------------------------------------

def midi_file(path: Path, notes: list[tuple[float, float, int, int]]) -> Path:
    mf = mido.MidiFile(ticks_per_beat=480)
    tr = mido.MidiTrack()
    mf.tracks.append(tr)
    tr.append(mido.MetaMessage("set_tempo", tempo=500000))
    ev = sorted([(s, 1, k, v) for s, d, k, v in notes] + [(s + d, 0, k, 0) for s, d, k, v in notes])
    t = 0
    for s, on, k, v in ev:
        tick = int(round(s * 960))
        tr.append(mido.Message("note_on" if on else "note_off", note=k, velocity=v, time=tick - t))
        t = tick
    mf.save(str(path))
    return path


def fluid(sf2: Path, notes, tmp: Path) -> np.ndarray:
    mid = midi_file(tmp / "t.mid", notes)
    out = tmp / "fluid.wav"
    subprocess.run(["fluidsynth", "-ni", "-q", "-g", "1.0", "-r", str(SR), "-F", str(out), str(sf2), str(mid)],
                   check=True, capture_output=True)
    return sf.read(str(out), always_2d=True)[0]


def pitch_at(x: np.ndarray, a: float, b: float, rate: int = SR) -> float | None:
    m, _ = pitch_track(x[int(a * rate): int(b * rate)].mean(axis=1), rate)
    m = m[~np.isnan(m)]
    return float(np.median(m)) if len(m) else None


def level_at(x: np.ndarray, t: float, rate: int = SR) -> float:
    return float(envelope_db(x[:, 0], rate)[int(t / 0.01)])


# ---- unit tests ----------------------------------------------------------------------------------------------------

def test_note_names():
    assert parse_note("C4") == 60 and parse_note("A4") == 69 and parse_note("C#3") == 49 and parse_note("Eb2") == 39
    assert parse_note("la4") == 69 and parse_note("Sol#3") == 56 and parse_note("72") == 72
    assert note_in_name("violino_A4.wav") == 69 and note_in_name("Flute C#5 ff.wav") == 73
    assert note_in_name("tromba-sol3.mp3") == 55 and note_in_name("take 2.wav") is None
    assert drum_key("Kick 01.wav") == 36 and drum_key("snare_rim.wav") == 38 and drum_key("HiHat Open.wav") == 46
    assert drum_key("cassa.wav") == 36 and drum_key("voce.wav") is None


def test_manifest():
    o = options_from_manifest({"nome": "Violino", "tipo": "batteria", "nota": "la4", "loop": "no", "rilascio": 1}, "x")
    assert (o.name, o.kind, o.root, o.loop, o.release) == ("Violino", "kit", 69, "off", 1.0)
    assert options_from_manifest({}, "Senza nome").name == "Senza nome"


def test_one_note_is_found_and_looped(tmp_path):
    f = write(tmp_path / "corda.wav", np.concatenate([np.zeros(SR // 2), bowed(hz(57), 2.5), np.zeros(SR // 2)]))
    r = create([f], Options(name="Corda"), tmp_path / "out")
    inst = r.instrument
    assert inst.kind == "melodic" and len(inst.zones) == 1
    z = inst.zones[0]
    assert z.root == 57 and z.mode == "loop" and (z.lokey, z.hikey) == (0, 127)
    s = inst.sounds[0]
    assert s.loop and (s.loop[1] - s.loop[0]) / SR > 0.3
    assert s.duration < 2.6, "the silence around the note is cut"


def test_several_notes_in_one_file_and_with_silence(tmp_path):
    legato = write(tmp_path / "scala.wav", np.concatenate([bowed(hz(60), 1.5), bowed(hz(64), 1.5), bowed(hz(67), 1.5)]))
    spaced = write(tmp_path / "pizz.wav", np.concatenate([plucked(hz(48), 1.5), np.zeros(SR // 2), plucked(hz(55), 1.5)]))
    inst = create([legato, spaced], Options(name="Archi"), tmp_path / "out").instrument
    assert sorted(z.root for z in inst.zones) == [48, 55, 60, 64, 67]
    # neighbours split the keys between them
    zones = sorted(inst.zones, key=lambda z: z.root)
    assert zones[0].lokey == 0 and zones[-1].hikey == 127
    assert all(a.hikey + 1 == b.lokey for a, b in zip(zones, zones[1:]))
    plucks = [z for z in inst.zones if z.root in (48, 55)]
    assert all(z.mode == "release" for z in plucks), "a plucked sound dies away: no loop"


def test_name_gives_the_note_and_velocity_layers(tmp_path):
    soft = write(tmp_path / "piano_C4_p.wav", plucked(hz(60), 1.0) * 0.2)
    loud = write(tmp_path / "piano_C4_f.wav", plucked(hz(60), 1.0))
    other = write(tmp_path / "piano_C5.wav", plucked(hz(72), 1.0))
    inst = create([soft, loud, other], Options(name="Piano"), tmp_path / "out").instrument
    c4 = sorted((z for z in inst.zones if z.root == 60), key=lambda z: z.lovel)
    assert len(c4) == 2 and c4[0].hivel + 1 == c4[1].lovel and c4[1].hivel == 127
    assert inst.sounds[c4[0].sound].loudness < inst.sounds[c4[1].sound].loudness


def test_kit_uses_general_midi_keys(tmp_path):
    files = [write(tmp_path / "Kick.wav", kick()), write(tmp_path / "Snare.wav", noise_hit(1)),
             write(tmp_path / "hihat closed.wav", noise_hit(2, 60)), write(tmp_path / "hihat open.wav", noise_hit(3, 8)),
             write(tmp_path / "vetro.wav", noise_hit(4, 30))]
    inst = create(files, Options(name="Batteria"), tmp_path / "out").instrument
    keys = {inst.sounds[z.sound].source: z.lokey for z in inst.zones}
    assert inst.kind == "kit"
    assert keys["Kick.wav"] == 36 and keys["Snare.wav"] == 38 and keys["hihat closed.wav"] == 42
    assert keys["hihat open.wav"] == 46 and keys["vetro.wav"] == 60
    assert all(z.mode == "oneshot" and z.lokey == z.hikey == z.root for z in inst.zones)
    hats = [z.group for z in inst.zones if z.lokey in (42, 46)]
    assert hats == [1, 1], "the closed hi-hat stops the open one"


def test_lone_sound_without_pitch_is_transposed(tmp_path):
    inst = create([write(tmp_path / "boing.wav", noise_hit(5, 5))], Options(name="Effetto"), tmp_path / "out").instrument
    assert inst.kind == "melodic" and inst.zones[0].root == 60 and inst.zones[0].lokey == 0


def test_any_format_and_rate(tmp_path):
    if not shutil.which("ffmpeg"):
        pytest.skip("ffmpeg not installed")
    hi = write(tmp_path / "flauto.wav", bowed(hz(72), 2.0, 96000), 96000)
    mp3 = tmp_path / "oboe.mp3"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(write(tmp_path / "o.wav", bowed(hz(65), 2.0))), str(mp3)],
                   check=True)
    inst = create([hi, mp3], Options(name="Fiati"), tmp_path / "out").instrument
    assert sorted(z.root for z in inst.zones) == [65, 72]
    assert {s.rate for s in inst.sounds} == {44100, 48000}, "96 kHz is brought down to 48 kHz"


def test_no_audio(tmp_path):
    bad = tmp_path / "note.txt"
    bad.write_text("not audio")
    with pytest.raises(SampleError):
        create([bad], Options(name="X"), tmp_path / "out")


# ---- SoundFont, played by FluidSynth ---------------------------------------------------------------------------------

@needs_fluid
@pytest.mark.skipif(mido is None, reason="mido not installed")
def test_soundfont_plays_in_tune_and_holds(tmp_path):
    f = write(tmp_path / "viola_A3.wav", bowed(hz(57), 2.0))
    r = create([f, write(tmp_path / "viola_A4.wav", bowed(hz(69), 2.0))], Options(name="Viola"), tmp_path / "out")
    notes = [(0, 1.5, 50, 100), (2, 1.5, 62, 100), (4, 1.5, 76, 100), (6, 6.0, 69, 100)]
    x = fluid(r.sf2, notes, tmp_path)
    for s, _, k, _ in notes:
        assert abs(pitch_at(x, s + 0.3, s + 1.2) - k) < 0.1, k
    # held for 6 s, three times the sample: the loop keeps it going
    assert level_at(x, 11.5) > level_at(x, 6.5) - 6


@needs_fluid
@pytest.mark.skipif(mido is None, reason="mido not installed")
def test_soundfont_kit(tmp_path):
    r = create([write(tmp_path / "kick.wav", kick()), write(tmp_path / "snare.wav", noise_hit(1))],
               Options(name="Kit"), tmp_path / "out")
    x = fluid(r.sf2, [(0, 0.05, 36, 110), (1, 0.05, 38, 110), (2, 0.05, 50, 110)], tmp_path)
    assert level_at(x, 0.1) > -40 and level_at(x, 1.05) > -40, "short notes still play the whole hit"
    assert level_at(x, 2.2) < -80, "keys without a sound stay silent"


# ---- VST3 ------------------------------------------------------------------------------------------------------------

@needs_vst
def test_vst3_bundle(tmp_path):
    r = create([write(tmp_path / "corno_F3.wav", bowed(hz(53), 2.0))], Options(name="Corno è"), tmp_path / "out", TEMPLATE)
    import zipfile
    names = zipfile.ZipFile(r.vst3_zip).namelist()
    assert "Corno è.vst3/Contents/Resources/instrument.txt" in names
    assert any(n.startswith("Corno è.vst3/Contents/Resources/samples/") for n in names)
    assert not any(n.endswith("moduleinfo.json") for n in names)
    txt = (tmp_path / "out" / "bundle" / "Corno è.vst3" / "Contents" / "Resources" / "instrument.txt").read_text("utf-8")
    assert f"uid {r.instrument.uid}" in txt and "name Corno è" in txt
    for arch in (tmp_path / "out" / "bundle" / "Corno è.vst3" / "Contents").iterdir():
        assert not any(f.stem == "SampleVST" for f in arch.iterdir()), "the module is renamed like the bundle"


@needs_vst
@pytest.mark.skipif(not VALIDATOR, reason="SAMPLEVST_VALIDATOR not set")
def test_vst3_passes_the_steinberg_validator(tmp_path):
    r = create([write(tmp_path / "s.wav", bowed(hz(60), 1.5))], Options(name="Prova"), tmp_path / "out", TEMPLATE)
    out = subprocess.run([VALIDATOR, str(tmp_path / "out" / "bundle" / "Prova.vst3")], capture_output=True, text=True)
    print(out.stdout[-600:])
    assert out.returncode == 0 and "0 tests failed" in out.stdout and "name = Prova" in out.stdout


@needs_vst
@pytest.mark.skipif(mido is None, reason="mido not installed")
def test_vst3_plays_in_a_host(tmp_path):
    pedalboard = pytest.importorskip("pedalboard")
    files = [write(tmp_path / "corda_C4.wav", bowed(hz(60), 1.5)), write(tmp_path / "corda_C5.wav", bowed(hz(72), 1.5))]
    r = create(files, Options(name="Corda"), tmp_path / "out", TEMPLATE)
    log = tmp_path / "plugin.log"
    os.environ["SAMPLEVST_LOG"] = str(log)   # read by the plug-in on its first call
    try:
        plugin = pedalboard.load_plugin(str(tmp_path / "out" / "bundle" / "Corda.vst3"))
    except ImportError:
        print("What the host asked the plug-in:\n" + (log.read_text() if log.exists() else "(nothing: not loaded)"))
        raise
    assert plugin.name == "Corda" and plugin.is_instrument
    notes = [(0, 1.5, 55, 100), (2, 1.5, 67, 100), (4, 1.5, 79, 100), (6, 6.0, 64, 100)]
    msgs = []
    for s, d, k, v in notes:
        msgs += [mido.Message("note_on", note=k, velocity=v, time=s), mido.Message("note_off", note=k, velocity=0, time=s + d)]
    x = plugin(msgs, duration=13.5, sample_rate=48000, num_channels=2, buffer_size=512).T
    for s, _, k, _ in notes:
        assert abs(pitch_at(x, s + 0.3, s + 1.2, 48000) - k) < 0.1, k
    assert level_at(x, 11.5, 48000) > level_at(x, 6.5, 48000) - 6, "held note keeps sounding (loop)"
    assert level_at(x, 13.3, 48000) < -80, "and stops after the release"


@needs_vst
def test_vst3_module_finds_its_bundle_on_a_non_ascii_path(tmp_path):
    """Loads the module itself (no host in between) from "Più è.vst3" in a folder "Città": its factory must
    report the instrument's name, read from Resources/instrument.txt next to it (wide APIs on Windows)."""
    import ctypes
    import sys

    r = create([write(tmp_path / "s.wav", bowed(hz(60), 1.0))], Options(name="Più è"), tmp_path / "Città", TEMPLATE)
    contents = tmp_path / "Città" / "bundle" / "Più è.vst3" / "Contents"
    if sys.platform.startswith("win"):
        module = contents / "x86_64-win" / "Più è.vst3"
    elif sys.platform == "darwin":
        module = contents / "MacOS" / "Più è"
    else:
        module = contents / "x86_64-linux" / "Più è.so"
    lib = ctypes.CDLL(str(module))
    if not sys.platform.startswith("win"):
        entry = getattr(lib, "bundleEntry" if sys.platform == "darwin" else "ModuleEntry")
        entry.restype = ctypes.c_bool
        entry.argtypes = [ctypes.c_void_p]
        assert entry(None)
    lib.GetPluginFactory.restype = ctypes.c_void_p
    factory = lib.GetPluginFactory()
    assert factory

    # IPluginFactory vtable: queryInterface, addRef, release, getFactoryInfo, countClasses, getClassInfo, ...
    vtable = ctypes.cast(ctypes.cast(factory, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    proto = ctypes.WINFUNCTYPE if sys.platform.startswith("win") else ctypes.CFUNCTYPE

    class PClassInfo(ctypes.Structure):
        _fields_ = [("cid", ctypes.c_ubyte * 16), ("cardinality", ctypes.c_int32), ("category", ctypes.c_char * 32),
                    ("name", ctypes.c_char * 64)]

    count = proto(ctypes.c_int32, ctypes.c_void_p)(vtable[4])(factory)
    get_info = proto(ctypes.c_int32, ctypes.c_void_p, ctypes.c_int32, ctypes.POINTER(PClassInfo))(vtable[5])
    infos = []
    for i in range(count):
        info = PClassInfo()
        assert get_info(factory, i, ctypes.byref(info)) == 0
        infos.append((info.category.decode(), info.name.decode("utf-8"), bytes(info.cid).hex().upper()))
    processor = [i for i in infos if i[0] == "Audio Module Class"]
    assert [p[1] for p in processor] == ["Più è"], infos
    if not sys.platform.startswith("win"):   # Windows stores the class ID in COM's byte order
        assert processor[0][2] == r.instrument.uid
