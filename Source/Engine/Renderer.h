#pragma once

#include "Analyzer.h"

namespace sp
{

/** Everything the renderer reads (all owned elsewhere, read-only while rendering). */
struct RenderSource
{
    const float*      samples = nullptr;  // the captured take (mono)
    int               length = 0;
    double            sampleRate = 44100.0;
    const Marks*      marks = nullptr;
    const PitchTrack* track = nullptr;
};

/** Renders the corrected voice for output samples [from, to) into dest (which holds
    the whole corrected take).  Pitch-synchronous overlap-add: pitch moves keep the
    formants, notes can be moved/stretched in time, untouched audio is copied
    bit-exact from the take.  The edges are crossfaded into what dest already holds.
    `notes` must be sorted by output start. The render is extended to the nearest
    unvoiced gaps (seams never fall inside a sung note); writtenFrom / writtenTo
    receive the range actually written. */
void renderRange (const RenderSource& source, const std::vector<Note>& notes,
                  juce::int64 from, juce::int64 to, float* dest, int destLength,
                  juce::int64* writtenFrom = nullptr, juce::int64* writtenTo = nullptr);

/** Output time range (seconds) affected by changing `before` into `after`.
    Returns false when nothing changed. */
bool changedRange (const std::vector<Note>& before, const std::vector<Note>& after,
                   double totalSeconds, double& fromSec, double& toSec);

} // namespace sp
