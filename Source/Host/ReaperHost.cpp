#include "ReaperHost.h"

#if JucePlugin_Build_VST3

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wshadow-field-in-constructor", "-Wnon-virtual-dtor", "-Wzero-as-null-pointer-constant",
                                     "-Wunused-parameter", "-Wextra-semi", "-Wshadow")
JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4100)
#include <pluginterfaces/base/funknown.h>

namespace reaper
{
    using namespace Steinberg;

    // From REAPER's reaper_vst3_interfaces.h (available from REAPER 5.02)
    class IReaperHostApplication : public FUnknown
    {
    public:
        virtual void* PLUGIN_API getReaperApi (CStringA funcname) = 0;
        virtual void* PLUGIN_API getReaperParent (uint32 w) = 0;
        virtual void* PLUGIN_API reaperExtended (uint32 call, void* parm1, void* parm2, void* parm3) = 0;
    };

    // raw interface id (no FUID object: its code lives only in the VST3 wrapper,
    // and this file is also part of the AU / Standalone builds)
    static const TUID iid = INLINE_UID (0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C);
}
JUCE_END_IGNORE_WARNINGS_MSVC
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

void ReaperHost::setIHostApplication (Steinberg::FUnknown* host)
{
    if (host == nullptr)
        return;

    void* obj = nullptr;
    if (host->queryInterface (reaper::iid, &obj) != Steinberg::kResultOk || obj == nullptr)
        return;

    auto* api = static_cast<reaper::IReaperHostApplication*> (obj);
    setEditCurPos = reinterpret_cast<void (*) (double, bool, bool)> (api->getReaperApi ("SetEditCurPos"));
    getPlayState  = reinterpret_cast<int (*) ()> (api->getReaperApi ("GetPlayState"));
    onPlayButton  = reinterpret_cast<void (*) ()> (api->getReaperApi ("OnPlayButton"));
    onStopButton  = reinterpret_cast<void (*) ()> (api->getReaperApi ("OnStopButton"));
    api->release();   // queryInterface added a reference
}

#else

void ReaperHost::setIHostApplication (Steinberg::FUnknown*) {}

#endif

bool ReaperHost::moveCursorTo (double projectSeconds) const
{
    if (setEditCurPos == nullptr)
        return false;
    const double t = juce::jmax (0.0, projectSeconds);
    juce::MessageManager::callAsync ([fn = setEditCurPos, t] { fn (t, true, true); });
    return true;
}

bool ReaperHost::togglePlay() const
{
    if (onPlayButton == nullptr || onStopButton == nullptr || getPlayState == nullptr)
        return false;
    juce::MessageManager::callAsync ([play = onPlayButton, stop = onStopButton, state = getPlayState]
    {
        if ((state() & 1) != 0) stop(); else play();
    });
    return true;
}
