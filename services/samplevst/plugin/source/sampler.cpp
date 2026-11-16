#include "sampler.h"

#include <algorithm>

namespace samplevst {
namespace {
constexpr double kSilence = 1e-4;   // -80 dB: a releasing voice under this is finished

inline float at(const std::vector<float>& d, int64_t i)
{
    return i >= 0 && i < int64_t(d.size()) ? d[size_t(i)] : 0.f;
}

//! 4-point, 3rd-order Hermite between d[i] and d[i+1]
inline float hermite(const std::vector<float>& d, int64_t i, float t, const Sample& s, bool loop)
{
    auto idx = [&](int64_t k) {
        if (loop && k >= s.loopEnd) {
            k = s.loopStart + (k - s.loopEnd) % (s.loopEnd - s.loopStart);
        }
        return k;
    };
    const float xm1 = at(d, idx(i - 1)), x0 = at(d, idx(i)), x1 = at(d, idx(i + 1)), x2 = at(d, idx(i + 2));
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
}

double Sampler::ratio(const Zone& z, int key) const
{
    const Sample& s = m_inst->samples[size_t(z.sample)];
    double semis = (z.mode == Mode::OneShot && m_inst->kit) ? 0.0 : double(key - z.root);
    semis += (z.tune + m_settings.tuneCents) / 100.0;
    return std::pow(2.0, semis / 12.0) * s.rate / m_rate;
}

Sampler::Voice* Sampler::freeVoice()
{
    Voice* best = nullptr;
    for (Voice& v : m_voices) {
        if (!v.active) {
            return &v;
        }
    }
    // all busy: the oldest releasing one, else the oldest
    for (Voice& v : m_voices) {
        if (v.releasing && (!best || v.age < best->age)) {
            best = &v;
        }
    }
    if (!best) {
        for (Voice& v : m_voices) {
            if (!best || v.age < best->age) {
                best = &v;
            }
        }
    }
    return best;
}

void Sampler::startRelease(Voice& v, double seconds)
{
    if (v.releasing) {
        return;
    }
    v.releasing = true;
    v.held = false;
    seconds = std::max(0.003, seconds);
    v.releaseMul = std::exp(std::log(kSilence) / (seconds * m_rate));
}

void Sampler::noteOn(int channel, int key, float velocity, int32_t noteId)
{
    if (!m_inst || velocity <= 0.f) {
        noteOff(channel, key, noteId);
        return;
    }
    const int vel = std::clamp(int(std::lround(velocity * 127.f)), 1, 127);
    for (const Zone& z : m_inst->zones) {
        if (key < z.lokey || key > z.hikey || vel < z.lovel || vel > z.hivel) {
            continue;
        }
        // the same key again: the previous note fades (a real instrument cannot play one note twice);
        // a group (hi-hat) stops its other sounds
        for (Voice& v : m_voices) {
            if (!v.active) {
                continue;
            }
            if (z.group > 0 && v.group == z.group) {
                startRelease(v, 0.01);
            } else if (v.key == key && v.channel == channel && v.zone == &z) {
                startRelease(v, 0.03);
            }
        }
        Voice* v = freeVoice();
        if (!v) {
            return;
        }
        const Sample& s = m_inst->samples[size_t(z.sample)];
        *v = Voice {};
        v->active = true;
        v->channel = channel;
        v->key = key;
        v->noteId = noteId;
        v->group = z.group;
        v->zone = &z;
        v->sample = &s;
        v->baseStep = ratio(z, key);
        const double sense = std::clamp(m_settings.velocitySense, 0.0, 1.0);
        v->gain = float(std::pow(double(velocity), 1.6 * sense));
        const double attack = z.mode == Mode::OneShot ? 0.0005 : m_settings.attack;
        v->attackInc = attack <= 0.0005 ? 1.0 : 1.0 / (attack * m_rate);
        v->level = v->attackInc >= 1.0 ? 1.0 : 0.0;
        v->age = ++m_counter;
    }
}

void Sampler::noteOff(int channel, int key, int32_t noteId)
{
    for (Voice& v : m_voices) {
        if (!v.active || v.releasing || !v.held) {
            continue;
        }
        const bool match = noteId >= 0 && v.noteId >= 0 ? v.noteId == noteId : (v.key == key && v.channel == channel);
        if (!match || v.zone->mode == Mode::OneShot) {
            continue;
        }
        if (m_sustain) {
            v.held = false;
            v.pedalHeld = true;
        } else {
            startRelease(v, m_settings.release);
        }
    }
}

void Sampler::setSustain(bool down)
{
    m_sustain = down;
    if (down) {
        return;
    }
    for (Voice& v : m_voices) {
        if (v.active && v.pedalHeld) {
            v.pedalHeld = false;
            startRelease(v, m_settings.release);
        }
    }
}

void Sampler::allOff(bool hard)
{
    for (Voice& v : m_voices) {
        if (hard) {
            v.active = false;
        } else if (v.active) {
            startRelease(v, 0.02);
        }
    }
    m_sustain = false;
}

int Sampler::activeVoices() const
{
    return int(std::count_if(m_voices.begin(), m_voices.end(), [](const Voice& v) { return v.active; }));
}

void Sampler::render(float* left, float* right, int frames)
{
    if (!m_inst) {
        return;
    }
    const double bend = m_inst->kit ? 1.0 : std::pow(2.0, m_bend * m_settings.bendRange / 12.0);
    const float master = float(m_settings.gain * m_ccVolume * m_ccVolume * m_ccExpression);
    for (Voice& v : m_voices) {
        if (!v.active) {
            continue;
        }
        const Sample& s = *v.sample;
        const bool loop = v.zone->mode == Mode::Loop && s.looped();
        const bool stereo = !s.right.empty();
        const double step = v.baseStep * bend;
        const int64_t end = s.frames();
        for (int n = 0; n < frames; ++n) {
            // envelope
            if (v.releasing) {
                v.level *= v.releaseMul;
                if (v.level < kSilence) {
                    v.active = false;
                    break;
                }
            } else if (v.level < 1.0) {
                v.level = std::min(1.0, v.level + v.attackInc);
            }
            if (loop && v.pos >= double(s.loopEnd)) {
                v.pos -= double(s.loopEnd - s.loopStart);
            }
            const int64_t i = int64_t(v.pos);
            if (i >= end) {
                v.active = false;
                break;
            }
            const float t = float(v.pos - double(i));
            const float g = float(v.level) * v.gain * master;
            const float l = hermite(s.left, i, t, s, loop);
            left[n] += l * g;
            right[n] += (stereo ? hermite(s.right, i, t, s, loop) : l) * g;
            v.pos += step;
        }
    }
}
}
