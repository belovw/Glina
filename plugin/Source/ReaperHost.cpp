#include "ReaperHost.h"

#include <atomic>
#include <cmath>
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
using FnGetPlayStateEx = int (*)(void* proj);
using FnGetPlayPositionEx = double (*)(void* proj);
using FnCountTrackMediaItems = int (*)(void* track);
using FnGetTrackMediaItem = void* (*)(void* track, int itemidx);
using FnGetMediaItemInfo_Value = double (*)(void* item, const char* parmname);
using FnGetMediaTrackInfo_Value = double (*)(void* track, const char* parmname);
using FnGetTrackName = bool (*)(void* track, char* bufOut, int bufOut_sz);
using FnGetActiveTake = void* (*)(void* item);
using FnGetMediaItemTake = void* (*)(void* item, int takeidx);
using FnGetMediaItemNumTakes = int (*)(void* item);
using FnTakeFX_GetCount = int (*)(void* take);
using FnTakeFX_GetFXName = bool (*)(void* take, int fx, char* bufOut, int bufOut_sz);
using FnTakeFX_GetEnabled = bool (*)(void* take, int fx);
using FnTakeFX_GetOffline = bool (*)(void* take, int fx);
using FnTakeFX_GetNamedConfigParm = bool (*)(void* take, int fx, const char* parmname, char* bufOut, int bufOut_sz);

std::atomic<FnEnumProjects> enumProjects { nullptr };
std::atomic<FnIsProjectDirty> isProjectDirty { nullptr };
std::atomic<FnGetCursorPositionEx> getCursorPositionEx { nullptr };
std::atomic<FnGetPlayStateEx> getPlayStateEx { nullptr };
std::atomic<FnGetPlayPositionEx> getPlayPositionEx { nullptr };
std::atomic<FnCountTrackMediaItems> countTrackMediaItems { nullptr };
std::atomic<FnGetTrackMediaItem> getTrackMediaItem { nullptr };
std::atomic<FnGetMediaItemInfo_Value> getMediaItemInfoValue { nullptr };
std::atomic<FnGetMediaTrackInfo_Value> getMediaTrackInfoValue { nullptr };
std::atomic<FnGetTrackName> getTrackName { nullptr };
std::atomic<FnGetActiveTake> getActiveTake { nullptr };
std::atomic<FnGetMediaItemTake> getMediaItemTake { nullptr };
std::atomic<FnGetMediaItemNumTakes> getMediaItemNumTakes { nullptr };
std::atomic<FnTakeFX_GetCount> takeFxCount { nullptr };
std::atomic<FnTakeFX_GetFXName> takeFxName { nullptr };
std::atomic<FnTakeFX_GetEnabled> takeFxEnabled { nullptr };
std::atomic<FnTakeFX_GetOffline> takeFxOffline { nullptr };
std::atomic<FnTakeFX_GetNamedConfigParm> takeFxConfig { nullptr };

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
    if (getPlayStateEx.load() == nullptr)
        getPlayStateEx = reinterpret_cast<FnGetPlayStateEx>(rh(obj)->getReaperApi("GetPlayStateEx"));
    if (getPlayPositionEx.load() == nullptr)
        getPlayPositionEx = reinterpret_cast<FnGetPlayPositionEx>(rh(obj)->getReaperApi("GetPlayPositionEx"));
    if (countTrackMediaItems.load() == nullptr)
        countTrackMediaItems = reinterpret_cast<FnCountTrackMediaItems>(rh(obj)->getReaperApi("CountTrackMediaItems"));
    if (getTrackMediaItem.load() == nullptr)
        getTrackMediaItem = reinterpret_cast<FnGetTrackMediaItem>(rh(obj)->getReaperApi("GetTrackMediaItem"));
    if (getMediaItemInfoValue.load() == nullptr)
        getMediaItemInfoValue = reinterpret_cast<FnGetMediaItemInfo_Value>(rh(obj)->getReaperApi("GetMediaItemInfo_Value"));
    if (getMediaTrackInfoValue.load() == nullptr)
        getMediaTrackInfoValue = reinterpret_cast<FnGetMediaTrackInfo_Value>(rh(obj)->getReaperApi("GetMediaTrackInfo_Value"));
    if (getTrackName.load() == nullptr)
        getTrackName = reinterpret_cast<FnGetTrackName>(rh(obj)->getReaperApi("GetTrackName"));
    if (getActiveTake.load() == nullptr)
        getActiveTake = reinterpret_cast<FnGetActiveTake>(rh(obj)->getReaperApi("GetActiveTake"));
    if (getMediaItemTake.load() == nullptr)
        getMediaItemTake = reinterpret_cast<FnGetMediaItemTake>(rh(obj)->getReaperApi("GetMediaItemTake"));
    if (getMediaItemNumTakes.load() == nullptr)
        getMediaItemNumTakes = reinterpret_cast<FnGetMediaItemNumTakes>(rh(obj)->getReaperApi("GetMediaItemNumTakes"));
    if (takeFxCount.load() == nullptr)
        takeFxCount = reinterpret_cast<FnTakeFX_GetCount>(rh(obj)->getReaperApi("TakeFX_GetCount"));
    if (takeFxName.load() == nullptr)
        takeFxName = reinterpret_cast<FnTakeFX_GetFXName>(rh(obj)->getReaperApi("TakeFX_GetFXName"));
    if (takeFxEnabled.load() == nullptr)
        takeFxEnabled = reinterpret_cast<FnTakeFX_GetEnabled>(rh(obj)->getReaperApi("TakeFX_GetEnabled"));
    if (takeFxOffline.load() == nullptr)
        takeFxOffline = reinterpret_cast<FnTakeFX_GetOffline>(rh(obj)->getReaperApi("TakeFX_GetOffline"));
    if (takeFxConfig.load() == nullptr)
        takeFxConfig = reinterpret_cast<FnTakeFX_GetNamedConfigParm>(rh(obj)->getReaperApi("TakeFX_GetNamedConfigParm"));
}

void* HostLink::project() const
{
    return reaperHost_ != nullptr ? rh(reaperHost_)->getReaperParent(3) : nullptr;
}

void* HostLink::track() const
{
    return reaperHost_ != nullptr ? rh(reaperHost_)->getReaperParent(1) : nullptr;
}

void* HostLink::take() const
{
    return reaperHost_ != nullptr ? rh(reaperHost_)->getReaperParent(2) : nullptr;
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

bool transport(void* project, double& sec, bool& playing)
{
    const auto state = getPlayStateEx.load();
    const auto play = getPlayPositionEx.load();
    const auto cursor = getCursorPositionEx.load();
    if (project == nullptr || state == nullptr || play == nullptr || cursor == nullptr) return false;
    const int st = state(project); // &1 playing, &2 paused, &4 recording
    playing = (st & 1) != 0 && (st & 2) == 0;
    sec = (st & 3) != 0 ? play(project) : cursor(project);
    return std::isfinite(sec);
}

bool trackInfo(void* track, TrackInfo& out, bool glina)
{
    const auto count = countTrackMediaItems.load();
    const auto item = getTrackMediaItem.load();
    const auto info = getMediaItemInfoValue.load();
    if (track == nullptr || count == nullptr || item == nullptr || info == nullptr) return false;
    out = TrackInfo{};
    if (const auto f = getTrackName.load()) {
        std::vector<char> buf(512, 0);
        f(track, buf.data(), (int)buf.size()); // false for an unnamed track, which still gets "Track N"
        out.name = juce::String::fromUTF8(buf.data());
    }
    if (const auto f = getMediaTrackInfoValue.load()) out.number = (int)f(track, "IP_TRACKNUMBER");
    const auto activeTake = getActiveTake.load();
    const auto takeAt = getMediaItemTake.load();
    const auto takeCount = getMediaItemNumTakes.load();
    const int n = count(track);
    for (int i = 0; i < n; ++i) {
        void* it = item(track, i);
        if (it == nullptr) continue;
        out.itemStarts.push_back(info(it, "D_POSITION"));
        int number = 0; // the active take is the one at index j: its number is j + 1
        if (activeTake != nullptr && takeAt != nullptr && takeCount != nullptr) {
            void* active = activeTake(it);
            const int takes = takeCount(it);
            for (int j = 0; j < takes && number == 0; ++j)
                if (takeAt(it, j) == active) number = j + 1;
        }
        out.itemTakes.push_back(number);
        if (glina) out.itemGlina.push_back(activeTake != nullptr ? glinaOfTake(activeTake(it)) : GlinaFx::unknown);
    }
    return true;
}

GlinaFx glinaOfTake(void* take)
{
    const auto count = takeFxCount.load();
    const auto enabled = takeFxEnabled.load();
    if (take == nullptr || count == nullptr || enabled == nullptr) return GlinaFx::unknown;
    const auto name = takeFxName.load();
    const auto offline = takeFxOffline.load();
    const auto config = takeFxConfig.load();
    // Glina by its plug-in identity (a renamed instance keeps it), else by the shown name.
    auto isGlina = [&](int fx) {
        std::vector<char> buf(1024, 0);
        for (const char* parm : {"fx_ident", "original_name"}) {
            buf[0] = 0;
            if (config != nullptr && config(take, fx, parm, buf.data(), (int)buf.size()) &&
                juce::String::fromUTF8(buf.data()).containsIgnoreCase("glina"))
                return true;
        }
        buf[0] = 0;
        return name != nullptr && name(take, fx, buf.data(), (int)buf.size()) &&
               juce::String::fromUTF8(buf.data()).containsIgnoreCase("glina");
    };
    GlinaFx best = GlinaFx::none;
    const int n = count(take);
    for (int fx = 0; fx < n; ++fx) {
        if (!isGlina(fx)) continue;
        if (offline != nullptr && offline(take, fx)) { if (best == GlinaFx::none) best = GlinaFx::offline; continue; }
        if (enabled(take, fx)) return GlinaFx::on;
        best = GlinaFx::bypassed;
    }
    return best;
}

} // namespace glina::plugin::reaper
