#include "ReaperHostProbe.h"

#include <atomic>
#include <vector>

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wnon-virtual-dtor", "-Wzero-as-null-pointer-constant", "-Wunused-parameter",
                                    "-Wshadow", "-Wextra-semi")
JUCE_BEGIN_IGNORE_WARNINGS_MSVC(4100)
#include <pluginterfaces/base/ftypes.h>
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
JUCE_END_IGNORE_WARNINGS_MSVC

namespace {

// REAPER's VST3 host extension, queried from the IHostApplication context (REAPER 5.02+).
// Method order and IID are copied from reaper-sdk/sdk/reaper_vst3_interfaces.h
// (the identical file ships with JUCE: examples/Plugins/extern/reaper_vst3_interfaces.h).
class IReaperHostApplication : public Steinberg::FUnknown {
public:
    // Gets a REAPER extension API function by name, NULL if it failed.
    virtual void* PLUGIN_API getReaperApi(Steinberg::CStringA funcname) = 0;
    // Parent of this FX: track=1, take=2, project=3, fxdsp=4, trackchan=5.
    virtual void* PLUGIN_API getReaperParent(Steinberg::uint32 w) = 0;
    virtual void* PLUGIN_API reaperExtended(Steinberg::uint32 call, void* parm1, void* parm2, void* parm3) = 0;
};
// INLINE_UID follows COM_COMPATIBLE, so the byte order matches what the host compares against.
const Steinberg::TUID kReaperHostApplicationIID = INLINE_UID(0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C);

} // namespace

JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace glina::plugin {
namespace {

// Signatures from reaper-sdk/sdk/reaper_plugin_functions.h (ReaProject*, MediaItem_Take*, PCM_source* -> void*).
using FnGetAppVersion = const char* (*)();
using FnGetProjectPath = void (*)(char* bufOut, int bufOut_sz);
using FnGetProjectPathEx = void (*)(void* proj, char* bufOut, int bufOut_sz);
using FnEnumProjects = void* (*)(int idx, char* projfnOutOptional, int projfnOutOptional_sz);
using FnGetProjectName = void (*)(void* proj, char* bufOut, int bufOut_sz);
using FnGetSetProjectInfoString = bool (*)(void* project, const char* desc, char* valuestrNeedBig, bool is_set);
using FnGetMediaItemTakeSource = void* (*)(void* take);
using FnGetMediaSourceFileName = void (*)(void* source, char* filenamebufOut, int filenamebufOut_sz);

std::atomic<int> instanceCounter { 0 };

juce::File logFile()
{
    return juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("glina_host_test.txt");
}

juce::String stamp()
{
    const auto t = juce::Time::getCurrentTime();
    return t.formatted("%Y-%m-%d %H:%M:%S") + "." + juce::String(t.getMilliseconds()).paddedLeft('0', 3);
}

void writeLog(const juce::String& text)
{
    static juce::CriticalSection lock;
    const juce::ScopedLock sl(lock);
    logFile().appendText(text, false, false, "\r\n");
}

juce::String ptr(const void* p)
{
    return p == nullptr ? juce::String("NULL") : "0x" + juce::String::toHexString((juce::pointer_sized_int) p);
}

juce::String quoted(const char* s)
{
    return s == nullptr ? juce::String("(nullptr)") : "\"" + juce::String::fromUTF8(s) + "\"";
}

juce::String resultName(Steinberg::tresult r)
{
    switch (r) {
    case Steinberg::kResultOk: return "kResultOk";
    case Steinberg::kNoInterface: return "kNoInterface";
    case Steinberg::kResultFalse: return "kResultFalse";
    case Steinberg::kNotImplemented: return "kNotImplemented";
    case Steinberg::kInvalidArgument: return "kInvalidArgument";
    default: return "tresult " + juce::String((int) r);
    }
}

} // namespace

ReaperHostProbe::ReaperHostProbe() : instanceId(++instanceCounter)
{
    juce::String s;
    s << "[" << stamp() << "] instance #" << instanceId << ": плагин создан"
#ifdef JucePlugin_VersionString
      << " (GlinaProbe " << JucePlugin_VersionString << ")"
#endif
      << ", процесс " << juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileName() << "\n";
    writeLog(s);
}

ReaperHostProbe::~ReaperHostProbe()
{
    stopTimer();
    writeLog("[" + stamp() + "] instance #" + juce::String(instanceId) + ": плагин удалён\n");
    if (reaperHost != nullptr) static_cast<IReaperHostApplication*>(reaperHost)->release();
    if (host != nullptr) host->release();
}

void ReaperHostProbe::attach(Steinberg::FUnknown* hostContext)
{
    ++attachCount;
    if (hostContext != host) {
        if (reaperHost != nullptr) static_cast<IReaperHostApplication*>(reaperHost)->release();
        if (host != nullptr) host->release();
        reaperHost = nullptr;
        host = hostContext;
        if (host != nullptr) host->addRef();
    }
    runProbe("setIHostApplication, вызов " + juce::String(attachCount));

    // The FX may not be in a take's chain yet at this moment: look again a little later.
    timerStep = 0;
    startTimer(1500);
}

void ReaperHostProbe::timerCallback()
{
    static constexpr int nextDelayMs[] = { 5000, 15000 };
    static constexpr const char* labels[] = { "через ~1,5 с", "через ~6,5 с", "через ~21,5 с" };
    stopTimer();
    const int step = timerStep++;
    runProbe(juce::String("повтор ") + labels[step] + " после setIHostApplication");
    if (step < 2) startTimer(nextDelayMs[step]);
}

void ReaperHostProbe::runProbe(const juce::String& why)
{
    juce::String s;
    s << "[" << stamp() << "] instance #" << instanceId << ": " << why << "\n";

    // 1. IHostApplication::getName
    if (host == nullptr) {
        s << "  1. IHostApplication: хост не передал контекст (nullptr)\n";
    } else {
        void* obj = nullptr;
        const auto r = host->queryInterface(Steinberg::Vst::IHostApplication_iid, &obj);
        if (r == Steinberg::kResultOk && obj != nullptr) {
            auto* app = static_cast<Steinberg::Vst::IHostApplication*>(obj);
            Steinberg::Vst::String128 name {};
            const auto nr = app->getName(name);
            const juce::String hostName(
                juce::CharPointer_UTF16(reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(name)));
            s << "  1. IHostApplication::getName -> " << resultName(nr) << ", \"" << hostName << "\"\n";
            app->release();
        } else {
            s << "  1. IHostApplication: queryInterface -> " << resultName(r) << "\n";
        }
    }

    // 2. REAPER's own VST3 interface
    if (host != nullptr && reaperHost == nullptr) {
        void* obj = nullptr;
        const auto r = host->queryInterface(kReaperHostApplicationIID, &obj);
        if (r == Steinberg::kResultOk && obj != nullptr) reaperHost = obj;
        else s << "  2. IReaperHostApplication {79655E36-77EE4267-A573FEF7-4912C27C}: queryInterface -> "
               << resultName(r) << "\n";
    }
    if (auto* rh = static_cast<IReaperHostApplication*>(reaperHost)) {
        s << "  2. IReaperHostApplication: есть\n";
        if (auto f = reinterpret_cast<FnGetAppVersion>(rh->getReaperApi("GetAppVersion")))
            s << "     GetAppVersion() -> " << quoted(f()) << "\n";

        void* track = rh->getReaperParent(1);
        void* take = rh->getReaperParent(2);
        void* project = rh->getReaperParent(3);
        s << "     getReaperParent: track(1)=" << ptr(track) << ", take(2)=" << ptr(take)
          << ", project(3)=" << ptr(project) << "\n";

        std::vector<char> buf(65536, 0);
        const int bufSize = (int) buf.size();

        if (auto f = reinterpret_cast<FnGetProjectPath>(rh->getReaperApi("GetProjectPath"))) {
            buf[0] = 0;
            f(buf.data(), bufSize);
            s << "     getReaperApi(\"GetProjectPath\") = " << ptr((void*) f) << "; GetProjectPath() -> "
              << quoted(buf.data()) << "   [папка записи/медиа активного проекта]\n";
        } else {
            s << "     getReaperApi(\"GetProjectPath\") -> NULL\n";
        }

        void* current = nullptr;
        if (auto f = reinterpret_cast<FnEnumProjects>(rh->getReaperApi("EnumProjects"))) {
            buf[0] = 0;
            current = f(-1, buf.data(), bufSize);
            s << "     EnumProjects(-1) -> project=" << ptr(current) << ", файл проекта " << quoted(buf.data())
              << (project != nullptr ? (current == project ? "  [= родительский проект FX]" : "  [НЕ родительский проект FX]")
                                     : "")
              << "\n";
        }

        if (project != nullptr) {
            if (auto f = reinterpret_cast<FnGetProjectPathEx>(rh->getReaperApi("GetProjectPathEx"))) {
                buf[0] = 0;
                f(project, buf.data(), bufSize);
                s << "     GetProjectPathEx(родительский проект) -> " << quoted(buf.data()) << "\n";
            }
            if (auto f = reinterpret_cast<FnGetProjectName>(rh->getReaperApi("GetProjectName"))) {
                buf[0] = 0;
                f(project, buf.data(), bufSize);
                s << "     GetProjectName(родительский проект) -> " << quoted(buf.data()) << "\n";
            }
            if (auto f = reinterpret_cast<FnGetSetProjectInfoString>(rh->getReaperApi("GetSetProjectInfo_String"))) {
                buf[0] = 0;
                const bool ok = f(project, "RECORD_PATH", buf.data(), false);
                s << "     GetSetProjectInfo_String(RECORD_PATH) -> " << (ok ? "true" : "false") << ", "
                  << quoted(buf.data()) << "   [как задано в настройках проекта, может быть пусто/относительно]\n";
            }
        }

        if (take != nullptr) {
            auto getSource = reinterpret_cast<FnGetMediaItemTakeSource>(rh->getReaperApi("GetMediaItemTake_Source"));
            auto getFileName = reinterpret_cast<FnGetMediaSourceFileName>(rh->getReaperApi("GetMediaSourceFileName"));
            if (getSource != nullptr && getFileName != nullptr) {
                void* source = getSource(take);
                buf[0] = 0;
                if (source != nullptr) getFileName(source, buf.data(), bufSize);
                s << "     файл take: GetMediaItemTake_Source=" << ptr(source) << ", GetMediaSourceFileName -> "
                  << quoted(buf.data()) << "\n";
            }
        }
    }

    // 3. Cockos VST2 extension
    s << "  3. Cockos VST2 audioMaster(0xdeadbeef, 0xdeadf00d, \"GetProjectPath\"): не вызывался - в VST3 нет "
         "audioMasterCallback (он есть только у VST2-плагинов); VST3-аналог этого вызова - getReaperApi, п. 2\n";

    writeLog(s);
}

} // namespace glina::plugin
