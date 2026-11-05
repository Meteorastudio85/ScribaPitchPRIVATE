// Sound quality test on a realistic voice: low male and higher female voices
// made from glottal pulses through formant resonators, with jitter, shimmer,
// breath noise and vibrato. Each note is shifted by -3..+3 semitones and the
// result is compared with the input:
//   - harmonics-to-noise ratio (HNR): how "clean / periodic" the voice is
//     (a "gracchiato" / rough result loses HNR)
//   - clicks: sudden jumps in the waveform that the input does not have
//
//   ScribaPitchQualityTest [folder for WAVs]

#include "Engine/Renderer.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>

using namespace sp;

struct Voice { const char* name; std::vector<float> melody; double breath; double jitter; bool hard = false; bool legato = false; bool rough = false; };

// Two-pole resonator
struct Reson
{
    double a1 = 0, a2 = 0, g = 0, y1 = 0, y2 = 0;
    void set (double f, double bw, double sr)
    {
        const double r = std::exp (-juce::MathConstants<double>::pi * bw / sr);
        a1 = 2.0 * r * std::cos (2.0 * juce::MathConstants<double>::pi * f / sr);
        a2 = -r * r;
        g = 1.0 - r;
    }
    double process (double x) { const double y = g * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
};

static std::vector<float> sing (const Voice& v, double sr, std::vector<std::pair<double, double>>& noteTimes)
{
    std::vector<float> out ((size_t) (0.4 * sr), 0.0f);
    juce::Random rng (99);
    // formants of an "a" vowel (male scale; female voices are a bit higher)
    const bool female = v.melody.front() > 64.0f;
    const double fs[] = { female ? 850.0 : 700.0, female ? 1250.0 : 1150.0, female ? 2900.0 : 2500.0, 3500.0 };
    const double bws[] = { 90.0, 110.0, 160.0, 250.0 };
    Reson res[4];
    for (int i = 0; i < 4; ++i) res[i].set (fs[i], bws[i], sr);

    double phase = 0.0;
    double lp = 0.0;
    int periodCount = 0;
    for (size_t k = 0; k < v.melody.size(); ++k)
    {
        const double dur = 0.55 + 0.15 * (double) (k % 3);
        const int len = (int) (dur * sr);
        const double start = (double) out.size() / sr;
        double jitterNow = 0.0, shimmerNow = 1.0;
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            double m = v.melody[k] - 0.4 * std::exp (-t / 0.05) + 0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.2 * t) * std::min (1.0, t / 0.25);
            const double f0 = 440.0 * std::pow (2.0, (m - 69.0) / 12.0) * (1.0 + jitterNow);
            phase += f0 / sr;
            if (v.rough && (i & 63) == 0)   // a strong first formant right on the 2nd harmonic: two peaks per period
                res[0].set (2.0 * 440.0 * std::pow (2.0, (v.melody[k] - 69.0) / 12.0), 60.0, sr);
            if (v.hard)   // vowel changes inside the note: "a" -> "e" -> "o"
            {
                const double u = t / dur;
                const double f1 = (female ? 850.0 : 700.0) * (1.0 - 0.35 * std::sin (juce::MathConstants<double>::pi * u));
                const double f2 = (female ? 1250.0 : 1150.0) * (1.0 + 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * u));
                if ((i & 63) == 0) { res[0].set (f1, bws[0], sr); res[1].set (f2, bws[1], sr); }
            }
            if (phase >= 1.0)
            {
                phase -= 1.0;
                ++periodCount;
                jitterNow = (rng.nextDouble() - 0.5) * 2.0 * v.jitter;
                shimmerNow = 1.0 + (rng.nextDouble() - 0.5) * 0.1;
            }
            // Rosenberg-like glottal flow derivative
            double g;
            static double opVar = 0.6;
            if (phase < f0 / sr) opVar = v.rough ? 0.45 + 0.25 * rng.nextDouble() : 0.6;   // glottal shape changes every period
            const double op = opVar, cl = 0.15;
            if (phase < op)            g = std::sin (juce::MathConstants<double>::pi * phase / op);
            else if (phase < op + cl)  g = -std::sin (juce::MathConstants<double>::pi * 0.5 * (phase - op) / cl) * 3.0;
            else                       g = 0.0;
            g *= shimmerNow;
            if (v.rough)   // breathy moments: the voice nearly disappears for ~30 ms inside the note
            {
                const double dip = std::fmod (t + 0.37 * (double) k, 0.45);
                if (dip < 0.03) g *= 0.12;
            }
            if (v.hard && t > dur - 0.18 && (periodCount & 1)) g *= 0.45;   // creaky end of the note (vocal fry)
            if (v.hard && t < 0.05) g = g * (t / 0.05);                      // consonant: noise burst first
            const double burst = (v.hard && t < 0.06) ? (rng.nextDouble() - 0.5) * 0.8 * (1.0 - t / 0.06) : 0.0;
            const double noise = (rng.nextDouble() - 0.5) * v.breath * (0.5 + 0.5 * (phase < op ? 1.0 : 0.3));
            const double x = g + noise + burst;
            double y = 0.0;
            for (int r = 0; r < 4; ++r) y += res[r].process (x) * (r == 0 ? 1.0 : r == 1 ? 0.7 : r == 2 ? 0.35 : 0.2);
            lp = lp * 0.2 + y * 0.8;
            const double env = v.legato ? std::min (1.0, std::min (k % 4 == 0 ? t / 0.03 : 1.0, k % 4 == 3 ? (dur - t) / 0.05 : 1.0))
                                        : std::min (1.0, std::min (t / 0.03, (dur - t) / 0.05));
            out.push_back ((float) (0.9 * lp * env));
        }
        noteTimes.push_back ({ start, start + dur });
        if (v.legato && (k % 4) != 3) continue;          // legato: the voice goes straight into the next note
        for (int i = 0; i < (int) (0.08 * sr); ++i)   // breath between notes
            out.push_back ((float) ((rng.nextDouble() - 0.5) * 0.004));
    }
    out.resize (out.size() + (size_t) (0.4 * sr), 0.0f);
    if (v.hard)   // small room: a few comb filters mixed in
    {
        std::vector<float> wet (out.size(), 0.0f);
        const int d[] = { 1557, 1617, 1491, 1422 };
        for (int k = 0; k < 4; ++k)
        {
            std::vector<float> buf ((size_t) d[k], 0.0f);
            size_t p = 0;
            for (size_t i = 0; i < out.size(); ++i)
            {
                const float y = buf[p];
                buf[p] = out[i] + y * 0.72f;
                p = (p + 1) % buf.size();
                wet[i] += y * 0.12f;
            }
        }
        for (size_t i = 0; i < out.size(); ++i) out[i] += wet[i];
    }
    float peak = 0.0f;
    for (auto s : out) peak = std::max (peak, std::abs (s));
    for (auto& s : out) s *= 0.5f / peak;
    return out;
}

// HNR (dB) over [a, b] using normalised autocorrelation at the best lag near the expected period
static double hnr (const std::vector<float>& x, double sr, double a, double b, double f0)
{
    const int win = (int) (0.04 * sr), hop = win / 2;
    const int T = (int) std::round (sr / f0);
    double sum = 0.0; int count = 0;
    for (int s = (int) (a * sr); s + win + T + 8 < (int) (b * sr); s += hop)
    {
        double best = 0.0;
        for (int lag = (int) (T * 0.9); lag <= (int) (T * 1.1); ++lag)
        {
            double xy = 0, xx = 0, yy = 0;
            for (int i = 0; i < win; ++i)
            {
                const double p = x[(size_t) (s + i)], q = x[(size_t) (s + i + lag)];
                xy += p * q; xx += p * p; yy += q * q;
            }
            if (xx > 1e-12 && yy > 1e-12) best = std::max (best, xy / std::sqrt (xx * yy));
        }
        best = juce::jlimit (0.001, 0.9999, best);
        sum += 10.0 * std::log10 (best / (1.0 - best));
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}

// pitch roughness: cents of fast pitch wobble left after removing the slow melody (5 ms frames)
static int gGrossFrames = 0, gFrames = 0;   // tracker octave / gross errors on the output, counted apart
static double wobbleCents (const std::vector<float>& x, double sr, double a, double b)
{
    const int i0 = (int) (a * sr), i1 = (int) (b * sr);
    if (i1 - i0 < (int) (0.1 * sr)) return 0.0;
    const auto tr = trackPitch (x.data() + i0, i1 - i0, sr);
    std::vector<float> v;
    for (auto m : tr.midi) if (m > 0.0f) v.push_back (m);
    if (v.size() < 12) return 0.0;
    // Gross jumps (> 3 st from the note's median: the measuring tracker jumping an
    // octave on fry / breath) are not wobble; they are counted separately.
    std::vector<float> sorted (v);
    std::nth_element (sorted.begin(), sorted.begin() + (long) sorted.size() / 2, sorted.end());
    const float med = sorted[sorted.size() / 2];
    std::vector<float> w;
    for (auto m : v) { ++gFrames; if (std::abs (m - med) > 3.0f) ++gGrossFrames; else w.push_back (m); }
    if (w.size() < 12) return 0.0;
    double sum = 0.0; int n = 0;
    for (size_t i = 3; i + 3 < w.size(); ++i)
    {
        const double avg = (w[i - 3] + w[i - 2] + w[i - 1] + w[i + 1] + w[i + 2] + w[i + 3]) / 6.0;
        sum += (w[i] - avg) * (w[i] - avg); ++n;
    }
    return 100.0 * std::sqrt (sum / n);
}

// clicks: samples where the 2nd difference jumps far above its local level
static int clicks (const std::vector<float>& x, double sr, double a, double b)
{
    const int i0 = (int) (a * sr) + 2, i1 = (int) (b * sr) - 2;
    std::vector<double> d2;
    for (int i = i0; i < i1; ++i) d2.push_back (std::abs ((double) x[(size_t) i + 1] - 2.0 * x[(size_t) i] + x[(size_t) i - 1]));
    int n = 0;
    const int w = (int) (0.01 * sr);
    for (size_t i = (size_t) w; i + (size_t) w < d2.size(); i += 8)
    {
        double local = 0.0;
        for (size_t j = i - (size_t) w; j < i + (size_t) w; j += 4) local += d2[j];
        local /= (double) (w / 2);
        if (d2[i] > 12.0 * local + 1e-5) ++n;
    }
    return n;
}

static void writeWav (const juce::File& f, const std::vector<float>& x, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 1, 24, {}, 0));
    const float* ch[] = { x.data() };
    w->writeFromFloatArrays (ch, 1, (int) x.size());
}

int main (int argc, char** argv)
{
    const double sr = 48000.0;
    const juce::File dir = argc > 1 ? juce::File { juce::String (argv[1]) } : juce::File();
    if (dir != juce::File()) dir.createDirectory();

    const Voice voices[] = {
        { "male_low",   { 45, 47, 48, 50, 52, 50, 48, 47 }, 0.25, 0.010 },   // A2..E3, breathy, rough
        { "male_mid",   { 52, 55, 57, 59, 60, 57, 55, 52 }, 0.12, 0.006 },   // E3..C4
        { "female",     { 64, 67, 69, 71, 72, 69, 67, 64 }, 0.15, 0.005 },   // E4..C5
        { "male_room",  { 47, 50, 52, 50, 48, 45, 47, 50 }, 0.20, 0.012, true },  // vowels, consonants, fry, room
        { "female_room",{ 67, 69, 71, 74, 72, 71, 69, 67 }, 0.15, 0.006, true },
        { "rough_male", { 50, 52, 53, 55, 53, 52, 50, 48 }, 0.30, 0.015, false, true, true },   // unstable waveform, breathy holes
        { "rough_fem",  { 65, 67, 69, 70, 69, 67, 65, 64 }, 0.25, 0.012, false, true, true },
    };
    const float shifts[] = { -3.0f, -1.0f, +1.0f, +3.0f };

    double worstLoss = 0.0, worstWobble = 0.0;
    int worstClicks = 0;
    for (const auto& v : voices)
    {
        std::vector<std::pair<double, double>> times;
        const auto x = sing (v, sr, times);
        const int n = (int) x.size();
        const auto track = trackPitch (x.data(), n, sr);
        auto notes = segmentNotes (track, n / sr);
        const auto marks = computeMarks (x.data(), n, sr, track);
        const RenderSource src { x.data(), n, sr, &marks, &track };
        {
            // how well the analysis follows this voice
            int frames = 0, wrong = 0, holes = 0;
            for (size_t k = 0; k < times.size(); ++k)
            {
                const double a = times[k].first + 0.1, b = times[k].second - 0.1;
                for (size_t i = 0; i < track.midi.size(); ++i)
                {
                    const double t = track.frameTime ((int) i);
                    if (t < a || t > b) continue;
                    ++frames;
                    if (track.midi[i] <= 0.0f) ++holes;
                    else if (std::abs (track.midi[i] - v.melody[k]) > 1.0f) ++wrong;
                }
            }
            if (const char* dump = std::getenv ("SP_DUMP"))
                if (juce::String (dump) == v.name)
                    for (size_t k = 0; k < times.size(); ++k)
                    {
                        juce::String line;
                        for (size_t i = 0; i < track.midi.size(); i += 6)
                        {
                            const double t = track.frameTime ((int) i);
                            if (t < times[k].first || t > times[k].second) continue;
                            line << juce::String (track.midi[i] - v.melody[k], 1) << " ";
                        }
                        std::printf ("  note %d (%.0f): %s\n", (int) k, (double) v.melody[k], line.toRawUTF8());
                    }
            std::printf ("%-11s analysis: %d notes found (sung %d), %.1f%% frames wrong pitch, %.1f%% frames lost inside notes\n",
                         v.name, (int) notes.size(), (int) v.melody.size(), 100.0 * wrong / frames, 100.0 * holes / frames);
        }

        for (int mode = 0; mode < 6; ++mode)
        {
            const float sh = mode < 4 ? shifts[mode] : 1.0f;
            auto edited = notes;
            for (auto& note : edited)
            {
                note.shift = sh;
                if (mode == 4) { note.centerAmt = 1.0f; note.driftAmt = 0.4f; note.vibAmt = 0.35f; }   // automatic correction, no vibrato
                if (mode == 5) { note.outEnd = note.outStart + note.inLength() * 0.92; }               // a bit shorter (stretch)
            }
            std::vector<float> y (x);
            renderRange (src, edited, 0, n, y.data(), n);

            double inH = 0, outH = 0, inW = 0, outW = 0; int inC = 0, outC = 0, cnt = 0;
            for (size_t k = 0; k < times.size(); ++k)
            {
                const double a = times[k].first + 0.12, b = times[k].second - 0.08;
                const double f0in = 440.0 * std::pow (2.0, (v.melody[k] - 69.0) / 12.0);
                if (mode == 5) { const double b2 = times[k].first + (times[k].second - times[k].first) * 0.92 - 0.08; inH += hnr (x, sr, a, b, f0in); outH += hnr (y, sr, a, b2, f0in * std::pow (2.0, sh / 12.0)); inC += clicks (x, sr, a, b); outC += clicks (y, sr, a, b2); ++cnt; continue; }
                inH  += hnr (x, sr, a, b, f0in);
                outH += hnr (y, sr, a, b, f0in * std::pow (2.0, sh / 12.0));
                inW  += wobbleCents (x, sr, a, b);
                outW += wobbleCents (y, sr, a, b);
                inC  += clicks (x, sr, a, b);
                outC += clicks (y, sr, a, b);
                ++cnt;
            }
            inH /= cnt; outH /= cnt; inW /= cnt; outW /= cnt;
            const char* label = mode < 4 ? "shift" : mode == 4 ? "autocorr" : "stretch";
            std::printf ("%-11s %-8s %+.0f st: HNR in %5.1f out %5.1f (loss %4.1f dB)  wobble in %4.1f out %4.1f ct  clicks %d/%d\n",
                         v.name, label, (double) sh, inH, outH, inH - outH, inW, outW, inC, outC);
            worstWobble = std::max (worstWobble, outW - inW);
            worstLoss = std::max (worstLoss, inH - outH);
            worstClicks = std::max (worstClicks, outC - inC);
            if (dir != juce::File())
                writeWav (dir.getChildFile (juce::String (v.name) + "_" + juce::String (label) + juce::String ((int) sh) + ".wav"), y, sr);
        }
        if (dir != juce::File())
            writeWav (dir.getChildFile (juce::String (v.name) + "_orig.wav"), x, sr);
    }
    // ---- dragging a note in a legato phrase: many small renders, as the editor does
    double worstSeam = 0.0;
    for (const Voice& v : { Voice { "legato_male", { 48, 50, 52, 53, 55, 53, 52, 50 }, 0.15, 0.008, false, true },
                            Voice { "legato_fem",  { 69, 71, 72, 74, 76, 74, 72, 71 }, 0.12, 0.006, false, true } })
    {
        std::vector<std::pair<double, double>> times;
        const auto x = sing (v, sr, times);
        const int n = (int) x.size();
        const auto track = trackPitch (x.data(), n, sr);
        auto notes = segmentNotes (track, n / sr);
        const auto marks = computeMarks (x.data(), n, sr, track);
        const RenderSource src { x.data(), n, sr, &marks, &track };

        // all notes corrected first (whole song), then note 2 is dragged up in 12 steps
        for (auto& note : notes) { note.centerAmt = 1.0f; note.shift = std::round (note.origMidi) - note.origMidi + note.origMidi - note.origMidi; }
        std::vector<float> y (x);
        renderRange (src, notes, 0, n, y.data(), n);
        const int dragged = juce::jmin (2, (int) notes.size() - 1);
        for (int step = 1; step <= 12; ++step)
        {
            auto before = notes;
            notes[(size_t) dragged].shift = 2.0f * (float) step / 12.0f;
            double f, t;
            if (changedRange (before, notes, n / sr, f, t))
                renderRange (src, notes, (juce::int64) (f * sr), (juce::int64) (t * sr), y.data(), n);
        }
        std::vector<float> full (x);
        renderRange (src, notes, 0, n, full.data(), n);

        double hInc = 0, hFull = 0; int c = 0, clkInc = 0, clkFull = 0;
        for (size_t k = 0; k < notes.size(); ++k)
        {
            const double a = notes[k].outStart + 0.05, b = notes[k].outEnd - 0.05;
            if (b - a < 0.1) continue;
            const double f0 = 440.0 * std::pow (2.0, (notes[k].newCenter() - 69.0) / 12.0);
            hInc += hnr (y, sr, a, b, f0); hFull += hnr (full, sr, a, b, f0);
            clkInc += clicks (y, sr, a, b); clkFull += clicks (full, sr, a, b);
            ++c;
        }
        hInc /= c; hFull /= c;
        std::printf ("%-11s drag 12 steps: HNR after dragging %5.1f dB vs one clean render %5.1f dB (loss %4.1f dB), clicks %d vs %d\n",
                     v.name, hInc, hFull, hFull - hInc, clkInc, clkFull);
        worstSeam = std::max (worstSeam, hFull - hInc);
        worstClicks = std::max (worstClicks, clkInc - clkFull);
        if (dir != juce::File())
        {
            writeWav (dir.getChildFile (juce::String (v.name) + "_after_drag.wav"), y, sr);
            writeWav (dir.getChildFile (juce::String (v.name) + "_clean.wav"), full, sr);
        }
    }
    worstLoss = std::max (worstLoss, worstSeam);

    std::printf ("\nworst HNR loss %.1f dB, worst extra pitch wobble %.1f cents, worst extra clicks %d\n", worstLoss, worstWobble, worstClicks);
    const double gross = 100.0 * gGrossFrames / std::max (1, gFrames);
    std::printf ("pitch-tracker gross jumps (fry / breath, input + output): %.2f%% of frames\n", gross);
    const bool ok = worstLoss < 3.0 && worstClicks < 10 && worstWobble < 4.0 && gross < 1.0;
    std::printf (ok ? "QUALITY OK\n" : "QUALITY NOT GOOD ENOUGH\n");
    return ok ? 0 : 1;
}
