#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** REAPER without ARA: REAPER gives VST3 plug-ins its own API (IReaperHostApplication).
    With it the plugin can move REAPER's edit cursor and start / stop the transport,
    so the time ruler of Scriba Pitch works like the DAW's one even when ARA is off. */
class ReaperHost  : public juce::VST3ClientExtensions
{
public:
    void setIHostApplication (Steinberg::FUnknown* host) override;

    bool isAvailable() const noexcept { return setEditCurPos != nullptr; }

    /** Moves REAPER's cursor to this project time (seconds). While playing, playback jumps there. */
    bool moveCursorTo (double projectSeconds) const;
    /** Play / stop. */
    bool togglePlay() const;

private:
    void (*setEditCurPos) (double time, bool moveview, bool seekplay) = nullptr;
    int  (*getPlayState) () = nullptr;
    void (*onPlayButton) () = nullptr;
    void (*onStopButton) () = nullptr;
};
