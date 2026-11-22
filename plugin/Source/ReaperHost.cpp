#include "ReaperHost.h"

#include <atomic>
#include <limits>

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wnon-virtual-dtor", "-Wzero-as-null-pointer-constant", "-Wunused-parameter",
                                    "-Wshadow", "-Wextra-semi")
JUCE_BEGIN_IGNORE_WARNINGS_MSVC(4100)
#include <pluginterfaces/base/ftypes.h>
#include <pluginterfaces/base/funknown.h>
JUCE_END_IGNORE_WARNINGS_MSVC

namespace {

// Queried from the IHostApplication context (REAPER 5.02+). Method order and IID as in
// reaper-sdk/sdk/reaper_vst3_interfaces.h (also shipped with JUCE in examples/Plugins/extern);
// checked in REAPER 7.86 with the GlinaProbe test build.
class IReaperHostApplication : public Steinberg::FUnknown {
public:
    virtual void* PLUGIN_API getReaperApi(Steinberg::CStringA funcname) = 0;
    // Parent of this FX: track=1, take=2, project=3, fxdsp=4, trackchan=5.
    virtual void* PLUGIN_API getReaperParent(Steinberg::uint32 w) = 0;
    virtual void* PLUGIN_API reaperExtended(Steinberg::uint32 call, void* parm1, void* parm2, void* parm3) = 0;
};
// INLINE_UID follows COM_COMPATIBLE, so the byte order matches what the host compares against.
const Steinberg::TUID kReaperHostApplicationIID = INLINE_UID(0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C);

} // namespace

JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace glina::plugin::reaper {
namespace {

// Signatures from reaper-sdk/sdk/reaper_plugin_functions.h (ReaProject* -> void*).
using FnEnumProjects = void* (*)(int idx, char* projfnOutOptional, int projfnOutOptional_sz);
using FnIsProjectDirty = int (*)(void* proj);
using FnGetCursorPositionEx = double (*)(void* proj);

std::atomic<FnEnumProjects> enumProjects { nullptr };
std::atomic<FnIsProjectDirty> isProjectDirty { nullptr };
std::atomic<FnGetCursorPositionEx> getCursorPositionEx { nullptr };

IReaperHostApplication* rh(void* p) { return static_cast<IReaperHostApplication*>(p); }

} // namespace

HostLink::~HostLink()
{
    if (reaperHost_ != nullptr) rh(reaperHost_)->release();
}

void HostLink::attach(Steinberg::FUnknown* hostContext)
{
    if (reaperHost_ != nullptr || hostContext == nullptr) return;
    void* obj = nullptr;
    if (hostContext->queryInterface(kReaperHostApplicationIID, &obj) != Steinberg::kResultOk || obj == nullptr) return;
    reaperHost_ = obj; // queryInterface add-ref'd it
    if (enumProjects.load() == nullptr)
        enumProjects = reinterpret_cast<FnEnumProjects>(rh(obj)->getReaperApi("EnumProjects"));
    if (isProjectDirty.load() == nullptr)
        isProjectDirty = reinterpret_cast<FnIsProjectDirty>(rh(obj)->getReaperApi("IsProjectDirty"));
    if (getCursorPositionEx.load() == nullptr)
        getCursorPositionEx = reinterpret_cast<FnGetCursorPositionEx>(rh(obj)->getReaperApi("GetCursorPositionEx"));
}

void* HostLink::project() const
{
    return reaperHost_ != nullptr ? rh(reaperHost_)->getReaperParent(3) : nullptr;
}

bool available() { return enumProjects.load() != nullptr; }

juce::String projectFile(void* project)
{
    const auto f = enumProjects.load();
    if (f == nullptr || project == nullptr) return {};
    std::vector<char> buf(4096, 0);
    for (int i = 0; i < 1000; ++i) {
        buf[0] = 0;
        void* p = f(i, buf.data(), (int)buf.size());
        if (p == nullptr) break;
        if (p == project) return juce::String::fromUTF8(buf.data());
    }
    return {};
}

std::vector<void*> openProjects()
{
    std::vector<void*> out;
    const auto f = enumProjects.load();
    if (f == nullptr) return out;
    for (int i = 0; i < 1000; ++i) {
        void* p = f(i, nullptr, 0);
        if (p == nullptr) break;
        out.push_back(p);
    }
    return out;
}

int isDirty(void* project)
{
    const auto f = isProjectDirty.load();
    return f != nullptr && project != nullptr ? f(project) : -1;
}

double cursorSec(void* project)
{
    const auto f = getCursorPositionEx.load();
    return f != nullptr && project != nullptr ? f(project) : std::numeric_limits<double>::quiet_NaN();
}

} // namespace glina::plugin::reaper
