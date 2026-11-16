// The instrument a SampleVST bundle plays: read from Contents/Resources/instrument.txt and the WAV files next to it.
// Written by services/samplevst/samplevst/vst3.py, which documents the format.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace samplevst {

struct Sample {
    std::vector<float> left;
    std::vector<float> right;   // empty: mono
    double rate = 44100.0;
    int64_t loopStart = -1;     // frames; -1: no loop
    int64_t loopEnd = -1;

    int64_t frames() const { return static_cast<int64_t>(left.size()); }
    bool looped() const { return loopStart >= 0 && loopEnd > loopStart + 2 && loopEnd <= frames(); }
};

enum class Mode { Loop, Release, OneShot };

struct Zone {
    int sample = 0;
    int lokey = 0, hikey = 127, lovel = 1, hivel = 127;
    int root = 60;
    int tune = 0;          // cents
    Mode mode = Mode::Release;
    int group = 0;         // > 0: a new note of the group stops the others (hi-hat)
};

struct Instrument {
    std::string name = "SampleVST";
    std::array<uint8_t, 16> uid {};
    bool kit = false;
    double attack = 0.002, decay = 0.0, sustain = 1.0, release = 0.25;
    std::vector<Sample> samples;
    std::vector<Zone> zones;
    std::string error;     // why nothing could be loaded, for the log
};

//! Contents/Resources of the bundle this module was loaded from
std::filesystem::path resourcesFolder();

//! Name, ID and envelope only: what the factory and the controller need, without reading the samples
std::shared_ptr<const Instrument> instrumentHeader();

//! Everything, samples included; loaded once and shared by every instance of the plugin
std::shared_ptr<const Instrument> instrument();

//! SAMPLEVST_LOG=<file>: what the host asks, one line per call (for hosts that refuse the plug-in silently)
void trace(const char* what, long value = 0);

//! The template bundle's own ID, used when there is no instrument.txt
constexpr std::array<uint8_t, 16> kTemplateUid { 0x5A, 0x4D, 0x50, 0x4C, 0x45, 0x56, 0x53, 0x54,
                                                 0x8E, 0x21, 0x4B, 0x0C, 0x9D, 0x77, 0x13, 0x01 };
}
