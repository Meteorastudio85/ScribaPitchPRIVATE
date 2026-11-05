#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <vector>

namespace sp
{

//==============================================================================
/** Pitch of the captured voice, one frame every `hop` seconds. midi == 0 means unvoiced. */
struct PitchTrack
{
    double hop    = 0.005;  // seconds between frames
    double offset = 0.016;  // time of the centre of frame 0
    std::vector<float> midi;  // detected pitch (MIDI, fractional), 0 = unvoiced
    std::vector<float> slow;  // slow component of the pitch (drift), 0 = unvoiced
    std::vector<float> rms;

    bool  empty() const noexcept { return midi.empty(); }
    float midiAt (double sec) const noexcept;  // 0 when unvoiced
    float slowAt (double sec) const noexcept;  // 0 when unvoiced
    double frameTime (int i) const noexcept { return offset + i * hop; }

private:
    static float sample (const std::vector<float>&, double pos) noexcept;
};

//==============================================================================
/** One sung note ("blob"). Input times refer to the captured take, output times
    to the corrected result (they differ once a note is moved or stretched). */
struct Note
{
    int    id = 0;
    double inStart = 0, inEnd = 0;     // seconds in the take
    double outStart = 0, outEnd = 0;   // seconds in the output
    float  origMidi = 60.0f;           // detected centre pitch

    // Edits (identity = shift 0, centerAmt 0, driftAmt 0, vibAmt 1, formant 0, gain 0)
    float shift     = 0.0f;  // semitones added to the note
    float centerAmt = 0.0f;  // 0..1: how much of the singer's offset from the semitone is removed
    float driftAmt  = 0.0f;  // 0..1: how much slow pitch drift is flattened
    float vibAmt    = 1.0f;  // 0..2: vibrato / fast modulation scale
    float formant   = 0.0f;  // semitones
    float gainDb    = 0.0f;

    // Pencil: a hand-drawn pitch curve. Points sorted by drawU (0..1 across the
    // sung note, input time), drawOff = semitones from newCenter(), so the drawing
    // follows the note when it is moved.
    std::vector<float> drawU, drawOff;

    bool hasDrawing() const noexcept { return drawU.size() >= 2; }
    /** Replaces the drawing between u0 and u1 with the given points (sorted by u). */
    void drawSegment (float u0, float u1, const std::vector<std::pair<float, float>>& points);
    /** Removes the drawing between u0 and u1. */
    void eraseDrawing (float u0, float u1);
    void clearDrawing() { drawU.clear(); drawOff.clear(); }
    /** True where the pitch comes from the pencil (u = 0..1 across the note). */
    bool isDrawnAt (float u) const noexcept;
    /** Splits the drawing at u: this keeps [0, u], the returned values hold (u, 1] re-normalised. */
    void splitDrawing (float u, std::vector<float>& rightU, std::vector<float>& rightOff);

    float newCenter() const noexcept
    {
        return origMidi + shift - (origMidi - std::round (origMidi)) * centerAmt;
    }

    /** Sets shift/centerAmt so the note sits exactly on (or at) the given pitch. */
    void moveCenterTo (float midi, bool exact) noexcept;

    /** Pitch the corrected note should have at input time tin, given the original pitch. */
    float newMidiAt (const PitchTrack& track, double tin) const noexcept;

    bool isIdentity() const noexcept;
    bool sameEdits (const Note& o) const noexcept;
    double inLength() const noexcept  { return inEnd - inStart; }
    double outLength() const noexcept { return outEnd - outStart; }
};

//==============================================================================
/** Monotonic piecewise-linear map between output time and input time, built from the notes. */
class TimeMap
{
public:
    void build (const std::vector<Note>& notesSortedByTime, double totalSeconds);
    double outToIn (double tout) const noexcept;
    double inToOut (double tin) const noexcept;
    bool   isIdentityAt (double tout, double tolerance) const noexcept;

private:
    std::vector<double> outPts, inPts;
};

//==============================================================================
// Keys and scales.  Key index: 0..11 = C..B major, 12..23 = C..B minor.
juce::String keyShortName (int key);   // "Am", "C", "F#m"
juce::String keyLongName  (int key);   // "A MINOR"
juce::String noteName     (int midi);  // "F#4"

enum ScaleType
{
    scaleOfKey = 0,     // major or natural minor, from the key
    scaleHarmonicMinor,
    scaleMelodicMinor,
    scaleDorian,
    scaleMixolydian,
    scaleChromatic,
    numScaleTypes
};

juce::StringArray scaleTypeNames();

struct Scale
{
    std::array<bool, 12> pcs {};
    int tonicPc = 0;
    bool valid = false;

    bool contains (int midi) const noexcept;
    /** Nearest pitch of the scale; ties go in the direction of the sung pitch. */
    int nearest (float midi) const noexcept;
};

Scale makeScale (int key, int scaleType);

struct KeyResult
{
    int key = -1;          // 0..23, -1 = unknown
    int alt = -1;
    float confidence = 0.0f;
    bool valid() const noexcept { return key >= 0; }
};

KeyResult detectKey (const std::vector<Note>& notes);

/** How a note sounds against the song: in tune, off by some cents, or a wrong note. */
enum class Tuning { inTune, offCents, outOfKey };

constexpr float offCentsThreshold = 10.0f;   // more than this from the semitone = orange

Tuning tuningOf (const Note&, const Scale&, float thresholdCents = offCentsThreshold) noexcept;

/** Notes sorted by output start. */
void sortNotes (std::vector<Note>&);

} // namespace sp
