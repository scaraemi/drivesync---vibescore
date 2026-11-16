#pragma once

#include "params.h"
#include "sampler.h"

#include "pluginterfaces/vst/ivstevents.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

namespace samplevst {

class Processor : public Steinberg::Vst::AudioEffect
{
public:
    Processor();
    static Steinberg::FUnknown* create(void*) { return static_cast<Steinberg::Vst::IAudioProcessor*>(new Processor); }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement* outputs,
                                                     Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::uint32 PLUGIN_API getTailSamples() override { return Steinberg::Vst::kInfiniteTail; }

private:
    void applyParam(Steinberg::Vst::ParamID id, double value);
    void handleEvent(const Steinberg::Vst::Event& e);

    Sampler m_sampler;
    std::array<double, kNumParams> m_params {};
    std::vector<float> m_scratchL, m_scratchR;
};
}
