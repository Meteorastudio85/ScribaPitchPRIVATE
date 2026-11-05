// Runs the Scriba Pitch engine on a real vocal file, the way the plugin does:
// analysis -> notes -> key -> AUTOMATIC CORRECTION (and a few manual edits) -> render.
// Writes the corrected files plus CSVs of the pitch curves for inspection.
//
//   ScribaPitchFileTest <vocal.wav> <outDir>

#include "Engine/Analyzer.h"
#include "Engine/Renderer.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <chrono>

using namespace sp;

static void writeWav (const juce::File& f, const std::vector<float>& x, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 1, 24, {}, 0));
    const float* ch[] = { x.data() };
    w->writeFromFloatArrays (ch, 1, (int) x.size());
}

static void writeTrack (const juce::File& f, const PitchTrack& t)
{
    juce::String s;
    for (size_t i = 0; i < t.midi.size(); ++i)
        s << juce::String (t.frameTime ((int) i), 3) << "," << juce::String (t.midi[i], 3) << "\n";
    f.replaceWithText (s);
}

// clicks: samples where the 2nd difference jumps far above its local level
static int clicks (const std::vector<float>& x, double sr, double a, double b)
{
    const int i0 = juce::jmax (2, (int) (a * sr)), i1 = juce::jmin ((int) x.size() - 2, (int) (b * sr));
    if (i1 - i0 < (int) (0.03 * sr)) return 0;
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

// fast pitch wobble (cents) of a pitch curve inside [a, b], gross jumps excluded
static double wobble (const PitchTrack& t, double a, double b)
{
    std::vector<float> v;
    for (size_t i = 0; i < t.midi.size(); ++i)
    {
        const double tt = t.frameTime ((int) i);
        if (tt >= a && tt <= b && t.midi[i] > 0.0f) v.push_back (t.midi[i]);
    }
    if (v.size() < 12) return 0.0;
    double sum = 0.0; int n = 0;
    for (size_t i = 3; i + 3 < v.size(); ++i)
    {
        const double avg = (v[i - 3] + v[i - 2] + v[i - 1] + v[i + 1] + v[i + 2] + v[i + 3]) / 6.0;
        if (std::abs (v[i] - avg) > 1.0) continue;
        sum += (v[i] - avg) * (v[i] - avg); ++n;
    }
    return n > 0 ? 100.0 * std::sqrt (sum / n) : 0.0;
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("usage: ScribaPitchFileTest <vocal.wav> <outDir>\n"); return 1; }
    const juce::File in { juce::String (argv[1]) }, dir { juce::String (argv[2]) };
    dir.createDirectory();

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (in));
    if (r == nullptr) { std::printf ("cannot read %s\n", argv[1]); return 1; }
    const double sr = r->sampleRate;
    const int n = (int) r->lengthInSamples;
    juce::AudioBuffer<float> buf ((int) r->numChannels, n);
    r->read (&buf, 0, n, 0, true, true);
    std::vector<float> x ((size_t) n, 0.0f);
    for (int c = 0; c < buf.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i) x[(size_t) i] += buf.getSample (c, i) / (float) buf.getNumChannels();
    std::printf ("%s: %.1f s, %.0f Hz, %d ch\n", in.getFileName().toRawUTF8(), n / sr, sr, (int) r->numChannels);

    const auto t0 = std::chrono::steady_clock::now();
    const auto track = trackPitch (x.data(), n, sr);
    auto notes = segmentNotes (track, n / sr);
    const auto marks = computeMarks (x.data(), n, sr, track);
    const auto key = detectKey (notes);
    const double anaSec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("analysis: %.2f s of CPU for %.1f s of vocal, %d notes, key %s (alt %s, %.0f%%)\n",
                 anaSec, n / sr, (int) notes.size(), keyLongName (key.key).toRawUTF8(),
                 keyLongName (key.alt).toRawUTF8(), key.confidence * 100.0);

    int voiced = 0;
    for (auto m : track.midi) voiced += m > 0.0f ? 1 : 0;
    std::printf ("voiced frames: %.1f%%\n", 100.0 * voiced / (double) track.midi.size());

    int scaleType = 0;
    if (argc > 3) scaleType = juce::String (argv[3]).getIntValue();
    const auto scale = makeScale (key.key, scaleType);

    juce::String list;
    int red = 0, orange = 0;
    for (const auto& note : notes)
    {
        const auto tu = tuningOf (note, scale);
        red += tu == Tuning::outOfKey ? 1 : 0;
        orange += tu == Tuning::offCents ? 1 : 0;
        const float c = note.newCenter();
        list << juce::String (note.inStart, 2) << "-" << juce::String (note.inEnd, 2) << "  " << noteName ((int) std::round (c))
             << " " << juce::String (juce::roundToInt ((c - std::round (c)) * 100.0f)) << "ct  "
             << (tu == Tuning::outOfKey ? "RED" : tu == Tuning::offCents ? "orange" : "cyan") << "\n";
    }
    dir.getChildFile ("notes.txt").replaceWithText (list);
    std::printf ("colours: %d red (out of key), %d orange (off by cents), %d cyan\n", red, orange, (int) notes.size() - red - orange);

    const RenderSource src { x.data(), n, sr, &marks, &track };
    writeTrack (dir.getChildFile ("pitch_original.csv"), track);
    writeWav (dir.getChildFile ("00_original.wav"), x, sr);

    struct Edit { const char* name; int mode; float st; };
    const Edit edits[] = { { "01_automatic_correction", 0, 0 }, { "02_all_up_1st", 1, 1 }, { "03_all_down_2st", 1, -2 },
                           { "04_auto_no_vibrato", 2, 0 } };
    for (const auto& e : edits)
    {
        auto ed = notes;
        juce::Random rng (12345);
        for (auto& note : ed)
        {
            if (e.mode == 1) { note.shift = e.st; continue; }
            const float centre = note.newCenter();
            const int target = scale.nearest (centre);
            note.shift = (float) target - std::round (note.origMidi) + (rng.nextFloat() * 2.0f - 1.0f) * 0.15f * 0.08f;
            note.centerAmt = 1.0f;
            note.driftAmt = 0.4f;
            note.vibAmt = e.mode == 2 ? 0.35f : 1.0f;
        }
        std::vector<float> y (x);
        const auto t1 = std::chrono::steady_clock::now();
        renderRange (src, ed, 0, n, y.data(), n);
        const double renSec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count();
        writeWav (dir.getChildFile (juce::String (e.name) + ".wav"), y, sr);

        const auto outTrack = trackPitch (y.data(), n, sr);
        writeTrack (dir.getChildFile (juce::String (e.name) + "_pitch.csv"), outTrack);
        double wIn = 0, wOut = 0; int cIn = 0, cOut = 0, cnt = 0;
        double worst = 0; juce::String worstAt;
        for (const auto& note : ed)
        {
            if (note.inLength() < 0.15) continue;
            const double a = note.outStart + 0.04, b = note.outEnd - 0.04;
            const double wi = wobble (track, a, b), wo = wobble (outTrack, a, b);
            wIn += wi; wOut += wo; ++cnt;
            cIn += clicks (x, sr, a, b); cOut += clicks (y, sr, a, b);
            if (wo - wi > worst) { worst = wo - wi; worstAt = juce::String (a, 2) + " s"; }
        }
        std::printf ("%-26s render %.2f s  wobble in %.1f out %.1f ct (worst note +%.1f ct at %s)  clicks %d -> %d\n",
                     e.name, renSec, wIn / cnt, wOut / cnt, worst, worstAt.toRawUTF8(), cIn, cOut);
    }
    return 0;
}
