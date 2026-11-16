// The plug-in factory. Unlike the SDK's macros, the classes are registered at run time: their name and ID come
// from the bundle's instrument.txt, so one binary serves every instrument made from samples.
#include "controller.h"
#include "processor.h"
#include "version.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "public.sdk/source/main/pluginfactory.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

//! UTF-8 -> UTF-16, at most n - 1 units plus the terminator
void toUtf16(const std::string& s, char16* out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < s.size() && o + 1 < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        int len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            len = 2;
        } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            len = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            len = 4;
        } else {
            cp = '?';
        }
        i += size_t(len);
        if (cp >= 0x10000) {
            if (o + 2 >= n) {
                break;
            }
            cp -= 0x10000;
            out[o++] = char16(0xD800 + (cp >> 10));
            out[o++] = char16(0xDC00 + (cp & 0x3FF));
        } else {
            out[o++] = char16(cp);
        }
    }
    out[o] = 0;
}

void fill8(PClassInfo2& info, const FUID& cid, const char* category, const std::string& name, const char* sub)
{
    std::memset(&info, 0, sizeof(info));
    cid.toTUID(info.cid);
    info.cardinality = PClassInfo::kManyInstances;
    std::strncpy(info.category, category, PClassInfo::kCategorySize - 1);
    // UTF-8 in the 8-bit name: what most hosts show; cut on a character boundary
    size_t n = std::min(name.size(), size_t(PClassInfo::kNameSize - 1));
    while (n > 0 && n < name.size() && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) {
        --n;
    }
    std::memcpy(info.name, name.data(), n);
    if (sub) {
        std::strncpy(info.subCategories, sub, PClassInfo2::kSubCategoriesSize - 1);
    }
    std::strncpy(info.vendor, SAMPLEVST_VENDOR, PClassInfo2::kVendorSize - 1);
    std::strncpy(info.version, SAMPLEVST_VERSION, PClassInfo2::kVersionSize - 1);
    std::strncpy(info.sdkVersion, kVstVersionString, PClassInfo2::kVersionSize - 1);
}

void fillW(PClassInfoW& info, const FUID& cid, const char* category, const std::string& name, const char* sub)
{
    std::memset(&info, 0, sizeof(info));
    cid.toTUID(info.cid);
    info.cardinality = PClassInfo::kManyInstances;
    std::strncpy(info.category, category, PClassInfo::kCategorySize);
    toUtf16(name, info.name, PClassInfo::kNameSize);
    info.classFlags = 0;
    if (sub) {
        std::strncpy(info.subCategories, sub, PClassInfo2::kSubCategoriesSize);
    }
    toUtf16(SAMPLEVST_VENDOR, info.vendor, PClassInfo2::kVendorSize);
    toUtf16(SAMPLEVST_VERSION, info.version, PClassInfo2::kVersionSize);
    toUtf16(kVstVersionString, info.sdkVersion, PClassInfo2::kVersionSize);
}
}

//! The SDK's factory answers getClassInfo/getClassInfo2 with nothing for a class registered as Unicode, and
//! many hosts ask only those: the classes are registered with 8-bit (UTF-8) infos, and the Unicode infos,
//! with the accented name intact, are answered here.
class Factory : public CPluginFactory
{
public:
    using CPluginFactory::CPluginFactory;
    PClassInfoW unicode[2] {};

    tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW* info) override
    {
        if (!info || index < 0 || index >= 2 || index >= countClasses()) {
            return kInvalidArgument;
        }
        *info = unicode[index];
        return kResultOk;
    }
};

SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory()
{
    if (gPluginFactory) {
        gPluginFactory->addRef();
        return gPluginFactory;
    }
    static PFactoryInfo factoryInfo(SAMPLEVST_VENDOR, SAMPLEVST_URL, SAMPLEVST_EMAIL, PFactoryInfo::kUnicode);
    auto* factory = new Factory(factoryInfo);
    gPluginFactory = factory;

    const auto inst = samplevst::instrumentHeader();
    const char* sub = inst->kit ? "Instrument|Drum|Sampler" : "Instrument|Sampler";
    const std::string controllerName = inst->name + " Controller";
    PClassInfo2 processor, controller;
    fill8(processor, samplevst::processorUid(*inst), kVstAudioEffectClass, inst->name, sub);
    fill8(controller, samplevst::controllerUid(*inst), kVstComponentControllerClass, controllerName, nullptr);
    fillW(factory->unicode[0], samplevst::processorUid(*inst), kVstAudioEffectClass, inst->name, sub);
    fillW(factory->unicode[1], samplevst::controllerUid(*inst), kVstComponentControllerClass, controllerName, nullptr);
    factory->registerClass(&processor, samplevst::Processor::create);
    factory->registerClass(&controller, samplevst::Controller::create);
    return gPluginFactory;
}
