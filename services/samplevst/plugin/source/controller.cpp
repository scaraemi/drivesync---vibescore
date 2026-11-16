#include "controller.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ustring.h"

#include <cstdio>
#include <functional>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace samplevst {
namespace {
//! Shows the value in its unit (dB, seconds, cents) instead of 0..1
class MappedParameter : public Parameter
{
public:
    MappedParameter(const TChar* title, ParamID id, const TChar* units, double def, int32 steps,
                    std::function<double(double)> toPlain, std::function<double(double)> toNorm, const char* format)
        : Parameter(title, id, units, def, steps, ParameterInfo::kCanAutomate), m_toPlain(std::move(toPlain)),
        m_toNorm(std::move(toNorm)), m_format(format)
    {
    }

    void toString(ParamValue v, String128 out) const override
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), m_format, m_toPlain(v));
        UString(out, 128).fromAscii(buf);
    }

    bool fromString(const TChar* in, ParamValue& v) const override
    {
        char buf[64] = {};
        UString(const_cast<TChar*>(in), 64).toAscii(buf, 64);
        double plain = 0;
        if (std::sscanf(buf, "%lf", &plain) != 1) {
            return false;
        }
        v = std::clamp(m_toNorm(plain), 0.0, 1.0);
        return true;
    }

    ParamValue toPlain(ParamValue v) const override { return m_toPlain(v); }
    ParamValue toNormalized(ParamValue p) const override { return std::clamp(m_toNorm(p), 0.0, 1.0); }

private:
    std::function<double(double)> m_toPlain, m_toNorm;
    const char* m_format;
};
}

tresult PLUGIN_API Controller::initialize(FUnknown* context)
{
    tresult r = EditController::initialize(context);
    trace("controller initialize", long(r));
    if (r != kResultOk) {
        return r;
    }
    const auto d = defaults(*instrumentHeader());
    auto add = [this](const TChar* title, ParamID id, const TChar* units, double def, int32 steps,
                      std::function<double(double)> toPlain, std::function<double(double)> toNorm, const char* fmt) {
        parameters.addParameter(new MappedParameter(title, id, units, def, steps, std::move(toPlain), std::move(toNorm), fmt));
    };
    add(STR16("Volume"), kVolume, STR16("dB"), d[kVolume], 0, volumeDb, volumeNorm, "%.1f");
    add(STR16("Attack"), kAttack, STR16("s"), d[kAttack], 0, attackSec, attackNorm, "%.3f");
    add(STR16("Release"), kRelease, STR16("s"), d[kRelease], 0, releaseSec, releaseNorm, "%.3f");
    add(STR16("Tune"), kTune, STR16("cent"), d[kTune], 0, tuneCents, [](double c) { return (c + 100.0) / 200.0; }, "%.0f");
    add(STR16("Velocity"), kVelocity, STR16("%"), d[kVelocity], 0, [](double v) { return v * 100.0; },
        [](double p) { return p / 100.0; }, "%.0f");
    add(STR16("Bend range"), kBendRange, STR16("st"), d[kBendRange], 12, bendSemis, [](double s) { return s / 12.0; }, "%.0f");

    const int32 hidden = ParameterInfo::kCanAutomate | ParameterInfo::kIsHidden;
    parameters.addParameter(STR16("Pitch bend"), nullptr, 0, 0.5, hidden, kMidiPitchBend);
    parameters.addParameter(STR16("Sustain pedal"), nullptr, 1, 0, hidden, kMidiSustain);
    parameters.addParameter(STR16("Channel volume"), nullptr, 0, 100.0 / 127.0, hidden, kMidiVolume);
    parameters.addParameter(STR16("Expression"), nullptr, 0, 1, hidden, kMidiExpression);
    parameters.addParameter(STR16("All notes off"), nullptr, 1, 0, hidden, kMidiAllOff);
    return kResultOk;
}

tresult PLUGIN_API Controller::setComponentState(IBStream* state)
{
    IBStreamer s(state, kLittleEndian);
    int32 version = 0, n = 0;
    if (!s.readInt32(version) || !s.readInt32(n)) {
        return kResultFalse;
    }
    for (int32 i = 0; i < n; ++i) {
        double v = 0;
        if (!s.readDouble(v)) {
            return kResultFalse;
        }
        if (i < kNumParams) {
            setParamNormalized(ParamID(i), v);
        }
    }
    return kResultOk;
}

tresult PLUGIN_API Controller::getMidiControllerAssignment(int32 busIndex, int16, CtrlNumber cc, ParamID& id)
{
    if (busIndex != 0) {
        return kResultFalse;
    }
    switch (cc) {
    case kPitchBend: id = kMidiPitchBend; return kResultTrue;
    case kCtrlSustainOnOff: id = kMidiSustain; return kResultTrue;
    case kCtrlVolume: id = kMidiVolume; return kResultTrue;
    case kCtrlExpression: id = kMidiExpression; return kResultTrue;
    case kCtrlAllSoundsOff:
    case kCtrlAllNotesOff: id = kMidiAllOff; return kResultTrue;
    default: return kResultFalse;
    }
}
}
