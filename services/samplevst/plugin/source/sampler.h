// The sound engine: voices reading the samples with 4-point interpolation, an amplitude envelope, loops,
// velocity layers, sustain pedal, pitch bend, hi-hat style groups. No allocation while playing.
#pragma once

#include "instrument.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace samplevst {

struct Settings {
    double gain = 1.0;        // linear, from the Volume parameter
    double attack = 0.002;    // seconds
    double release = 0.25;
    double tuneCents = 0.0;
    double velocitySense = 1.0;  // 0: every note at full level, 1: soft notes much softer
    double bendRange = 2.0;      // semitones
};

class Sampler
{
public:
    static constexpr int kVoices = 64;

    void setInstrument(std::shared_ptr<const Instrument> inst) { m_inst = std::move(inst); allOff(true); }
    const Instrument* instrumentData() const { return m_inst.get(); }
    void setRate(double rate) { m_rate = rate > 0 ? rate : 44100.0; }
    Settings& settings() { return m_settings; }

    void noteOn(int channel, int key, float velocity, int32_t noteId);
    void noteOff(int channel, int key, int32_t noteId);
    void setPitchBend(double v) { m_bend = v; }          // -1..1
    void setSustain(bool down);
    void setChannelVolume(double v) { m_ccVolume = v; }  // 0..1, CC7
    void setExpression(double v) { m_ccExpression = v; } // 0..1, CC11
    void allOff(bool hard);

    //! Adds to out (zeroed by the caller)
    void render(float* left, float* right, int frames);

    int activeVoices() const;

private:
    struct Voice {
        bool active = false;
        bool held = true;          // key down (or kept down by the pedal)
        bool pedalHeld = false;    // key up while the pedal is down
        bool releasing = false;
        int channel = 0, key = 0, group = 0;
        int32_t noteId = -1;
        const Zone* zone = nullptr;
        const Sample* sample = nullptr;
        double pos = 0.0;
        double baseStep = 1.0;     // without pitch bend
        double level = 0.0;        // envelope
        double attackInc = 1.0;
        double releaseMul = 0.0;
        float gain = 1.f;
        uint64_t age = 0;
    };

    Voice* freeVoice();
    void startRelease(Voice& v, double seconds);
    double ratio(const Zone& z, int key) const;

    std::shared_ptr<const Instrument> m_inst;
    std::array<Voice, kVoices> m_voices {};
    Settings m_settings;
    double m_rate = 44100.0;
    double m_bend = 0.0;
    double m_ccVolume = 1.0, m_ccExpression = 1.0;
    bool m_sustain = false;
    uint64_t m_counter = 0;
};
}
