#include "PitchTypes.h"
#include <algorithm>

namespace sp
{

//==============================================================================
float PitchTrack::sample (const std::vector<float>& v, double pos) noexcept
{
    if (v.empty() || pos < 0.0 || pos > (double) (v.size() - 1))
        return 0.0f;

    const auto i = (size_t) pos;
    const auto a = v[i];
    if (i + 1 >= v.size()) return a;
    const auto b = v[i + 1];

    if (a <= 0.0f || b <= 0.0f)   // do not interpolate into unvoiced frames
        return (pos - (double) i) < 0.5 ? a : b;

    const auto f = (float) (pos - (double) i);
    return a + (b - a) * f;
}

float PitchTrack::midiAt (double sec) const noexcept { return sample (midi, (sec - offset) / hop); }
float PitchTrack::slowAt (double sec) const noexcept { return sample (slow, (sec - offset) / hop); }

//==============================================================================
void Note::moveCenterTo (float midi, bool exact) noexcept
{
    const float offsetFromSemi = origMidi - std::round (origMidi);

    if (exact)
    {
        centerAmt = 1.0f;
        shift = std::round (midi) - std::round (origMidi);
    }
    else
    {
        shift = midi - origMidi + offsetFromSemi * centerAmt;
    }
}

float Note::newMidiAt (const PitchTrack& track, double tin) const noexcept
{
    const float m0 = track.midiAt (tin);
    if (m0 <= 0.0f)
        return 0.0f;

    float s = track.slowAt (tin);
    if (s <= 0.0f) s = m0;

    const float slowDev = s - origMidi;
    const float fast    = m0 - s;
    const float natural = newCenter() + slowDev * (1.0f - driftAmt) + fast * vibAmt;

    if (! hasDrawing() || inLength() <= 0.0)
        return natural;

    // Pencil: inside a drawn stroke the drawn pitch wins; strokes are separated
    // where two points are more than 6% of the note apart (erased gaps).
    const float u = (float) ((tin - inStart) / inLength());
    if (u < drawU.front() || u > drawU.back())
        return natural;
    const auto it = std::upper_bound (drawU.begin(), drawU.end(), u);
    const size_t i1 = (size_t) juce::jlimit<long> (1, (long) drawU.size() - 1, (long) std::distance (drawU.begin(), it));
    const size_t i0 = i1 - 1;
    const float gap = drawU[i1] - drawU[i0];
    if (gap > 0.06f)
        return natural;
    const float f = gap > 1.0e-6f ? (u - drawU[i0]) / gap : 0.0f;
    const float drawn = newCenter() + drawOff[i0] + (drawOff[i1] - drawOff[i0]) * f;

    // fade 15 ms into and out of each stroke so the voice does not jump
    auto strokeEdge = [this] (size_t from, int dir)
    {
        size_t k = from;
        while (true)
        {
            const long next = (long) k + dir;
            if (next < 0 || next >= (long) drawU.size()) break;
            if (std::abs (drawU[(size_t) next] - drawU[k]) > 0.06f) break;
            k = (size_t) next;
        }
        return drawU[k];
    };
    const double fadeU = 0.015 / inLength();
    const double dIn = u - strokeEdge (i0, -1), dOut = strokeEdge (i1, +1) - u;
    const float w = (float) juce::jlimit (0.0, 1.0, std::min (dIn, dOut) / fadeU);
    return natural + (drawn - natural) * w;
}

bool Note::isDrawnAt (float u) const noexcept
{
    if (! hasDrawing() || u < drawU.front() || u > drawU.back()) return false;
    const auto it = std::upper_bound (drawU.begin(), drawU.end(), u);
    const size_t i1 = (size_t) juce::jlimit<long> (1, (long) drawU.size() - 1, (long) std::distance (drawU.begin(), it));
    return drawU[i1] - drawU[i1 - 1] <= 0.06f;
}

void Note::drawSegment (float u0, float u1, const std::vector<std::pair<float, float>>& pts)
{
    if (u1 < u0) std::swap (u0, u1);
    eraseDrawing (u0, u1);
    for (const auto& p : pts)
    {
        const float u = juce::jlimit (0.0f, 1.0f, p.first);
        const auto it = std::lower_bound (drawU.begin(), drawU.end(), u);
        const auto idx = std::distance (drawU.begin(), it);
        drawU.insert (it, u);
        drawOff.insert (drawOff.begin() + idx, p.second);
    }
}

void Note::eraseDrawing (float u0, float u1)
{
    if (u1 < u0) std::swap (u0, u1);
    std::vector<float> nu, no;
    for (size_t i = 0; i < drawU.size(); ++i)
        if (drawU[i] < u0 || drawU[i] > u1) { nu.push_back (drawU[i]); no.push_back (drawOff[i]); }
    drawU.swap (nu);
    drawOff.swap (no);
    if (drawU.size() < 2) clearDrawing();
}

void Note::splitDrawing (float u, std::vector<float>& rightU, std::vector<float>& rightOff)
{
    rightU.clear(); rightOff.clear();
    std::vector<float> lu, lo;
    for (size_t i = 0; i < drawU.size(); ++i)
    {
        if (drawU[i] <= u) { lu.push_back (u > 0.0f ? drawU[i] / u : 0.0f); lo.push_back (drawOff[i]); }
        else { rightU.push_back (u < 1.0f ? (drawU[i] - u) / (1.0f - u) : 1.0f); rightOff.push_back (drawOff[i]); }
    }
    drawU.swap (lu);
    drawOff.swap (lo);
    if (drawU.size() < 2) clearDrawing();
    if (rightU.size() < 2) { rightU.clear(); rightOff.clear(); }
}

bool Note::isIdentity() const noexcept
{
    return std::abs (newCenter() - origMidi) < 1.0e-4f
        && driftAmt < 1.0e-4f
        && std::abs (vibAmt - 1.0f) < 1.0e-4f
        && std::abs (formant) < 1.0e-4f
        && std::abs (gainDb) < 1.0e-4f
        && ! hasDrawing()
        && std::abs (outStart - inStart) < 1.0e-6
        && std::abs (outEnd - inEnd) < 1.0e-6;
}

bool Note::sameEdits (const Note& o) const noexcept
{
    using juce::exactlyEqual;
    return id == o.id
        && exactlyEqual (inStart, o.inStart) && exactlyEqual (inEnd, o.inEnd)
        && exactlyEqual (outStart, o.outStart) && exactlyEqual (outEnd, o.outEnd)
        && exactlyEqual (origMidi, o.origMidi) && exactlyEqual (shift, o.shift)
        && exactlyEqual (centerAmt, o.centerAmt) && exactlyEqual (driftAmt, o.driftAmt)
        && exactlyEqual (vibAmt, o.vibAmt) && exactlyEqual (formant, o.formant)
        && exactlyEqual (gainDb, o.gainDb)
        && drawU == o.drawU && drawOff == o.drawOff;
}

void sortNotes (std::vector<Note>& notes)
{
    std::sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.outStart < b.outStart; });
}

//==============================================================================
void TimeMap::build (const std::vector<Note>& notes, double total)
{
    outPts.clear();
    inPts.clear();

    auto add = [this] (double o, double i)
    {
        if (! outPts.empty() && (o < outPts.back() || i < inPts.back()))
            return;   // keep the map monotonic
        outPts.push_back (o);
        inPts.push_back (i);
    };

    add (0.0, 0.0);
    for (const auto& n : notes)
    {
        add (n.outStart, n.inStart);
        add (n.outEnd, n.inEnd);
    }
    add (std::max (total, outPts.back()), std::max (total, inPts.back()));
}

static double mapLinear (const std::vector<double>& xs, const std::vector<double>& ys, double x) noexcept
{
    if (xs.empty()) return x;
    if (x <= xs.front()) return ys.front() + (x - xs.front());
    if (x >= xs.back())  return ys.back() + (x - xs.back());

    const auto it = std::upper_bound (xs.begin(), xs.end(), x);
    const auto i = (size_t) std::distance (xs.begin(), it) - 1;
    const double dx = xs[i + 1] - xs[i];
    if (dx <= 1.0e-12) return ys[i + 1];
    return ys[i] + (ys[i + 1] - ys[i]) * (x - xs[i]) / dx;
}

double TimeMap::outToIn (double tout) const noexcept { return mapLinear (outPts, inPts, tout); }
double TimeMap::inToOut (double tin) const noexcept  { return mapLinear (inPts, outPts, tin); }

bool TimeMap::isIdentityAt (double tout, double tol) const noexcept
{
    return std::abs (outToIn (tout) - tout) < tol;
}

//==============================================================================
static const char* const sharpNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char* const majorNames[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
static const char* const minorNames[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B" };

juce::String keyShortName (int key)
{
    if (key < 0 || key > 23) return "-";
    return key < 12 ? juce::String (majorNames[key]) : juce::String (minorNames[key - 12]) + "m";
}

juce::String keyLongName (int key)
{
    if (key < 0 || key > 23) return "-";
    return key < 12 ? juce::String (majorNames[key]) + " MAJOR" : juce::String (minorNames[key - 12]) + " MINOR";
}

juce::String noteName (int midi)
{
    const int pc = ((midi % 12) + 12) % 12;
    const int octave = (int) std::floor (midi / 12.0) - 1;
    return juce::String (sharpNames[pc]) + juce::String (octave);
}

juce::StringArray scaleTypeNames()
{
    return { "Key scale", "Harmonic minor", "Melodic minor", "Dorian", "Mixolydian", "Chromatic" };
}

Scale makeScale (int key, int scaleType)
{
    Scale s;
    if (key < 0 && scaleType != scaleChromatic)
        return s;

    const int root = key < 0 ? 0 : key % 12;
    const bool minor = key >= 12;

    static const int major[]    = { 0, 2, 4, 5, 7, 9, 11 };
    static const int natMinor[] = { 0, 2, 3, 5, 7, 8, 10 };
    static const int harmMinor[]= { 0, 2, 3, 5, 7, 8, 11 };
    static const int melMinor[] = { 0, 2, 3, 5, 7, 9, 11 };
    static const int dorian[]   = { 0, 2, 3, 5, 7, 9, 10 };
    static const int mixo[]     = { 0, 2, 4, 5, 7, 9, 10 };

    const int* steps = minor ? natMinor : major;
    switch (scaleType)
    {
        case scaleHarmonicMinor: steps = harmMinor; break;
        case scaleMelodicMinor:  steps = melMinor; break;
        case scaleDorian:        steps = dorian; break;
        case scaleMixolydian:    steps = mixo; break;
        default: break;
    }

    if (scaleType == scaleChromatic)
        s.pcs.fill (true);
    else
        for (int i = 0; i < 7; ++i)
            s.pcs[(size_t) ((root + steps[i]) % 12)] = true;

    s.tonicPc = root;
    s.valid = true;
    return s;
}

bool Scale::contains (int midi) const noexcept
{
    return valid && pcs[(size_t) (((midi % 12) + 12) % 12)];
}

int Scale::nearest (float midi) const noexcept
{
    const int base = (int) std::round (midi);
    if (! valid || contains (base))
        return base;

    int best = base;
    float bestDist = 1.0e9f;
    for (int c = base - 2; c <= base + 2; ++c)
    {
        if (! contains (c)) continue;
        const float d = std::abs (midi - (float) c);
        const bool tie = std::abs (d - bestDist) <= 1.0e-4f;
        const bool prefer = midi >= (float) base ? c > best : c < best;   // tie: follow the singer's direction
        if (d < bestDist - 1.0e-4f || (tie && prefer))
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

Tuning tuningOf (const Note& n, const Scale& scale, float thresholdCents) noexcept
{
    const float c = n.newCenter();
    const float semi = std::round (c);
    if (scale.valid && ! scale.contains ((int) semi))
        return Tuning::outOfKey;
    return std::abs (c - semi) * 100.0f > thresholdCents ? Tuning::offCents : Tuning::inTune;
}

//==============================================================================
KeyResult detectKey (const std::vector<Note>& notes)
{
    static const double majorProfile[] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double minorProfile[] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    std::array<double, 12> hist {};
    double total = 0.0;
    for (const auto& n : notes)
    {
        const double w = std::sqrt (std::max (0.02, n.inLength()));
        hist[(size_t) (((int) std::round (n.origMidi) % 12 + 12) % 12)] += w;
        total += w;
    }

    KeyResult r;
    if (total <= 0.0 || notes.size() < 4)
        return r;

    auto corr = [&hist] (const double* profile, int root)
    {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += hist[(size_t) i]; my += profile[(i - root + 12) % 12]; }
        mx /= 12.0; my /= 12.0;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double x = hist[(size_t) i] - mx, y = profile[(i - root + 12) % 12] - my;
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        return (sxx > 0 && syy > 0) ? sxy / std::sqrt (sxx * syy) : 0.0;
    };

    std::array<double, 24> scores {};
    for (int k = 0; k < 24; ++k)
        scores[(size_t) k] = corr (k < 12 ? majorProfile : minorProfile, k % 12);

    int best = 0, second = -1;
    for (int k = 1; k < 24; ++k)
        if (scores[(size_t) k] > scores[(size_t) best]) best = k;
    for (int k = 0; k < 24; ++k)
        if (k != best && (second < 0 || scores[(size_t) k] > scores[(size_t) second])) second = k;

    r.key = best;
    r.alt = second;
    const double gap = scores[(size_t) best] - scores[(size_t) second];
    r.confidence = (float) juce::jlimit (0.35, 0.99, 0.55 + gap * 2.5 + (scores[(size_t) best] - 0.6) * 0.5);
    return r;
}

} // namespace sp
