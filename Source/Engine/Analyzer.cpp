#include "Analyzer.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>

namespace sp
{

static float medianOf (std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    const auto mid = v.begin() + (long) (v.size() / 2);
    std::nth_element (v.begin(), mid, v.end());
    return *mid;
}

//==============================================================================
PitchTrack trackPitch (const float* samples, int numSamples, double sampleRate, const std::atomic<bool>* cancel)
{
    PitchTrack t;
    if (samples == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return t;

    constexpr double fs = 16000.0;
    constexpr int W = 512;        // 32 ms analysis window
    constexpr int tauMin = 14;    // ~1140 Hz
    constexpr int tauMax = 246;   // ~65 Hz
    constexpr int hop = 80;       // 5 ms
    constexpr int order = 10;
    constexpr int N = 1 << order;

    // Anti-alias and resample to 16 kHz.
    std::vector<float> f (samples, samples + numSamples);
    if (sampleRate > fs * 1.05)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::IIRFilter lp;
            lp.setCoefficients (juce::IIRCoefficients::makeLowPass (sampleRate, 6500.0));
            lp.processSamples (f.data(), numSamples);
        }
    }

    const double ratio = sampleRate / fs;
    const int m = (int) std::floor ((numSamples - 8) / ratio);
    if (m < W + tauMax + 1)
        return t;

    std::vector<float> y ((size_t) m);
    juce::LagrangeInterpolator interp;
    interp.process (ratio, f.data(), y.data(), m);

    const int frames = (m - (W + tauMax)) / hop + 1;
    t.hop = hop / fs;
    t.offset = (W * 0.5) / fs;
    t.midi.assign ((size_t) frames, 0.0f);
    t.rms.assign ((size_t) frames, 0.0f);
    std::vector<float> aperiodicity ((size_t) frames, 1.0f);

    struct Cand { float midi, cost; };
    std::vector<std::vector<Cand>> candidates ((size_t) frames);
    std::vector<float> uvCostOf ((size_t) frames, 0.0f), frameVoicedProb ((size_t) frames, 0.0f);

    juce::dsp::FFT fft (order);
    std::vector<float> A (2 * N), B (2 * N);
    std::vector<double> sq ((size_t) (W + tauMax + 1)), dn ((size_t) (tauMax + 1));

    for (int fi = 0; fi < frames; ++fi)
    {
        if (cancel != nullptr && (fi & 255) == 0 && cancel->load())
            return {};

        const float* fr = y.data() + (size_t) fi * hop;

        sq[0] = 0.0;
        for (int j = 0; j < W + tauMax; ++j)
            sq[(size_t) j + 1] = sq[(size_t) j] + (double) fr[j] * fr[j];

        const double e1 = sq[W];
        t.rms[(size_t) fi] = (float) std::sqrt (e1 / W);
        if (e1 < 1.0e-9)
            continue;

        std::fill (A.begin(), A.end(), 0.0f);
        std::fill (B.begin(), B.end(), 0.0f);
        std::copy (fr, fr + W, A.begin());
        std::copy (fr, fr + W + tauMax, B.begin());
        fft.performRealOnlyForwardTransform (A.data());
        fft.performRealOnlyForwardTransform (B.data());

        for (int k = 0; k < N; ++k)
        {
            const float ar = A[(size_t) (2 * k)], ai = A[(size_t) (2 * k + 1)];
            const float br = B[(size_t) (2 * k)], bi = B[(size_t) (2 * k + 1)];
            A[(size_t) (2 * k)]     = ar * br + ai * bi;   // conj(A) * B -> cross-correlation
            A[(size_t) (2 * k + 1)] = ar * bi - ai * br;
        }
        fft.performRealOnlyInverseTransform (A.data());

        const double scale = std::abs (A[0]) > 1.0e-12 ? e1 / (double) A[0] : 1.0;

        // Cumulative mean normalised difference function.
        double running = 0.0;
        dn[0] = 1.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            const double e2 = sq[(size_t) (tau + W)] - sq[(size_t) tau];
            double d = e1 + e2 - 2.0 * (double) A[(size_t) tau] * scale;
            if (d < 0.0) d = 0.0;
            running += d;
            dn[(size_t) tau] = running > 0.0 ? d * tau / running : 1.0;
        }

        // Candidates: every dip of the difference function, with pYIN's probability:
        // for a spread of thresholds, the FIRST dip (highest pitch) below each one
        // collects that threshold's weight. Similar dips -> the higher pitch wins,
        // which is what avoids the "octave below" errors.
        struct Dip { double tauF; double v; };
        std::vector<Dip> dips;
        for (int tau = tauMin + 1; tau < tauMax; ++tau)
        {
            const double v = dn[(size_t) tau];
            if (v >= 0.8 || v > dn[(size_t) tau - 1] || v > dn[(size_t) tau + 1]) continue;
            double tb = tau;
            const double a = dn[(size_t) tau - 1], c = dn[(size_t) tau + 1];
            const double den = a - 2.0 * v + c;
            if (std::abs (den) > 1.0e-12) tb = tau + 0.5 * (a - c) / den;
            dips.push_back ({ tb, v });
        }

        static const auto weights = []
        {
            std::array<double, 40> w {};   // beta(2, 11.3): mean threshold ~0.15
            double sum = 0.0;
            for (size_t i = 0; i < w.size(); ++i)
            {
                const double th = 0.01 + 0.015 * (double) i;
                w[i] = th * std::pow (1.0 - th, 7.0);   // beta(2, 8): mean threshold ~0.2 (breathy voices stay voiced)
                sum += w[i];
            }
            for (auto& x : w) x /= sum;
            return w;
        }();

        std::vector<double> prob (dips.size(), 0.0);
        for (size_t i = 0; i < weights.size(); ++i)
        {
            const double th = 0.01 + 0.015 * (double) i;
            for (size_t d = 0; d < dips.size(); ++d)
                if (dips[d].v < th) { prob[d] += weights[i]; break; }
        }

        auto& cands = candidates[(size_t) fi];
        double voicedProb = 0.0;
        for (size_t d = 0; d < dips.size(); ++d)
        {
            const double f0 = fs / dips[d].tauF;
            if (prob[d] <= 1.0e-6 || f0 <= 60.0 || f0 >= 1150.0) continue;
            cands.push_back ({ (float) (69.0 + 12.0 * std::log2 (f0 / 440.0)), (float) -std::log (prob[d]) });
            voicedProb += prob[d];
        }
        std::sort (cands.begin(), cands.end(), [] (const Cand& x, const Cand& y2) { return x.cost < y2.cost; });
        if (cands.size() > 6) cands.resize (6);
        uvCostOf[(size_t) fi] = (float) -std::log (juce::jlimit (0.02, 0.98, 1.0 - voicedProb));
        frameVoicedProb[(size_t) fi] = (float) voicedProb;
        aperiodicity[(size_t) fi] = dips.empty() ? 1.0f : (float) dips.front().v;
    }

    // ---- Viterbi: the most coherent pitch path over time ------------------
    // A candidate costs its aperiodicity; jumping costs in proportion to the
    // interval (an octave jump ~0.5); unvoiced costs a constant. This removes
    // octave errors and keeps notes in one piece.
    {
        const float peakRms = t.rms.empty() ? 0.0f : *std::max_element (t.rms.begin(), t.rms.end());
        const float gate = peakRms * 0.02f;
        constexpr float vuvSwitch = 2.0f, perSemitone = 0.35f;   // -log probabilities
        // Loud frames are much more likely to be singing than silence/noise: an
        // unvoiced decision there must be earned (breathy singing stays voiced).
        for (int f = 0; f < frames; ++f)
        {
            const float loud = peakRms > 0.0f ? t.rms[(size_t) f] / peakRms : 0.0f;
            if (loud > 0.08f && ! candidates[(size_t) f].empty())
                uvCostOf[(size_t) f] = (float) -std::log (juce::jlimit (0.02, 0.98, (1.0 - frameVoicedProb[(size_t) f]) * 0.25));
        }
        const int nf = frames;
        std::vector<std::vector<float>> acc ((size_t) nf);
        std::vector<std::vector<int>> from ((size_t) nf);

        auto stateCount = [&] (int f) { return (int) candidates[(size_t) f].size() + 1; };   // last state = unvoiced
        for (int f = 0; f < nf; ++f)
        {
            const int ns = stateCount (f);
            acc[(size_t) f].assign ((size_t) ns, 1.0e9f);
            from[(size_t) f].assign ((size_t) ns, -1);
            const bool loud = t.rms[(size_t) f] >= gate;

            for (int sidx = 0; sidx < ns; ++sidx)
            {
                const bool uv = sidx == ns - 1;
                if (! uv && ! loud) continue;
                const float local = uv ? (loud ? uvCostOf[(size_t) f] : 0.0f) : candidates[(size_t) f][(size_t) sidx].cost;
                if (f == 0) { acc[0][(size_t) sidx] = local; continue; }

                const int np = stateCount (f - 1);
                for (int p = 0; p < np; ++p)
                {
                    const float prev = acc[(size_t) f - 1][(size_t) p];
                    if (prev >= 1.0e8f) continue;
                    const bool puv = p == np - 1;
                    float trans = 0.0f;
                    if (uv != puv) trans = vuvSwitch;
                    else if (! uv)
                    {
                        const float d = std::abs (candidates[(size_t) f][(size_t) sidx].midi - candidates[(size_t) f - 1][(size_t) p].midi);
                        trans = perSemitone * d;
                    }
                    const float total = prev + trans + local;
                    if (total < acc[(size_t) f][(size_t) sidx]) { acc[(size_t) f][(size_t) sidx] = total; from[(size_t) f][(size_t) sidx] = p; }
                }
            }
        }

        if (nf > 0)
        {
            int sidx = 0;
            for (int k = 1; k < stateCount (nf - 1); ++k)
                if (acc[(size_t) nf - 1][(size_t) k] < acc[(size_t) nf - 1][(size_t) sidx]) sidx = k;
            for (int f = nf - 1; f >= 0; --f)
            {
                const int ns = stateCount (f);
                t.midi[(size_t) f] = (sidx >= 0 && sidx < ns - 1) ? candidates[(size_t) f][(size_t) sidx].midi : 0.0f;
                sidx = from[(size_t) f][(size_t) juce::jlimit (0, ns - 1, sidx)];
            }
        }
    }

    // Vocal fry / period doubling: a short stretch exactly an octave below (or above)
    // the note it is attached to is the same note, not a new one.
    {
        const int nf = frames;
        struct Seg { int a, b; float med; };
        for (int i = 0; i < nf;)
        {
            if (t.midi[(size_t) i] <= 0.0f) { ++i; continue; }
            int e = i;
            while (e < nf && t.midi[(size_t) e] > 0.0f) ++e;

            std::vector<Seg> segs;   // pieces of this voiced run split at big jumps
            int sa = i;
            for (int k = i + 1; k <= e; ++k)
                if (k == e || std::abs (t.midi[(size_t) k] - t.midi[(size_t) k - 1]) > 6.0f)
                {
                    std::vector<float> v (t.midi.begin() + sa, t.midi.begin() + k);
                    segs.push_back ({ sa, k, medianOf (v) });
                    sa = k;
                }
            for (size_t q = 0; q < segs.size(); ++q)
            {
                auto& sg = segs[q];
                if ((sg.b - sg.a) * t.hop > 0.5) continue;
                // either neighbour that is longer and exactly an octave away (the note it belongs to)
                for (int side : { -1, +1 })
                {
                    const long qi = (long) q + side;
                    if (qi < 0 || qi >= (long) segs.size()) continue;
                    const auto& ref = segs[(size_t) qi];
                    if (ref.b - ref.a <= sg.b - sg.a) continue;
                    const float d = ref.med - sg.med;
                    if (std::abs (std::abs (d) - 12.0f) < 1.2f)
                    {
                        const float lift = d > 0.0f ? 12.0f : -12.0f;
                        for (int k = sg.a; k < sg.b; ++k) t.midi[(size_t) k] += lift;
                        sg.med += lift;
                        break;
                    }
                }
            }
            i = e;
        }
    }

    // Energy gate (-34 dB from the loudest frame).
    const float peak = t.rms.empty() ? 0.0f : *std::max_element (t.rms.begin(), t.rms.end());
    for (size_t i = 0; i < t.midi.size(); ++i)
        if (t.rms[i] < peak * 0.02f)
            t.midi[i] = 0.0f;

    auto removeShortIslands = [&t] (int minFrames)
    {
        const int n = (int) t.midi.size();
        for (int i = 0; i < n;)
        {
            if (t.midi[(size_t) i] <= 0.0f) { ++i; continue; }
            int e = i;
            while (e < n && t.midi[(size_t) e] > 0.0f) ++e;
            if (e - i < minFrames)
                for (int k = i; k < e; ++k) t.midi[(size_t) k] = 0.0f;
            i = e;
        }
    };
    removeShortIslands (4);

    // Median smoothing and octave-error repair.
    const int n = (int) t.midi.size();
    auto voicedMedian = [&t, n] (int centre, int radius)
    {
        std::vector<float> v;
        for (int k = std::max (0, centre - radius); k <= std::min (n - 1, centre + radius); ++k)
            if (t.midi[(size_t) k] > 0.0f) v.push_back (t.midi[(size_t) k]);
        return medianOf (std::move (v));
    };

    {
        std::vector<float> fixedOct (t.midi);
        for (int i = 0; i < n; ++i)
        {
            const float v = t.midi[(size_t) i];
            if (v <= 0.0f) continue;
            const float med = voicedMedian (i, 12);
            if (std::abs (v - med) > 9.0f)
                fixedOct[(size_t) i] = v + 12.0f * std::round ((med - v) / 12.0f);
        }
        t.midi = fixedOct;

        std::vector<float> smoothed (t.midi);
        for (int i = 0; i < n; ++i)
            if (t.midi[(size_t) i] > 0.0f)
                smoothed[(size_t) i] = voicedMedian (i, 2);
        t.midi = smoothed;
    }
    removeShortIslands (6);

    // Breathy moments: a short loss of pitch inside a sung phrase (<= 60 ms,
    // similar pitch on both sides) is filled, so the correction does not switch
    // on and off inside the note.
    for (int i = 1; i < n;)
    {
        if (t.midi[(size_t) i] > 0.0f || t.midi[(size_t) i - 1] <= 0.0f) { ++i; continue; }
        int e = i;
        while (e < n && t.midi[(size_t) e] <= 0.0f) ++e;
        // up to 60 ms always; up to 160 ms when the voice keeps its energy (breathy singing)
        bool loudGap = true;
        for (int k = i; k < e && loudGap; ++k)
            loudGap = t.rms[(size_t) k] > peak * 0.05f;
        if (e < n && (e - i <= 12 || (loudGap && e - i <= 32)))
        {
            const float a = t.midi[(size_t) i - 1], b = t.midi[(size_t) e];
            if (std::abs (a - b) < 2.5f)
                for (int k = i; k < e; ++k)
                    t.midi[(size_t) k] = a + (b - a) * (float) (k - i + 1) / (float) (e - i + 1);
        }
        i = e;
    }

    // Slow component: moving average within each voiced run (~150 ms).
    t.slow.assign (t.midi.size(), 0.0f);
    for (int i = 0; i < n;)
    {
        if (t.midi[(size_t) i] <= 0.0f) { ++i; continue; }
        int e = i;
        while (e < n && t.midi[(size_t) e] > 0.0f) ++e;
        for (int k = i; k < e; ++k)
        {
            double sum = 0.0;
            int cnt = 0;
            for (int j = std::max (i, k - 15); j < std::min (e, k + 16); ++j) { sum += t.midi[(size_t) j]; ++cnt; }
            t.slow[(size_t) k] = (float) (sum / cnt);
        }
        i = e;
    }

    return t;
}

//==============================================================================
std::vector<Note> segmentNotes (const PitchTrack& track, double totalSeconds)
{
    std::vector<Note> notes;
    const auto& mid = track.midi;
    const int n = (int) mid.size();
    int nextId = 1;

    auto voicedMeanRange = [&mid] (int a, int b)
    {
        double s = 0; int c = 0;
        for (int k = a; k < b; ++k) if (mid[(size_t) k] > 0.0f) { s += mid[(size_t) k]; ++c; }
        return c > 0 ? (float) (s / c) : 0.0f;
    };
    auto voicedMedianRange = [&mid] (int a, int b)
    {
        std::vector<float> v;
        for (int k = a; k < b; ++k) if (mid[(size_t) k] > 0.0f) v.push_back (mid[(size_t) k]);
        return medianOf (std::move (v));
    };

    auto addNote = [&] (int a, int b)
    {
        const double dur = (b - a) * track.hop;
        if (dur < 0.05) return;

        const int trim = (int) ((b - a) * 0.15);
        float centre = voicedMedianRange (a + trim, b - trim);
        if (centre <= 0.0f) centre = voicedMedianRange (a, b);
        if (centre <= 0.0f) return;

        Note note;
        note.id = nextId++;
        note.inStart = juce::jlimit (0.0, totalSeconds, track.frameTime (a) - track.hop * 0.5);
        note.inEnd   = juce::jlimit (0.0, totalSeconds, track.frameTime (b - 1) + track.hop * 0.5);
        note.outStart = note.inStart;
        note.outEnd = note.inEnd;
        note.origMidi = centre;
        if (note.inEnd - note.inStart > 0.03)
            notes.push_back (note);
    };

    for (int i = 0; i < n;)
    {
        if (mid[(size_t) i] <= 0.0f) { ++i; continue; }

        // A run of voice, bridging gaps of up to 3 unvoiced frames.
        int e = i;
        while (e < n)
        {
            if (mid[(size_t) e] > 0.0f) { ++e; continue; }
            int g = e;
            while (g < n && mid[(size_t) g] <= 0.0f && g - e <= 3) ++g;
            if (g < n && mid[(size_t) g] > 0.0f && g - e <= 3) e = g; else break;
        }

        int segStart = i;
        for (int k = i + 1; k < e; ++k)
        {
            if (k - segStart < 12) continue;   // notes of at least 60 ms
            const float ref  = voicedMeanRange (std::max (segStart, k - 20), k);
            const float look = voicedMedianRange (k, std::min (e, k + 10));
            if (ref <= 0.0f || look <= 0.0f || std::abs (look - ref) <= 0.7f)
                continue;

            bool sustained = e - k >= 8;
            for (int j = k; j < std::min (e, k + 8) && sustained; ++j)
                if (mid[(size_t) j] > 0.0f && std::abs (mid[(size_t) j] - look) > 0.5f)
                    sustained = false;

            if (sustained)
            {
                addNote (segStart, k);
                segStart = k;
            }
        }
        addNote (segStart, e);
        i = e;
    }

    // Join fragments of the same sung note: tiny pieces, or neighbours at the same
    // pitch with (almost) no gap. One sung note = one blob to correct.
    for (bool changed = true; changed && notes.size() > 1;)
    {
        changed = false;
        for (size_t k = 0; k + 1 < notes.size(); ++k)
        {
            auto& a = notes[k];
            auto& b = notes[k + 1];
            const double gap = b.inStart - a.inEnd;
            const float diff = std::abs (a.origMidi - b.origMidi);
            const bool samePitch = gap < 0.05 && diff < 0.6f;
            const bool tinyPiece = gap < 0.04 && diff < 1.6f && (a.inLength() < 0.12 || b.inLength() < 0.12);
            if (! samePitch && ! tinyPiece) continue;

            const double la = a.inLength(), lb = b.inLength();
            const float centre = voicedMedianRange ((int) ((a.inStart - track.offset) / track.hop),
                                                    (int) ((b.inEnd - track.offset) / track.hop));
            a.origMidi = centre > 0.0f ? centre : (float) ((a.origMidi * la + b.origMidi * lb) / (la + lb));
            a.inEnd = a.outEnd = b.inEnd;
            notes.erase (notes.begin() + (long) k + 1);
            changed = true;
            break;
        }
    }
    for (size_t k = 0; k < notes.size(); ++k) notes[k].id = (int) k + 1;

    // Keep notes from overlapping.
    for (size_t k = 1; k < notes.size(); ++k)
        if (notes[k].inStart < notes[k - 1].inEnd)
            notes[k].inStart = notes[k].outStart = notes[k - 1].inEnd;

    return notes;
}

//==============================================================================
int Marks::nearest (double p) const noexcept
{
    if (pos.empty()) return -1;
    const auto it = std::lower_bound (pos.begin(), pos.end(), p);
    if (it == pos.begin()) return 0;
    if (it == pos.end()) return (int) pos.size() - 1;
    const auto i = (int) std::distance (pos.begin(), it);
    return (pos[(size_t) i] - p) < (p - pos[(size_t) i - 1]) ? i : i - 1;
}

float Marks::protectAt (double p) const noexcept
{
    if (protect.empty() || p < 0.0) return 0.0f;
    const double b = p / protectBlock;
    const size_t i = (size_t) b;
    if (i + 1 >= protect.size()) return i < protect.size() ? protect[i] : 0.0f;
    const float f = (float) (b - (double) i);
    return protect[i] + (protect[i + 1] - protect[i]) * f;
}

/** Finds sharp attacks (plosives, mouth clicks): the curvature of the waveform
    jumps far above its level in the surrounding 10 ms. */
static std::vector<float> findTransients (const float* x, int n, double sr, const PitchTrack& track)
{
    constexpr int B = Marks::protectBlock;
    const int nb = n / B + 2;
    std::vector<float> out ((size_t) nb, 0.0f);
    if (n < 8) return out;

    std::vector<double> pre ((size_t) n + 1, 0.0);
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const double d2 = (i > 0 && i + 1 < n) ? std::abs ((double) x[i + 1] - 2.0 * x[i] + x[i - 1]) : 0.0;
        pre[(size_t) i + 1] = pre[(size_t) i] + d2;
        peak = std::max (peak, std::abs (x[i]));
    }
    const double floorLevel = 0.004 * peak;
    const int w = (int) (0.010 * sr);
    std::vector<uint8_t> hit ((size_t) nb, 0);
    for (int k = 0; k < nb; ++k)
    {
        const int a = k * B, b = std::min (n - 1, a + B);
        if (a >= n - 1) break;
        double bmax = 0.0;
        for (int i = std::max (1, a); i < b; ++i)
            bmax = std::max (bmax, std::abs ((double) x[i + 1] - 2.0 * x[i] + x[i - 1]));
        const int l0 = std::max (0, a - w), l1 = std::min (n, b + w);
        const double local = (pre[(size_t) l1] - pre[(size_t) l0]) / std::max (1, l1 - l0);
        // short burst energy against the 15 ms before it (plosive attack)
        const int s0 = a, s1 = std::min (n, a + (int) (0.001 * sr));
        const int p0 = std::max (0, a - (int) (0.015 * sr));
        const double eShort = (pre[(size_t) s1] - pre[(size_t) s0]) / std::max (1, s1 - s0);
        const double ePrev  = (pre[(size_t) a] - pre[(size_t) p0]) / std::max (1, a - p0);
        const bool click  = bmax > 10.0 * local && bmax > floorLevel;
        const bool attack = eShort > 6.0 * ePrev && eShort > 0.25 * floorLevel && a - p0 > (int) (0.005 * sr);
        // Only where the voice is starting or not pitched: inside a sung note the
        // renderer's output is not in phase with the original, and switching to it
        // would itself make a glitch.
        const double t = (double) a / sr;
        const bool inNote = track.midiAt (t - 0.025) > 0.0f && track.midiAt (t) > 0.0f;
        if ((click || attack) && ! inNote) hit[(size_t) k] = 1;
    }
    // protect 1.5 ms before to 5 ms after each hit, with 1.5 ms ramps
    const int before = std::max (1, (int) std::round (0.0015 * sr / B));
    const int after  = std::max (1, (int) std::round (0.005 * sr / B));
    const int ramp   = std::max (1, (int) std::round (0.0015 * sr / B));
    for (int k = 0; k < nb; ++k)
    {
        if (! hit[(size_t) k]) continue;
        for (int j = k - before - ramp; j <= k + after + ramp; ++j)
        {
            if (j < 0 || j >= nb) continue;
            float v = 1.0f;
            if (j < k - before)     v = 1.0f - (float) (k - before - j) / (float) (ramp + 1);
            else if (j > k + after) v = 1.0f - (float) (j - k - after) / (float) (ramp + 1);
            out[(size_t) j] = std::max (out[(size_t) j], v);
        }
    }
    return out;
}

Marks computeMarks (const float* x, int n, double sr, const PitchTrack& track)
{
    Marks mk;
    if (x == nullptr || n <= 0) return mk;
    mk.protect = findTransients (x, n, sr, track);

    // Low-passed copy to find the glottal pulses reliably.
    std::vector<float> lp (x, x + n);
    {
        juce::IIRFilter f;
        f.setCoefficients (juce::IIRCoefficients::makeLowPass (sr, 900.0));
        f.processSamples (lp.data(), n);
    }

    auto argMax = [&lp, n] (double from, double to)
    {
        int a = std::max (0, (int) std::floor (from));
        int b = std::min (n - 1, (int) std::ceil (to));
        int best = a;
        for (int i = a + 1; i <= b; ++i)
            if (lp[(size_t) i] > lp[(size_t) best]) best = i;
        return (double) best;
    };

    const double uvT = 0.005 * sr;
    double pos = 0.0;
    bool prevVoiced = false;

    mk.pos.reserve ((size_t) (n / 100));
    while (pos < n)
    {
        const float m = track.midiAt (pos / sr);
        if (m > 0.0f)
        {
            const double f0 = 440.0 * std::pow (2.0, (m - 69.0) / 12.0);
            const double T = sr / f0;
            if (! prevVoiced)
                pos = argMax (pos, pos + T);

            mk.pos.push_back (pos);
            mk.period.push_back ((float) T);
            mk.voiced.push_back (1);

            // Next mark: one period later, on a peak of the waveform. When there are
            // several candidate peaks (real voices: the glottal peak competes with a
            // formant peak), take the one whose cycle best matches this cycle
            // (cross-correlation), not simply the highest: the marks stay on the same
            // point of every cycle, which keeps the corrected voice smooth.
            double next = pos + T;
            if (track.midiAt (next / sr) > 0.0f)
            {
                const int half = std::max (4, (int) (T * 0.5));
                const int stride = std::max (1, half / 48);
                auto corr = [&] (int c)
                {
                    const int ip = (int) pos;
                    if (ip - half < 0 || c - half < 0 || c + half >= n) return -2.0;
                    double xy = 0, xx = 0, yy = 0;
                    for (int j2 = -half; j2 <= half; j2 += stride)
                    {
                        const double a = lp[(size_t) (ip + j2)], b = lp[(size_t) (c + j2)];
                        xy += a * b; xx += a * a; yy += b * b;
                    }
                    return (xx > 1.0e-12 && yy > 1.0e-12) ? xy / std::sqrt (xx * yy) : -2.0;
                };
                const int a = std::max (1, (int) std::floor (next - 0.2 * T));
                const int b = std::min (n - 2, (int) std::ceil (next + 0.2 * T));
                double peakMax = -1.0e9;
                for (int k2 = a; k2 <= b; ++k2) peakMax = std::max (peakMax, (double) lp[(size_t) k2]);
                int bestPeak = -1;
                double bestScore = -1.0e9;
                for (int k2 = a; k2 <= b; ++k2)
                {
                    const float v = lp[(size_t) k2];
                    if (v < lp[(size_t) k2 - 1] || v < lp[(size_t) k2 + 1]) continue;   // local peak
                    if (v < 0.5 * peakMax) continue;                                    // a real candidate
                    const double score = corr (k2) - 0.3 * std::abs (k2 - next) / T;    // match, then closeness
                    if (score > bestScore) { bestScore = score; bestPeak = k2; }
                }
                // Phase-locked: follow the pitch track exactly (one period per mark), and
                // let the waveform pull the marks only gently. A peak that suddenly moves
                // (vowel change, consonant, room echo) cannot make the pitch jump.
                const double found = bestPeak >= 0 ? (double) bestPeak : argMax (next - 0.2 * T, next + 0.2 * T);
                next += 0.25 * (found - next);
            }
            pos = std::max (next, pos + 0.5 * T);
            prevVoiced = true;
        }
        else
        {
            mk.pos.push_back (pos);
            mk.period.push_back ((float) uvT);
            mk.voiced.push_back (0);
            pos += uvT;
            prevVoiced = false;
        }
    }

    // Steady the marks: real voices (room, breath) make the chosen peak jitter by a
    // few samples from cycle to cycle. Each jitter becomes a tiny pitch wobble in the
    // corrected voice, so inside a voiced run every mark is pulled towards the middle
    // of its neighbours (the glottal rhythm is regular, the noise is not).
    for (int pass = 0; pass < 3; ++pass)
    {
        std::vector<double> p2 (mk.pos);
        for (size_t i = 1; i + 1 < mk.pos.size(); ++i)
        {
            if (! (mk.voiced[i - 1] && mk.voiced[i] && mk.voiced[i + 1])) continue;
            const double T = mk.period[i];
            const double d1 = mk.pos[i] - mk.pos[i - 1], d2 = mk.pos[i + 1] - mk.pos[i];
            if (d1 < 0.7 * T || d1 > 1.4 * T || d2 < 0.7 * T || d2 > 1.4 * T) continue;
            p2[i] = 0.5 * mk.pos[i] + 0.25 * (mk.pos[i - 1] + mk.pos[i + 1]);
        }
        mk.pos.swap (p2);
    }
    return mk;
}

} // namespace sp
