#include "processor.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace samplevst {

static const int32 kStateVersion = 1;

Processor::Processor()
{
    setControllerClass(controllerUid(*instrumentHeader()));
    m_params = defaults(*instrumentHeader());
}

tresult PLUGIN_API Processor::initialize(FUnknown* context)
{
    tresult r = AudioEffect::initialize(context);
    trace("processor initialize", long(r));
    if (r != kResultOk) {
        return r;
    }
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    addEventInput(STR16("MIDI In"), 16);

    m_sampler.setInstrument(instrument());
    for (uint32 i = 0; i < kNumParams; ++i) {
        applyParam(i, m_params[i]);
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::setBusArrangements(SpeakerArrangement*, int32 numIns, SpeakerArrangement* outputs,
                                                 int32 numOuts)
{
    trace("setBusArrangements ins", long(numIns));
    trace("setBusArrangements outs", long(numOuts));
    if (numIns == 0 && numOuts == 1 && outputs[0] == SpeakerArr::kStereo) {
        return AudioEffect::setBusArrangements(nullptr, 0, outputs, numOuts);
    }
    return kResultFalse;
}

tresult PLUGIN_API Processor::canProcessSampleSize(int32 size)
{
    trace("canProcessSampleSize", long(size));
    return size == kSample32 || size == kSample64 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API Processor::setupProcessing(ProcessSetup& setup)
{
    trace("setupProcessing rate", long(setup.sampleRate));
    trace("setupProcessing block", long(setup.maxSamplesPerBlock));
    trace("setupProcessing size", long(setup.symbolicSampleSize));
    m_sampler.setRate(setup.sampleRate);
    m_scratchL.assign(size_t(std::max<int32>(setup.maxSamplesPerBlock, 1)), 0.f);
    m_scratchR.assign(m_scratchL.size(), 0.f);
    return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API Processor::setActive(TBool state)
{
    trace("setActive", long(state));
    if (!state) {
        m_sampler.allOff(true);
    }
    return AudioEffect::setActive(state);
}

void Processor::applyParam(ParamID id, double v)
{
    Settings& s = m_sampler.settings();
    switch (id) {
    case kVolume: s.gain = std::pow(10.0, volumeDb(v) / 20.0); break;
    case kAttack: s.attack = attackSec(v); break;
    case kRelease: s.release = releaseSec(v); break;
    case kTune: s.tuneCents = tuneCents(v); break;
    case kVelocity: s.velocitySense = v; break;
    case kBendRange: s.bendRange = bendSemis(v); break;
    case kMidiPitchBend: m_sampler.setPitchBend(v * 2.0 - 1.0); return;
    case kMidiSustain: m_sampler.setSustain(v >= 0.5); return;
    case kMidiVolume: m_sampler.setChannelVolume(v); return;
    case kMidiExpression: m_sampler.setExpression(v); return;
    case kMidiAllOff:
        if (v > 0.5) {
            m_sampler.allOff(false);
        }
        return;
    default: return;
    }
    if (id < kNumParams) {
        m_params[id] = v;
    }
}

void Processor::handleEvent(const Event& e)
{
    switch (e.type) {
    case Event::kNoteOnEvent:
        m_sampler.noteOn(e.noteOn.channel, e.noteOn.pitch, e.noteOn.velocity, e.noteOn.noteId);
        break;
    case Event::kNoteOffEvent:
        m_sampler.noteOff(e.noteOff.channel, e.noteOff.pitch, e.noteOff.noteId);
        break;
    default:
        break;
    }
}

tresult PLUGIN_API Processor::process(ProcessData& data)
{
    if (IParameterChanges* changes = data.inputParameterChanges) {
        for (int32 i = 0; i < changes->getParameterCount(); ++i) {
            IParamValueQueue* q = changes->getParameterData(i);
            ParamValue value;
            int32 offset;
            if (q && q->getPointCount() > 0 && q->getPoint(q->getPointCount() - 1, offset, value) == kResultTrue) {
                applyParam(q->getParameterId(), value);
            }
        }
    }
    if (data.numOutputs < 1 || data.outputs[0].numChannels < 2) {
        // still take the notes, so none is left hanging
        if (IEventList* events = data.inputEvents) {
            Event e {};
            for (int32 i = 0; i < events->getEventCount(); ++i) {
                if (events->getEvent(i, e) == kResultOk) {
                    handleEvent(e);
                }
            }
        }
        return kResultOk;
    }

    const int32 frames = data.numSamples;
    if (int32(m_scratchL.size()) < frames) {   // a host that ignores maxSamplesPerBlock
        m_scratchL.resize(size_t(frames));
        m_scratchR.resize(size_t(frames));
    }
    std::fill(m_scratchL.begin(), m_scratchL.begin() + frames, 0.f);
    std::fill(m_scratchR.begin(), m_scratchR.begin() + frames, 0.f);

    // render between events, so each note starts on its own sample
    IEventList* events = data.inputEvents;
    const int32 count = events ? events->getEventCount() : 0;
    int32 pos = 0;
    Event e {};
    for (int32 i = 0; i <= count; ++i) {
        int32 at = frames;
        bool have = false;
        if (i < count && events->getEvent(i, e) == kResultOk) {
            at = std::clamp(e.sampleOffset, pos, frames);
            have = true;
        } else if (i < count) {
            continue;
        }
        if (at > pos) {
            m_sampler.render(m_scratchL.data() + pos, m_scratchR.data() + pos, at - pos);
            pos = at;
        }
        if (have) {
            handleEvent(e);
        }
    }

    AudioBusBuffers& out = data.outputs[0];
    bool silent = true;
    for (int32 n = 0; n < frames && silent; ++n) {
        silent = m_scratchL[size_t(n)] == 0.f && m_scratchR[size_t(n)] == 0.f;
    }
    if (data.symbolicSampleSize == kSample64) {
        for (int32 n = 0; n < frames; ++n) {
            out.channelBuffers64[0][n] = m_scratchL[size_t(n)];
            out.channelBuffers64[1][n] = m_scratchR[size_t(n)];
        }
    } else {
        std::memcpy(out.channelBuffers32[0], m_scratchL.data(), sizeof(float) * size_t(frames));
        std::memcpy(out.channelBuffers32[1], m_scratchR.data(), sizeof(float) * size_t(frames));
    }
    out.silenceFlags = silent ? 0x3 : 0;
    return kResultOk;
}

tresult PLUGIN_API Processor::setState(IBStream* state)
{
    IBStreamer s(state, kLittleEndian);
    int32 version = 0;
    if (!s.readInt32(version) || version < 1) {
        return kResultFalse;
    }
    int32 n = 0;
    if (!s.readInt32(n)) {
        return kResultFalse;
    }
    for (int32 i = 0; i < n; ++i) {
        double v = 0;
        if (!s.readDouble(v)) {
            return kResultFalse;
        }
        if (i < kNumParams) {
            applyParam(ParamID(i), std::clamp(v, 0.0, 1.0));
        }
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::getState(IBStream* state)
{
    IBStreamer s(state, kLittleEndian);
    s.writeInt32(kStateVersion);
    s.writeInt32(kNumParams);
    for (double v : m_params) {
        s.writeDouble(v);
    }
    return kResultOk;
}
}
