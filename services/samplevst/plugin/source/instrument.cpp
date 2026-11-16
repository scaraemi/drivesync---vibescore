#include "instrument.h"

#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace samplevst {
namespace {

void anchor() {}

fs::path modulePath()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &module)) {
        return {};
    }
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf);
#else
    Dl_info info {};
    if (!dladdr(reinterpret_cast<void*>(&anchor), &info) || !info.dli_fname) {
        return {};
    }
    return fs::path(info.dli_fname);
#endif
}

bool parseHex(const std::string& s, std::array<uint8_t, 16>& out)
{
    if (s.size() != 32) {
        return false;
    }
    for (size_t i = 0; i < 16; ++i) {
        const std::string byte = s.substr(i * 2, 2);
        char* end = nullptr;
        const long v = std::strtol(byte.c_str(), &end, 16);
        if (end != byte.c_str() + 2) {
            return false;
        }
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

//! PCM 16/24/32-bit or 32-bit float WAV, mono or stereo
bool readWav(const fs::path& path, Sample& s, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path.u8string();
        return false;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
        error = "not a WAV file: " + path.u8string();
        return false;
    }
    int format = 0, channels = 0, bits = 0;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    for (size_t pos = 12; pos + 8 <= d.size();) {
        const uint32_t size = le32(&d[pos + 4]);
        const uint8_t* body = &d[pos + 8];
        const size_t avail = std::min<size_t>(size, d.size() - pos - 8);
        if (!std::memcmp(&d[pos], "fmt ", 4) && avail >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            s.rate = le32(body + 4);
            bits = le16(body + 14);
            if (format == 0xFFFE && avail >= 26) {
                format = le16(body + 24);   // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the format tag
            }
        } else if (!std::memcmp(&d[pos], "data", 4)) {
            data = body;
            dataSize = avail;
        }
        pos += 8 + size + (size & 1);
    }
    if (!data || channels < 1 || channels > 2 || !(format == 1 || format == 3) || s.rate <= 0) {
        error = "unsupported WAV: " + path.u8string();
        return false;
    }
    const int bytes = bits / 8;
    if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32))) {
        error = "unsupported WAV sample size: " + path.u8string();
        return false;
    }
    const size_t frames = dataSize / (size_t(bytes) * channels);
    s.left.resize(frames);
    if (channels == 2) {
        s.right.resize(frames);
    }
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            const uint8_t* p = data + (i * channels + c) * bytes;
            float v = 0.f;
            if (format == 3) {
                std::memcpy(&v, p, 4);
            } else if (bits == 16) {
                v = int16_t(le16(p)) / 32768.f;
            } else if (bits == 24) {
                const int32_t x = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8;
                v = x / 8388608.f;
            } else {
                v = int32_t(le32(p)) / 2147483648.f;
            }
            (c == 0 ? s.left : s.right)[i] = v;
        }
    }
    return true;
}

std::shared_ptr<Instrument> load(bool withSamples)
{
    auto inst = std::make_shared<Instrument>();
    inst->uid = kTemplateUid;
    const fs::path res = resourcesFolder();
    std::ifstream f(res / "instrument.txt", std::ios::binary);
    if (!f) {
        inst->error = "no instrument.txt in " + res.u8string();
        return inst;
    }
    std::string line;
    std::vector<std::pair<int, std::string> > files;   // sample id, path
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t sp = line.find(' ');
        const std::string key = line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        std::istringstream in(rest);
        if (key == "name") {
            if (!rest.empty()) {
                inst->name = rest;
            }
        } else if (key == "uid") {
            std::array<uint8_t, 16> uid {};
            if (parseHex(rest, uid)) {
                inst->uid = uid;
            }
        } else if (key == "kind") {
            inst->kit = rest == "kit";
        } else if (key == "envelope") {
            in >> inst->attack >> inst->decay >> inst->sustain >> inst->release;
        } else if (key == "sample") {
            int id = -1;
            std::string file;
            int64_t ls = -1, le = -1;
            in >> id >> file >> ls >> le;
            if (id < 0 || id > 4096) {
                continue;
            }
            if (int(inst->samples.size()) <= id) {
                inst->samples.resize(id + 1);
            }
            inst->samples[id].loopStart = ls;
            inst->samples[id].loopEnd = le;
            files.emplace_back(id, file);
        } else if (key == "zone") {
            Zone z;
            std::string mode;
            in >> z.sample >> z.lokey >> z.hikey >> z.lovel >> z.hivel >> z.root >> z.tune >> mode >> z.group;
            z.mode = mode == "loop" ? Mode::Loop : mode == "oneshot" ? Mode::OneShot : Mode::Release;
            if (!in.fail()) {
                inst->zones.push_back(z);
            }
        }
    }
    if (withSamples) {
        for (const auto& [id, file] : files) {
            std::string err;
            if (!readWav(res / fs::u8path(file), inst->samples[id], err)) {
                inst->error += err + "\n";
            }
        }
        // a zone whose sample is missing would play nothing at best
        std::vector<Zone> ok;
        for (const Zone& z : inst->zones) {
            if (z.sample >= 0 && z.sample < int(inst->samples.size()) && inst->samples[z.sample].frames() > 0) {
                ok.push_back(z);
            }
        }
        inst->zones.swap(ok);
        for (Sample& s : inst->samples) {
            if (!s.looped()) {
                s.loopStart = s.loopEnd = -1;
            }
        }
    }
    return inst;
}
}

fs::path resourcesFolder()
{
    // Windows: X.vst3/Contents/x86_64-win/X.vst3   macOS: X.vst3/Contents/MacOS/X   Linux: X.vst3/Contents/x86_64-linux/X.so
    return modulePath().parent_path().parent_path() / "Resources";
}

std::shared_ptr<const Instrument> instrumentHeader()
{
    static std::once_flag once;
    static std::shared_ptr<const Instrument> header;
    std::call_once(once, [] { header = load(false); });
    return header;
}

std::shared_ptr<const Instrument> instrument()
{
    static std::mutex mutex;
    static std::weak_ptr<const Instrument> cached;   // freed when the last instance goes
    std::lock_guard<std::mutex> lock(mutex);
    if (auto inst = cached.lock()) {
        return inst;
    }
    std::shared_ptr<const Instrument> inst = load(true);
    cached = inst;
    return inst;
}
}
