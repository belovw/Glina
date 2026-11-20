// REAPER's VST3 host extension (IReaperHostApplication): which project the plug-in
// lives in and where that project's .rpp file is. Outside REAPER everything here
// quietly reports "unknown". Message thread only.
#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace Steinberg { class FUnknown; }

namespace glina::plugin::reaper {

// One per plug-in instance, fed from VST3ClientExtensions::setIHostApplication.
class HostLink {
public:
    HostLink() = default;
    ~HostLink();

    void attach(Steinberg::FUnknown* hostContext);
    // The REAPER project (ReaProject*) of this FX. Null outside REAPER, and for the
    // first second or two after the FX was created (REAPER fills the parent in later).
    void* project() const;

private:
    void* reaperHost_ = nullptr; // IReaperHostApplication*, add-ref'd

    JUCE_DECLARE_NON_COPYABLE(HostLink)
};

// True once a HostLink was attached inside REAPER (the API functions are process-wide).
bool available();
// Full path of the project's .rpp; empty if it was never saved or is no longer open.
juce::String projectFile(void* project);
// The project behind each open tab (saved or not).
std::vector<void*> openProjects();
// REAPER's "project has unsaved changes" flag; -1 if unknown.
int isDirty(void* project);

} // namespace glina::plugin::reaper
