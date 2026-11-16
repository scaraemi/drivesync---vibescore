// Parameters, shared by the processor and the controller. Values travel normalised (0..1).
#pragma once

#include "instrument.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "pluginterfaces/base/funknown.h"

namespace samplevst {

enum ParamId : uint32_t {
    kVolume = 0,      // -40 .. +6 dB
    kAttack,          // 0 .. 4 s, squared
    kRelease,         // 0.005 .. 8 s, squared
    kTune,            // -100 .. +100 cents
    kVelocity,        // velocity sensitivity 0 .. 100 %
    kBendRange,       // 0 .. 12 semitones
    kNumParams,

    // MIDI controllers arrive as hidden parameters (IMidiMapping)
    kMidiPitchBend = 100,
    kMidiSustain,
    kMidiVolume,
    kMidiExpression,
    kMidiAllOff,
};

inline double volumeDb(double n) { return -40.0 + 46.0 * n; }
inline double volumeNorm(double db) { return std::clamp((db + 40.0) / 46.0, 0.0, 1.0); }
inline double attackSec(double n) { return 4.0 * n * n; }
inline double attackNorm(double s) { return std::clamp(std::sqrt(std::max(0.0, s) / 4.0), 0.0, 1.0); }
inline double releaseSec(double n) { return 0.005 + 7.995 * n * n; }
inline double releaseNorm(double s) { return std::clamp(std::sqrt(std::max(0.0, s - 0.005) / 7.995), 0.0, 1.0); }
inline double tuneCents(double n) { return -100.0 + 200.0 * n; }
inline double bendSemis(double n) { return std::round(12.0 * n); }

//! Default normalised values: the envelope comes from the instrument
inline std::array<double, kNumParams> defaults(const Instrument& inst)
{
    return { volumeNorm(0.0), attackNorm(inst.attack), releaseNorm(inst.release), 0.5, 1.0, 2.0 / 12.0 };
}

//! Processor and controller class IDs, from the instrument's uid: every generated instrument has its own
inline Steinberg::FUID processorUid(const Instrument& inst)
{
    const auto& u = inst.uid;
    auto w = [&](int i) { return uint32_t(u[i]) << 24 | uint32_t(u[i + 1]) << 16 | uint32_t(u[i + 2]) << 8 | u[i + 3]; };
    return Steinberg::FUID(w(0), w(4), w(8), w(12));
}

inline Steinberg::FUID controllerUid(const Instrument& inst)
{
    const auto& u = inst.uid;
    auto w = [&](int i) { return uint32_t(u[i]) << 24 | uint32_t(u[i + 1]) << 16 | uint32_t(u[i + 2]) << 8 | u[i + 3]; };
    return Steinberg::FUID(w(0) ^ 0x436F6E74u, w(4) ^ 0x726F6C6Cu, w(8), w(12) ^ 0x65720001u);
}
}
