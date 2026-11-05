#pragma once

#include "PitchTypes.h"
#include <atomic>

namespace sp
{

/** Pitch detection (YIN, computed with FFTs on a 16 kHz copy of the voice). */
PitchTrack trackPitch (const float* samples, int numSamples, double sampleRate,
                       const std::atomic<bool>* cancel = nullptr);

/** Splits the voiced parts of the track into notes. */
std::vector<Note> segmentNotes (const PitchTrack& track, double totalSeconds);

/** Analysis marks for the pitch-synchronous renderer: one per period on voiced
    parts (aligned to the glottal pulses), a fixed 5 ms grid on unvoiced parts. */
struct Marks
{
    std::vector<double>  pos;     // samples
    std::vector<float>   period;  // samples
    std::vector<uint8_t> voiced;

    /** Consonant attacks and mouth clicks, one value (0..1) every 32 input samples.
        The renderer leaves these few milliseconds untouched: a pitch-synchronous
        shift would repeat or move them, which sounds like crackle. */
    static constexpr int protectBlock = 32;
    std::vector<float> protect;
    float protectAt (double samplePos) const noexcept;

    bool empty() const noexcept { return pos.empty(); }
    int  nearest (double samplePos) const noexcept;
};

Marks computeMarks (const float* samples, int numSamples, double sampleRate, const PitchTrack& track);

} // namespace sp
