// REAPER's VST3 host extension (IReaperHostApplication): which project the plug-in
// lives in and where that project's .rpp file is. Outside REAPER everything here
// quietly reports "unknown". Message thread only.
#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace Steinberg { class FUnknown; }

namespace glina::plugin::reaper {

// A REAPER track as REAPER describes it: used to find the same track among the ARA tracks.
struct TrackInfo {
    juce::String name;              // as in the track header ("Track N" when unnamed)
    int number = 0;                 // 1-based; 0 if unknown
    std::vector<double> itemStarts; // project seconds, one per item on the track
};

// One per plug-in instance, fed from VST3ClientExtensions::setIHostApplication.
class HostLink {
public:
    HostLink() = default;
    ~HostLink();

    void attach(Steinberg::FUnknown* hostContext);
    // The REAPER project (ReaProject*) of this FX. Null outside REAPER, and for the
    // first second or two after the FX was created (REAPER fills the parent in later).
    void* project() const;
    // The REAPER track (MediaTrack*) of this FX: the track of a Track FX, or the track of the
    // take of a Take FX. Null outside REAPER and right after the FX was created.
    void* track() const;

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
// The edit cursor of the project, in project seconds (it is where the transport starts, and it
// is set even when the transport is stopped); NaN if unknown.
double cursorSec(void* project);
// Name, number and item starts of a REAPER track; false if the track or the API is unknown.
bool trackInfo(void* track, TrackInfo& out);

} // namespace glina::plugin::reaper
