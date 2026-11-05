// Engine test: synthesises a sung melody in A minor with out-of-tune and
// out-of-key notes, analyses it, corrects it automatically, moves/stretches a
// note and checks the rendered result with the same pitch tracker.

#include "Engine/Renderer.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>

using namespace sp;

struct SungNote { float midi; float cents; double dur; bool vibrato; };

static std::vector<float> synthVoice (const std::vector<SungNote>& melody, double sr, double gap)
{
    std::vector<float> out;
    juce::Random rng (42);
    double phase = 0.0;

    auto formantGain = [] (double f)
    {
        auto peak = [f] (double c, double bw, double g) { const double d = (f - c) / bw; return g * std::exp (-0.5 * d * d); };
        return 0.08 + peak (700, 130, 1.0) + peak (1150, 160, 0.6) + peak (2600, 250, 0.25);
    };

    out.resize ((size_t) (0.3 * sr), 0.0f);   // leading silence
    for (const auto& n : melody)
    {
        const int len = (int) (n.dur * sr);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            double m = n.midi + n.cents / 100.0;
            m += 0.35 * std::exp (-t / 0.04) * -1.0;                  // small scoop at the attack
            if (n.vibrato) m += 0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * t) * std::min (1.0, t / 0.15);
            const double f0 = 440.0 * std::pow (2.0, (m - 69.0) / 12.0);
            phase += f0 / sr;
            double s = 0.0;
            for (int k = 1; k * f0 < 7000.0; ++k)
                s += formantGain (k * f0) / std::sqrt ((double) k) * std::sin (2.0 * juce::MathConstants<double>::pi * k * phase);
            const double env = std::min (1.0, std::min (t / 0.02, (n.dur - t) / 0.03));
            out.push_back ((float) (0.25 * s * env + 0.002 * (rng.nextFloat() - 0.5f)));
        }
        const int g = (int) (gap * sr);
        for (int i = 0; i < g; ++i)   // breath noise between notes
            out.push_back (0.01f * (rng.nextFloat() - 0.5f));
    }
    out.resize (out.size() + (size_t) (0.3 * sr), 0.0f);
    return out;
}

static float measure (const float* x, int n, double sr, double fromSec, double toSec)
{
    const auto tr = trackPitch (x, n, sr);
    std::vector<float> v;
    const double a = fromSec + (toSec - fromSec) * 0.2, b = toSec - (toSec - fromSec) * 0.2;
    for (size_t i = 0; i < tr.midi.size(); ++i)
    {
        const double t = tr.frameTime ((int) i);
        if (t >= a && t <= b && tr.midi[i] > 0.0f) v.push_back (tr.midi[i]);
    }
    if (v.empty()) return 0.0f;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

static void writeWav (const juce::File& f, const std::vector<float>& x, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 1, 24, {}, 0));
    if (w == nullptr) return;
    const float* chans[] = { x.data() };
    w->writeFromFloatArrays (chans, 1, (int) x.size());
}

int main (int argc, char** argv)
{
    const double sr = 44100.0;
    const std::vector<SungNote> melody = {
        { 69, 10, 0.55, false }, { 72, -15, 0.45, false }, { 64, 14, 0.50, false }, { 67, -16, 0.40, false },
        { 69, 12, 0.80, true },  { 66, 24, 0.40, false },  { 64, -10, 0.45, false }, { 62, 18, 0.40, false },
        { 60, -20, 0.70, true }, { 61, -22, 0.35, false }, { 62, 9, 0.40, false },  { 64, -12, 0.45, false },
        { 69, 8, 0.90, true }
    };

    const auto voice = synthVoice (melody, sr, 0.09);
    const int n = (int) voice.size();
    std::printf ("Synth voice: %.2f s\n", n / sr);

    // ---- analysis -----------------------------------------------------------
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto track = trackPitch (voice.data(), n, sr);
    auto notes = segmentNotes (track, n / sr);
    const auto key = detectKey (notes);
    const auto marks = computeMarks (voice.data(), n, sr, track);
    auto t1 = juce::Time::getMillisecondCounterHiRes();
    std::printf ("Analysis: %.1f ms, %d notes, key %s (%.0f%%), alt %s, %d marks\n",
                 t1 - t0, (int) notes.size(), keyLongName (key.key).toRawUTF8(), key.confidence * 100.0f,
                 keyShortName (key.alt).toRawUTF8(), (int) marks.pos.size());

    int failures = 0;
    {
        // marks must step one period at a time (same phase of every cycle)
        double sum = 0, mn = 1e9, mx = 0; int c = 0;
        for (size_t i = 1; i < marks.pos.size(); ++i)
            if (marks.voiced[i] && marks.voiced[i - 1])
            {
                const double r = (marks.pos[i] - marks.pos[i - 1]) / marks.period[i - 1];
                sum += r; ++c; mn = std::min (mn, r); mx = std::max (mx, r);
            }
        std::printf ("Marks: spacing / period  mean %.4f  min %.3f  max %.3f\n", sum / c, mn, mx);
    }
    if ((int) notes.size() != (int) melody.size())
    {
        std::printf ("FAIL: expected %d notes\n", (int) melody.size());
        ++failures;
    }
    for (size_t i = 0; i < notes.size() && i < melody.size(); ++i)
    {
        const float expected = melody[i].midi + melody[i].cents / 100.0f;
        const float err = (notes[i].origMidi - expected) * 100.0f;
        std::printf ("  note %2d  %-4s  detected %6.2f  expected %6.2f  (%+5.1f ct)  %.2f-%.2f s\n", (int) i,
                     noteName ((int) std::round (notes[i].origMidi)).toRawUTF8(), notes[i].origMidi, expected, err,
                     notes[i].inStart, notes[i].inEnd);
        if (std::abs (err) > 15.0f) { std::printf ("    FAIL: detection error\n"); ++failures; }
    }
    if (key.key != 21 && key.key != 0)
    {
        std::printf ("FAIL: key should be A minor (or relative C major)\n");
        ++failures;
    }

    // ---- automatic correction to A minor -------------------------------------
    const auto scale = makeScale (21, scaleOfKey);
    std::vector<float> corrected (voice);
    const RenderSource src { voice.data(), n, sr, &marks, &track };

    auto before = notes;
    std::vector<int> targets;
    for (auto& note : notes)
    {
        const int target = scale.nearest (note.origMidi);
        targets.push_back (target);
        note.moveCenterTo ((float) target, true);
        note.driftAmt = 0.5f;
    }

    double from, to;
    changedRange (before, notes, n / sr, from, to);
    t0 = juce::Time::getMillisecondCounterHiRes();
    renderRange (src, notes, (juce::int64) (from * sr), (juce::int64) (to * sr), corrected.data(), n);
    t1 = juce::Time::getMillisecondCounterHiRes();
    std::printf ("Render (whole take, %.2f s of audio): %.1f ms  -> %.0fx faster than real time\n",
                 n / sr, t1 - t0, (n / sr * 1000.0) / (t1 - t0));

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const float got = measure (corrected.data(), n, sr, notes[i].outStart, notes[i].outEnd);
        const float err = (got - (float) targets[i]) * 100.0f;
        std::printf ("  corrected %2d  target %-4s  measured %6.2f  (%+5.1f ct)\n", (int) i,
                     noteName (targets[i]).toRawUTF8(), got, err);
        if (std::abs (err) > 12.0f) { std::printf ("    FAIL: correction error\n"); ++failures; }
    }

    // ---- move note 5 up two semitones and stretch note 8 ---------------------
    before = notes;
    notes[5].moveCenterTo ((float) targets[5] + 2.0f, true);
    const double delay = 0.06;
    notes[8].outStart += delay;   // the word came late: pull its end later too (stretch)
    notes[8].outEnd += delay * 0.5;
    sortNotes (notes);
    if (changedRange (before, notes, n / sr, from, to))
    {
        t0 = juce::Time::getMillisecondCounterHiRes();
        renderRange (src, notes, (juce::int64) (from * sr), (juce::int64) (to * sr), corrected.data(), n);
        t1 = juce::Time::getMillisecondCounterHiRes();
        std::printf ("Partial render %.2f-%.2f s: %.1f ms\n", from, to, t1 - t0);
    }

    const float got5 = measure (corrected.data(), n, sr, notes[5].outStart, notes[5].outEnd);
    std::printf ("  moved note 5: target %.0f measured %.2f\n", (double) targets[5] + 2.0, got5);
    if (std::abs (got5 - (targets[5] + 2.0f)) > 0.12f) { std::printf ("    FAIL\n"); ++failures; }

    const float got8 = measure (corrected.data(), n, sr, notes[8].outStart, notes[8].outEnd);
    std::printf ("  stretched note 8 (%.2f-%.2f s): target %d measured %.2f\n", notes[8].outStart, notes[8].outEnd, targets[8], got8);
    if (std::abs (got8 - (float) targets[8]) > 0.12f) { std::printf ("    FAIL\n"); ++failures; }

    // Untouched audio must be bit-exact (first 0.25 s is before any note).
    bool exact = true;
    for (int i = 0; i < (int) (0.2 * sr); ++i)
        if (! juce::exactlyEqual (corrected[(size_t) i], voice[(size_t) i])) { exact = false; break; }
    std::printf ("  untouched audio bit-exact: %s\n", exact ? "yes" : "NO");
    if (! exact) ++failures;

    if (argc > 1)
    {
        const juce::File dir { juce::String (argv[1]) };
        dir.createDirectory();
        writeWav (dir.getChildFile ("original.wav"), voice, sr);
        writeWav (dir.getChildFile ("corrected.wav"), corrected, sr);
        std::printf ("Wrote WAVs to %s\n", dir.getFullPathName().toRawUTF8());
    }

    std::printf (failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
