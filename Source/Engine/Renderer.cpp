#include "Renderer.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <unordered_map>

namespace sp
{

static inline float readLinear (const float* x, int n, double p) noexcept
{
    if (p < 0.0 || p >= (double) (n - 1)) return 0.0f;
    const int i = (int) p;
    const float f = (float) (p - i);
    return x[i] + (x[i + 1] - x[i]) * f;
}

static void smoothForwardBackward (std::vector<float>& v, double tauSeconds, double controlRate)
{
    if (v.size() < 2) return;
    const float a = (float) std::exp (-1.0 / (tauSeconds * controlRate));
    float s = v.front();
    for (auto& x : v) { s = a * s + (1.0f - a) * x; x = s; }
    s = v.back();
    for (auto it = v.rbegin(); it != v.rend(); ++it) { s = a * s + (1.0f - a) * *it; *it = s; }
}

static const Note* noteAtOutput (const std::vector<Note>& notes, double t) noexcept
{
    auto it = std::upper_bound (notes.begin(), notes.end(), t,
                                [] (double v, const Note& n) { return v < n.outStart; });
    if (it == notes.begin()) return nullptr;
    --it;
    return (t < it->outEnd) ? &*it : nullptr;
}

//==============================================================================
void renderRange (const RenderSource& src, const std::vector<Note>& notes,
                  juce::int64 from, juce::int64 to, float* dest, int destLength,
                  juce::int64* writtenFrom, juce::int64* writtenTo)
{
    if (writtenFrom != nullptr) *writtenFrom = 0;
    if (writtenTo != nullptr)   *writtenTo = 0;
    if (src.samples == nullptr || src.marks == nullptr || src.track == nullptr
        || src.marks->empty() || dest == nullptr || destLength <= 0)
        return;

    const double sr = src.sampleRate;

    from = juce::jlimit<juce::int64> (0, destLength, from);
    to   = juce::jlimit<juce::int64> (from, destLength, to);

    TimeMap map;
    map.build (notes, src.length / sr);

    // Seams only where the voice is not pitched (breaths, consonants, silence).
    // Two renders that start at different points place the voice periods at
    // slightly different positions: crossfading them inside a sung note makes
    // them cancel ("crackle"). Starting and ending each render in an unvoiced
    // gap avoids it: there the phase does not matter. Without a gap nearby the
    // render simply extends to the start / end of the take.
    const double total = (double) destLength / sr;
    const double fadeSec = 0.010, needSec = 0.014, stepSec = 0.0025;
    auto unvoicedOut = [&] (double tout) { return src.track->midiAt (map.outToIn (tout)) <= 0.0f; };

    double aSec = 0.0;
    {
        int run = 0;
        for (double t = (double) from / sr - 0.02; t > 0.0; t -= stepSec)
        {
            run = unvoicedOut (t) ? run + 1 : 0;
            if (run * stepSec >= needSec) { aSec = t; break; }   // [t, t + needSec] is unvoiced
        }
    }
    double bSec = total;
    {
        int run = 0;
        for (double t = (double) to / sr + 0.02; t < total; t += stepSec)
        {
            run = unvoicedOut (t) ? run + 1 : 0;
            if (run * stepSec >= needSec) { bSec = t; break; }   // [t - needSec, t] is unvoiced
        }
    }

    const juce::int64 a = juce::jlimit<juce::int64> (0, destLength, (juce::int64) (aSec * sr));
    const juce::int64 b = juce::jlimit<juce::int64> (a, destLength, (juce::int64) std::ceil (bSec * sr));
    const juce::int64 fadeInEnd  = a > 0 ? juce::jmin (b, a + (juce::int64) (fadeSec * sr)) : a;
    const juce::int64 fadeOutBeg = b < destLength ? juce::jmax (fadeInEnd, b - (juce::int64) (fadeSec * sr)) : b;
    const int len = (int) (b - a);
    if (len <= 0) return;
    if (writtenFrom != nullptr) *writtenFrom = a;
    if (writtenTo != nullptr)   *writtenTo = b;

    // ---- control curves (every 32 samples) --------------------------------
    constexpr int step = 32;
    const int nc = len / step + 2;
    const double controlRate = sr / step;
    std::vector<float> shift ((size_t) nc), form ((size_t) nc), gain ((size_t) nc), timeEdit ((size_t) nc);

    for (int c = 0; c < nc; ++c)
    {
        const double t = (double) (a + (juce::int64) c * step) / sr;
        const Note* n = noteAtOutput (notes, t);
        timeEdit[(size_t) c] = map.isIdentityAt (t, 0.5 / sr) ? 0.0f : 1.0f;

        if (n != nullptr)
        {
            const double tin = map.outToIn (t);
            const float m0 = src.track->midiAt (tin);
            const float nm = n->newMidiAt (*src.track, tin);
            shift[(size_t) c] = (m0 > 0.0f && nm > 0.0f) ? nm - m0 : n->newCenter() - n->origMidi;
            form[(size_t) c]  = n->formant;
            gain[(size_t) c]  = n->gainDb;
        }
    }

    smoothForwardBackward (shift, 0.012, controlRate);
    smoothForwardBackward (form,  0.015, controlRate);
    smoothForwardBackward (gain,  0.010, controlRate);

    // Where processing is needed (dilated by 15 ms, then softened).
    std::vector<float> mask ((size_t) nc);
    for (int c = 0; c < nc; ++c)
        mask[(size_t) c] = (std::abs (shift[(size_t) c]) > 0.002f || std::abs (form[(size_t) c]) > 0.002f
                            || std::abs (gain[(size_t) c]) > 0.01f || timeEdit[(size_t) c] > 0.0f) ? 1.0f : 0.0f;
    {
        const int r = (int) (0.015 * controlRate);
        std::vector<float> d (mask);
        for (int c = 0; c < nc; ++c)
            if (mask[(size_t) c] > 0.0f)
                for (int k = std::max (0, c - r); k <= std::min (nc - 1, c + r); ++k) d[(size_t) k] = 1.0f;
        mask = d;
        smoothForwardBackward (mask, 0.004, controlRate);
    }

    auto ctrl = [&] (const std::vector<float>& v, double pos)
    {
        const double ci = juce::jlimit (0.0, (double) (nc - 1), (pos - (double) a) / step);
        const int i = (int) ci;
        if (i + 1 >= nc) return v[(size_t) i];
        const float f = (float) (ci - i);
        return v[(size_t) i] + (v[(size_t) i + 1] - v[(size_t) i]) * f;
    };

    // ---- pitch-synchronous overlap-add -------------------------------------
    const auto& marks = *src.marks;
    const int pad = (int) (sr / 40.0) + 8;   // longer than the longest grain half-length
    std::vector<float> acc ((size_t) (len + 2 * pad), 0.0f), wsum ((size_t) (len + 2 * pad), 0.0f);

    bool anyProcessed = false;
    for (auto m : mask) if (m > 0.0f) { anyProcessed = true; break; }

    if (anyProcessed)
    {
        double t = (double) a;
        while (t < (double) b)
        {
            const double tin = map.outToIn (t / sr) * sr;
            const int k = marks.nearest (tin);
            double mpos = marks.pos[(size_t) k];
            const float T = marks.period[(size_t) k];
            const bool voiced = marks.voiced[(size_t) k] != 0;

            const double rho = voiced ? std::pow (2.0, ctrl (shift, t) / 12.0) : 1.0;
            const double phi = std::pow (2.0, ctrl (form, t) / 12.0);
            const float g = juce::Decibels::decibelsToGain (ctrl (gain, t));

            if (! voiced && ctrl (timeEdit, t) > 0.0f)   // stretching noise: decorrelate repeated grains
            {
                const auto h = ((juce::uint32) k * 2654435761u) ^ (juce::uint32) (t * 0.25);
                mpos += ((double) (h % 1000u) / 1000.0 - 0.5) * T * 0.5;   // same result in every render
            }

            // Grain half-length: one input period, or the output spacing when the pitch
            // goes down, so neighbouring grains always overlap (no gaps = no buzz).
            const double Tout = (double) T / rho;
            const int L = std::max (8, (int) std::round (std::max ((double) T, Tout)));
            const int centre = (int) std::round (t - (double) a) + pad;
            const double invL = juce::MathConstants<double>::pi / L;

            for (int j = -L; j <= L; ++j)
            {
                const int idx = centre + j;
                if (idx < 0 || idx >= (int) acc.size()) continue;
                const float w = (float) (0.5 + 0.5 * std::cos (j * invL));
                acc[(size_t) idx]  += g * w * readLinear (src.samples, src.length, mpos + j * phi);
                wsum[(size_t) idx] += w;
            }

            t += std::max (Tout, 16.0);
        }
    }

    // ---- mix with the untouched take and crossfade into dest ----------------
    for (int i = 0; i < len; ++i)
    {
        const juce::int64 p = a + i;
        float orig = p < src.length ? src.samples[p] : 0.0f;
        float m = ctrl (mask, (double) p);
        if (m > 0.0f && ! marks.protect.empty())
        {
            // consonant attack / mouth click: keep the original few milliseconds
            const double tin = ctrl (timeEdit, (double) p) > 0.0f ? map.outToIn ((double) p / sr) * sr : (double) p;
            const float pr = marks.protectAt (tin);
            if (pr > 0.0f)
            {
                if (tin != (double) p) orig = readLinear (src.samples, src.length, tin);
                m *= 1.0f - pr;
            }
        }
        float v = orig;
        if (m > 0.0f)
        {
            const float ws = wsum[(size_t) (i + pad)];
            const float ps = ws > 1.0e-3f ? acc[(size_t) (i + pad)] / ws : 0.0f;   // exact overlap-add normalisation
            v = m * ps + (1.0f - m) * orig;
        }

        float x = 1.0f;
        if (p < fadeInEnd)        x = (float) (p - a) / (float) juce::jmax<juce::int64> (1, fadeInEnd - a);
        else if (p >= fadeOutBeg) x = (float) (b - p) / (float) juce::jmax<juce::int64> (1, b - fadeOutBeg);

        dest[p] = dest[p] * (1.0f - x) + v * x;
    }
}

//==============================================================================
bool changedRange (const std::vector<Note>& before, const std::vector<Note>& after,
                   double total, double& from, double& to)
{
    from = 1.0e18;
    to = -1.0;

    std::unordered_map<int, const Note*> old;
    for (const auto& n : before) old[n.id] = &n;

    auto include = [&] (double s, double e) { from = std::min (from, s); to = std::max (to, e); };

    for (const auto& n : after)
    {
        auto it = old.find (n.id);
        if (it == old.end())
            include (n.outStart, n.outEnd);
        else
        {
            if (! n.sameEdits (*it->second))
                include (std::min (n.outStart, it->second->outStart), std::max (n.outEnd, it->second->outEnd));
            old.erase (it);
        }
    }
    for (const auto& kv : old)
        include (kv.second->outStart, kv.second->outEnd);

    if (to < 0.0)
        return false;

    // Gaps around a moved note stretch too: extend to the neighbouring notes.
    double prevEnd = 0.0, nextStart = total;
    for (const auto& n : after)
    {
        if (n.outEnd <= from + 1.0e-9) prevEnd = std::max (prevEnd, n.outEnd);
        if (n.outStart >= to - 1.0e-9) nextStart = std::min (nextStart, n.outStart);
    }
    from = std::max (0.0, std::min (from, prevEnd) - 0.05);
    to   = std::min (total, std::max (to, nextStart) + 0.05);
    return true;
}

} // namespace sp
