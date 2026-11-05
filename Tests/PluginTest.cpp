// End-to-end test of the plugin without a DAW: a fake transport plays a sung
// melody through processBlock (capture), the analysis runs, AUTOMATIC
// CORRECTION is applied, the corrected playback is measured, the session is
// saved and reloaded, and the editor is rendered to PNG files.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
#include <cstring>
#include <pluginterfaces/base/funknown.h>

// A stand-in for REAPER: answers IReaperHostApplication and records the cursor moves.
namespace fakereaper
{
    double lastCursor = -1.0; bool lastSeekPlay = false; int playPresses = 0, stopPresses = 0, playState = 0;
    void setEditCurPos (double t, bool, bool seekplay) { lastCursor = t; lastSeekPlay = seekplay; }
    int  getPlayState() { return playState; }
    void onPlay() { ++playPresses; playState = 1; }
    void onStop() { ++stopPresses; playState = 0; }

    struct Api : public Steinberg::FUnknown
    {
        Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID tuid, void** obj) override
        {
            static const Steinberg::TUID want = INLINE_UID (0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C);
            if (std::memcmp (tuid, want, sizeof (Steinberg::TUID)) == 0) { *obj = this; addRef(); return Steinberg::kResultOk; }
            *obj = nullptr; return Steinberg::kNoInterface;
        }
        Steinberg::uint32 PLUGIN_API addRef() override  { return 1; }
        Steinberg::uint32 PLUGIN_API release() override { return 1; }
        virtual void* PLUGIN_API getReaperApi (const char* name)
        {
            const juce::String n (name);
            if (n == "SetEditCurPos") return (void*) &setEditCurPos;
            if (n == "GetPlayState")  return (void*) &getPlayState;
            if (n == "OnPlayButton")  return (void*) &onPlay;
            if (n == "OnStopButton")  return (void*) &onStop;
            return nullptr;
        }
        virtual void* PLUGIN_API getReaperParent (Steinberg::uint32) { return nullptr; }
        virtual void* PLUGIN_API reaperExtended (Steinberg::uint32, void*, void*, void*) { return nullptr; }
        virtual ~Api() = default;
    };
}

using namespace sp;

struct FakePlayHead : juce::AudioPlayHead
{
    juce::int64 pos = 0;
    bool playing = false;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setTimeInSamples (pos);
        p.setBpm (100.0);
        p.setPpqPosition (pos / 44100.0 * 100.0 / 60.0);
        p.setTimeSignature (TimeSignature { 4, 4 });
        return p;
    }
};

struct SungNote { float midi; float cents; double dur; bool vibrato; };

static std::vector<float> synthVoice (double sr)
{
    const std::vector<SungNote> melody = {
        { 69, 10, 0.55, false }, { 72, -15, 0.45, false }, { 64, 14, 0.50, false }, { 67, -16, 0.40, false },
        { 69, 12, 0.80, true },  { 66, 24, 0.40, false },  { 64, -10, 0.45, false }, { 62, 18, 0.40, false },
        { 60, -20, 0.70, true }, { 61, -22, 0.35, false }, { 62, 9, 0.40, false },  { 64, -12, 0.45, false },
        { 69, 8, 0.90, true }
    };
    std::vector<float> out ((size_t) (0.5 * sr), 0.0f);
    juce::Random rng (7);
    double phase = 0.0;
    auto formant = [] (double f)
    {
        auto pk = [f] (double c, double bw, double g) { const double d = (f - c) / bw; return g * std::exp (-0.5 * d * d); };
        return 0.08 + pk (700, 130, 1.0) + pk (1150, 160, 0.6) + pk (2600, 250, 0.25);
    };
    for (const auto& n : melody)
    {
        const int len = (int) (n.dur * sr);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            double m = n.midi + n.cents / 100.0 - 0.35 * std::exp (-t / 0.04);
            if (n.vibrato) m += 0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * t) * std::min (1.0, t / 0.15);
            const double f0 = 440.0 * std::pow (2.0, (m - 69.0) / 12.0);
            phase += f0 / sr;
            double s = 0.0;
            for (int k = 1; k * f0 < 7000.0; ++k)
                s += formant (k * f0) / std::sqrt ((double) k) * std::sin (2.0 * juce::MathConstants<double>::pi * k * phase);
            const double env = std::min (1.0, std::min (t / 0.02, (n.dur - t) / 0.03));
            out.push_back ((float) (0.25 * s * env));
        }
        for (int i = 0; i < (int) (0.09 * sr); ++i) out.push_back (0.01f * (rng.nextFloat() - 0.5f));
    }
    out.resize (out.size() + (size_t) (0.5 * sr), 0.0f);
    return out;
}

static std::vector<float> playThrough (ScribaPitchProcessor& p, FakePlayHead& ph, const std::vector<float>& in, double sr)
{
    constexpr int block = 512;
    std::vector<float> out;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    ph.playing = true;
    for (size_t pos = 0; pos + block <= in.size(); pos += block)
    {
        ph.pos = (juce::int64) pos;
        for (int ch = 0; ch < 2; ++ch)
            buf.copyFrom (ch, 0, in.data() + pos, block);
        p.processBlock (buf, midi);
        out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
        if (((pos / block) & 15) == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    }
    ph.playing = false;
    buf.clear();
    p.processBlock (buf, midi);
    juce::ignoreUnused (sr);
    return out;
}

static bool waitFor (const std::function<bool()>& cond, int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
    {
        if (cond()) return true;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }
    return cond();
}

static float measure (const std::vector<float>& x, double sr, double a, double b)
{
    const auto tr = trackPitch (x.data(), (int) x.size(), sr);
    std::vector<float> v;
    const double lo = a + (b - a) * 0.2, hi = b - (b - a) * 0.2;
    for (size_t i = 0; i < tr.midi.size(); ++i)
    {
        const double t = tr.frameTime ((int) i);
        if (t >= lo && t <= hi && tr.midi[i] > 0.0f) v.push_back (tr.midi[i]);
    }
    if (v.empty()) return 0.0f;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

static void snapshot (juce::AudioProcessorEditor& ed, const juce::File& f)
{
    auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
}

// SP_VOICE=<file.wav>: the real-vocal scenario. Every red note is dragged with the
// mouse one row up and one row down; the result must be a note in the key.
static int realVoiceDrag (const juce::File& wav, const juce::File& outDir)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (wav));
    if (r == nullptr) return 1;
    const double sr = r->sampleRate;
    juce::AudioBuffer<float> buf ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&buf, 0, buf.getNumSamples(), 0, true, true);
    std::vector<float> voice ((size_t) buf.getNumSamples());
    for (int i = 0; i < buf.getNumSamples(); ++i) voice[(size_t) i] = buf.getSample (0, i);

    FakePlayHead ph;
    auto proc = std::make_unique<ScribaPitchProcessor>();
    proc->setPlayHead (&ph);
    proc->setRateAndBufferSizeDetails (sr, 512);
    proc->prepareToPlay (sr, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    editor->setSize (1280, 800);
    proc->pressAnalyze();
    waitFor ([] { return false; }, 60);
    playThrough (*proc, ph, voice, sr);
    waitFor ([&] { return proc->getStatus() == ScribaPitchProcessor::Status::ready; }, 20000);
    waitFor ([] { return false; }, 300);

    PianoRoll* roll = nullptr;
    std::function<void (juce::Component*)> find = [&] (juce::Component* c)
    {
        if (auto* rr = dynamic_cast<PianoRoll*> (c)) roll = rr;
        for (auto* ch : c->getChildren()) find (ch);
    };
    find (editor.get());
    auto source = juce::Desktop::getInstance().getMainMouseSource();
    auto mouse = [&] (juce::Point<float> p, juce::Point<float> down, bool isDown)
    {
        return juce::MouseEvent (source, p, isDown ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(),
                                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, roll, roll, juce::Time::getCurrentTime(), down,
                                 juce::Time::getCurrentTime(), 1, true);
    };

    const auto scale = proc->getScale();
    std::printf ("key %s, %d notes\n", sp::keyLongName (proc->getEffectiveKey()).toRawUTF8(), (int) proc->getNotes().size());
    int tried = 0, ok = 0, timeMoved = 0, shown = 0;
    const auto start = proc->getNotes();
    for (const auto& n0 : start)
    {
        if (sp::tuningOf (n0, scale) != sp::Tuning::outOfKey) continue;
        for (int dir : { -1, 1 })
        {
            proc->pushUndo();   // so we can go back after each try
            roll->showAt (0.5 * (n0.outStart + n0.outEnd), n0.newCenter());
            waitFor ([] { return false; }, 30);
            const auto rb = roll->getNoteBounds (n0.id);
            if (rb.isEmpty()) continue;
            // a real hand: a bit of sideways wobble while dragging one row
            auto p0 = rb.getCentre();
            roll->mouseDown (mouse (p0, p0, true));
            for (int k = 1; k <= 8; ++k)
                roll->mouseDrag (mouse (p0.translated ((float) (k % 3), -dir * 24.0f * k / 8.0f), p0, true));
            roll->mouseUp (mouse (p0.translated (2.0f, -dir * 24.0f), p0, false));
            waitFor ([] { return false; }, 20);
            const sp::Note* m = nullptr;
            const auto now = proc->getNotes();
            for (const auto& x : now) if (x.id == n0.id) m = &x;
            ++tried;
            const bool good = m != nullptr && sp::tuningOf (*m, scale) != sp::Tuning::outOfKey
                              && std::abs (m->newCenter() - n0.newCenter()) > 0.3f
                              && (dir > 0 ? m->newCenter() > n0.newCenter() : m->newCenter() < n0.newCenter());
            ok += good ? 1 : 0;
            if (m != nullptr && std::abs (m->outStart - n0.outStart) > 1.0e-4) ++timeMoved;
            if (! good && shown++ < 12)
                std::printf ("  %.2f s %s %s -> %s (%s)\n", n0.outStart, noteName ((int) std::round (n0.newCenter())).toRawUTF8(),
                             dir > 0 ? "up" : "down", m ? noteName ((int) std::round (m->newCenter())).toRawUTF8() : "?",
                             m ? (sp::tuningOf (*m, scale) == sp::Tuning::outOfKey ? "still red" : "not moved the right way") : "lost");
            proc->undo();
        }
    }
    std::printf ("red notes dragged one row: %d/%d end in the key; %d also moved in time\n", ok, tried, timeMoved);
    juce::ignoreUnused (outDir);
    return ok == tried ? 0 : 1;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (auto* v = std::getenv ("SP_VOICE"))
        return realVoiceDrag (juce::File (juce::String (v)), argc > 1 ? juce::File (juce::String (argv[1])) : juce::File());
    const double sr = 44100.0;
    int failures = 0;
    const juce::File outDir = argc > 1 ? juce::File { juce::String (argv[1]) } : juce::File::getCurrentWorkingDirectory();
    outDir.createDirectory();

    FakePlayHead ph;
    auto proc = std::make_unique<ScribaPitchProcessor>();
    proc->setPlayHead (&ph);
    proc->setRateAndBufferSizeDetails (sr, 512);
    proc->prepareToPlay (sr, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    editor->setSize (1280, 800);
    snapshot (*editor, outDir.getChildFile ("gui_empty.png"));

    const auto voice = synthVoice (sr);

    // ---- capture ----------------------------------------------------------------
    proc->pressAnalyze();
    waitFor ([] { return false; }, 60);
    std::printf ("After ANALYZE: %s\n", proc->getStatusText().toRawUTF8());
    const auto passThrough = playThrough (*proc, ph, voice, sr);
    if (! waitFor ([&] { return proc->getStatus() == ScribaPitchProcessor::Status::ready; }, 8000))
    {
        std::printf ("FAIL: analysis did not finish (%s)\n", proc->getStatusText().toRawUTF8());
        return 1;
    }

    auto notes = proc->getNotes();
    const auto key = proc->getDetectedKey();
    std::printf ("Analysed: %d notes, key %s %.0f%%, take %.2f s\n", (int) notes.size(),
                 keyLongName (key.key).toRawUTF8(), key.confidence * 100.0f, proc->getTakeSeconds());
    if (notes.size() != 13) { std::printf ("FAIL: expected 13 notes\n"); ++failures; }
    if (key.key != 21) { std::printf ("FAIL: expected A minor\n"); ++failures; }

    // pass-through while recording must be untouched
    bool same = true;
    for (size_t i = 0; i < passThrough.size(); ++i)
        if (! juce::exactlyEqual (passThrough[i], voice[i])) { same = false; break; }
    std::printf ("Audio untouched while capturing: %s\n", same ? "yes" : "NO");
    if (! same) ++failures;

    waitFor ([] { return false; }, 100);
    snapshot (*editor, outDir.getChildFile ("gui_analysed.png"));

    // ---- colours: in tune / off by cents / out of key -------------------------------
    {
        int inTune = 0, off = 0, out = 0;
        const auto sc = proc->getScale();
        for (const auto& n : proc->getNotes())
        {
            const auto t = tuningOf (n, sc);
            inTune += t == Tuning::inTune; off += t == Tuning::offCents; out += t == Tuning::outOfKey;
        }
        std::printf ("Colours before correction: %d cyan (in tune), %d orange (off by cents), %d red (out of key)\n", inTune, off, out);
        if (out != 2 || off < 8) { std::printf ("FAIL: colours\n"); ++failures; }
    }

    // ---- Meteora Link: a Scriba instance in the same DAW ------------------------------
    {
        namespace ml = meteora::link;
        ml::Publisher fakeScriba;          // stands in for Scriba on the guitar track
        ml::Info info;
        info.plugin = "Scriba";
        info.instrument = "GUITAR";
        info.key = 2;                      // D major: now C4/C5 become wrong notes
        info.alt = 23;
        info.confidence = 0.91f;
        fakeScriba.publish (info);
        proc->pollLink();

        const bool linkedOk = proc->getKeySource() == ScribaPitchProcessor::KeySource::scriba && proc->getEffectiveKey() == 2;
        int red = 0;
        for (const auto& n : proc->getNotes())
            red += tuningOf (n, proc->getScale()) == Tuning::outOfKey;
        std::printf ("Link: Scriba (GUITAR) says %s -> Scriba Pitch uses %s, %d red notes (the C's)\n",
                     keyLongName (info.key).toRawUTF8(), keyLongName (proc->getEffectiveKey()).toRawUTF8(), red);
        if (! linkedOk || red != 2) { std::printf ("FAIL: link key\n"); ++failures; }

        // Scriba Pitch publishes the vocal key for Scriba
        bool sawVocal = false;
        for (const auto& o : ml::readOthers (fakeScriba.getInstanceId()))
            if (o.plugin == "Scriba Pitch" && o.key == 21) sawVocal = true;
        std::printf ("Link: Scriba can read the vocal key (A MINOR) from Scriba Pitch: %s\n", sawVocal ? "yes" : "NO");
        if (! sawVocal) ++failures;

        // the real case for the screenshot: guitar + bass in A minor, with the chords
        info.key = 21; info.alt = 0; info.confidence = 0.94f;
        const char* names[] = { "Am", "F", "C", "G" };
        for (int b = 0; b < 8; ++b) info.chords.push_back ({ b * 4.0, 4.0, names[b % 4] });
        fakeScriba.publish (info);
        ml::Publisher fakeBass;
        ml::Info bass; bass.plugin = "Scriba"; bass.instrument = "BASS"; bass.key = 21; bass.alt = 0; bass.confidence = 0.9f;
        fakeBass.publish (bass);
        proc->pollLink();
        waitFor ([] { return false; }, 150);
        snapshot (*editor, outDir.getChildFile ("gui_linked.png"));
        std::printf ("Link: %s from %s, %d chords on the roll\n", keyLongName (proc->getEffectiveKey()).toRawUTF8(),
                     proc->getLinked().sources.joinIntoString (" + ").toRawUTF8(), (int) proc->getLinked().chords.size());
        if (proc->getEffectiveKey() != 21 || proc->getLinked().chords.size() != 8) ++failures;

        fakeScriba.withdraw();
        fakeBass.withdraw();
        proc->pollLink();
        std::printf ("Link: Scriba closed -> back to the vocal key: %s\n",
                     proc->getKeySource() == ScribaPitchProcessor::KeySource::vocal ? "yes" : "NO");
        if (proc->getKeySource() != ScribaPitchProcessor::KeySource::vocal) ++failures;
    }

    // ---- automatic correction -----------------------------------------------------
    proc->automaticCorrection();
    waitFor ([] { return false; }, 400);   // background render
    snapshot (*editor, outDir.getChildFile ("gui_corrected.png"));

    notes = proc->getNotes();
    const auto scale = proc->getScale();
    {
        // CPU of corrected playback: processBlock only, no message pumping
        constexpr int block = 512;
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        ph.playing = true;
        double total = 0.0;
        int blocks = 0;
        for (size_t pos = 0; pos + block <= voice.size(); pos += block, ++blocks)
        {
            ph.pos = (juce::int64) pos;
            for (int ch = 0; ch < 2; ++ch) buf.copyFrom (ch, 0, voice.data() + pos, block);
            const auto t0 = juce::Time::getHighResolutionTicks();
            proc->processBlock (buf, midi);
            total += juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        }
        ph.playing = false;
        const double audioSecs = blocks * block / sr;
        std::printf ("CPU during corrected playback: %.4f%% of one core (%.2f us per 512-sample block)\n",
                     100.0 * total / audioSecs, 1.0e6 * total / blocks);
    }
    const auto corrected = playThrough (*proc, ph, voice, sr);
    int good = 0;
    for (const auto& n : notes)
    {
        const float target = n.newCenter();
        const float got = measure (corrected, sr, n.outStart, n.outEnd);
        const bool ok = std::abs (got - target) < 0.12f && scale.contains ((int) std::round (target));
        good += ok ? 1 : 0;
        std::printf ("  %-4s -> %-4s  measured %6.2f  target %6.2f %s\n", noteName ((int) std::round (n.origMidi)).toRawUTF8(),
                     noteName ((int) std::round (target)).toRawUTF8(), got, target, ok ? "" : " <-- FAIL");
    }
    if (good != (int) notes.size()) ++failures;
    std::printf ("Corrected playback: %d/%d notes in tune and in key (moved %d out-of-key notes)\n",
                 good, (int) notes.size(), proc->getLastMovedCount());

    // ---- mouse editing in the roll: drag a note up two semitones, stretch another --------
    {
        PianoRoll* roll = nullptr;
        std::function<void (juce::Component*)> find = [&] (juce::Component* c)
        {
            if (auto* r = dynamic_cast<PianoRoll*> (c)) roll = r;
            for (auto* ch : c->getChildren()) find (ch);
        };
        find (editor.get());
        if (roll == nullptr) { std::printf ("FAIL: no roll\n"); return 1; }

        auto source = juce::Desktop::getInstance().getMainMouseSource();
        auto mouse = [&] (juce::Point<float> p, juce::Point<float> down, bool isDown)
        {
            return juce::MouseEvent (source, p, isDown ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, roll, roll, juce::Time::getCurrentTime(), down,
                                     juce::Time::getCurrentTime(), 1, true);
        };

        roll->setSnap (1);                               // SCALE snap for this part
        auto shown = roll->getShownNotes();
        const int idA = shown[2].id;                     // E4: drag up 2 rows -> scale snap
        const float before = shown[2].newCenter();
        auto r = roll->getNoteBounds (idA);
        auto p0 = r.getCentre();
        // while the note is held, a clean tone at its new pitch plays (like Melodyne),
        // not the audio of that part of the take; it stops on release
        auto renderStopped = [&] (double seconds)
        {
            ph.playing = false;
            std::vector<float> out;
            juce::AudioBuffer<float> blk (2, 512);
            juce::MidiBuffer midi;
            for (int k = 0; k < (int) (seconds * sr / 512); ++k)
            {
                blk.clear();
                proc->processBlock (blk, midi);
                for (int j = 0; j < 512; ++j) out.push_back (blk.getSample (0, j));
            }
            return out;
        };
        auto rms = [] (const std::vector<float>& x, size_t from)
        {
            double e = 0.0; for (size_t j = from; j < x.size(); ++j) e += x[j] * x[j];
            return std::sqrt (e / (double) juce::jmax<size_t> (1, x.size() - from));
        };
        roll->mouseDown (mouse (p0, p0, true));
        const auto toneStart = renderStopped (0.5);
        const float startPitch = measure (toneStart, sr, 0.0, 0.5);
        roll->mouseDrag (mouse (p0.translated (0.0f, -24.0f * 2.0f), p0, true));
        const auto toneHeld = renderStopped (1.5);
        const float heldPitch = measure (toneHeld, sr, 0.2, 1.5);
        roll->mouseUp (mouse (p0.translated (0.0f, -24.0f * 2.0f), p0, false));
        const auto toneAfter = renderStopped (0.5);
        const double tail = rms (toneAfter, toneAfter.size() / 2);
        const bool toneOk = std::abs (startPitch - 64.0f) < 0.15f && std::abs (heldPitch - 67.0f) < 0.1f
                            && rms (toneHeld, toneHeld.size() / 2) > 0.05 && tail < 1.0e-4;
        std::printf ("Drag tone: on click %.2f (E4 = 64), held after drag %.2f (G4 = 67), silent after release: %s -> %s\n",
                     startPitch, heldPitch, tail < 1.0e-4 ? "yes" : "NO", toneOk ? "ok" : "FAIL");
        if (! toneOk) ++failures;

        const int idB = shown[8].id;                     // C4: pull the right edge 60 px
        const double lenBefore = shown[8].outLength();
        r = roll->getNoteBounds (idB);
        p0 = { r.getRight() - 3.0f, r.getCentreY() };
        roll->mouseDown (mouse (p0, p0, true));
        roll->mouseDrag (mouse (p0.translated (60.0f, 0.0f), p0, true));
        roll->mouseUp (mouse (p0.translated (60.0f, 0.0f), p0, false));

        waitFor ([] { return false; }, 300);
        const auto after = proc->getNotes();
        const sp::Note* a = nullptr; const sp::Note* b = nullptr;
        for (const auto& n : after) { if (n.id == idA) a = &n; if (n.id == idB) b = &n; }
        std::printf ("Mouse: dragged %s up -> %s;  stretched %s from %.2f s to %.2f s\n",
                     noteName ((int) std::round (before)).toRawUTF8(), a ? noteName ((int) std::round (a->newCenter())).toRawUTF8() : "?",
                     b ? noteName ((int) std::round (b->newCenter())).toRawUTF8() : "?", lenBefore, b ? b->outLength() : 0.0);
        if (a == nullptr || std::abs (a->newCenter() - 67.0f) > 0.05f) { std::printf ("FAIL: drag\n"); ++failures; }
        if (b == nullptr || b->outLength() < lenBefore + 0.05) { std::printf ("FAIL: stretch\n"); ++failures; }

        const auto edited = playThrough (*proc, ph, voice, sr);
        const float gotA = measure (edited, sr, a->outStart, a->outEnd);
        const float gotB = measure (edited, sr, b->outStart, b->outEnd);
        std::printf ("  rendered: moved note %.2f (target 67), stretched note %.2f (target %.0f)\n", gotA, gotB, std::round (b->newCenter()));
        if (std::abs (gotA - 67.0f) > 0.12f || std::abs (gotB - std::round (b->newCenter())) > 0.12f) { std::printf ("FAIL: edited render\n"); ++failures; }

        snapshot (*editor, outDir.getChildFile ("gui_edited.png"));

        // ---- pencil: draw a flat line one semitone up over the middle of the vibrato A4 ----
        {
            roll->setTool (PianoRoll::toolDraw);
            auto drawMouse = [&] (juce::Point<float> p, juce::Point<float> down, bool isDown, bool alt)
            {
                auto mods = juce::ModifierKeys (isDown ? juce::ModifierKeys::leftButtonModifier : 0)
                              .withFlags (alt ? juce::ModifierKeys::altModifier : 0);
                return juce::MouseEvent (source, p, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, roll, roll,
                                         juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), 1, true);
            };
            auto stroke = [&] (int id, float fromFrac, float toFrac, float semis, bool alt)
            {
                const auto nr = roll->getNoteBounds (id);
                const float y = nr.getCentreY() - semis * 24.0f;
                const juce::Point<float> pa { nr.getX() + nr.getWidth() * fromFrac, y };
                const juce::Point<float> pb { nr.getX() + nr.getWidth() * toFrac, y };
                roll->mouseDown (drawMouse (pa, pa, true, alt));
                for (int k = 1; k <= 6; ++k)
                    roll->mouseDrag (drawMouse (pa + (pb - pa) * (k / 6.0f), pa, true, alt));
                roll->mouseUp (drawMouse (pb, pa, false, alt));
                waitFor ([] { return false; }, 300);
            };

            const auto vib = roll->getShownNotes()[4];
            const float centre = vib.newCenter();
            stroke (vib.id, 0.25f, 0.75f, 1.0f, false);

            const sp::Note* drawnNote = nullptr;
            for (const auto& n : proc->getNotes()) if (n.id == vib.id) drawnNote = &n;
            const auto noteNow = *drawnNote;
            const auto audio = playThrough (*proc, ph, voice, sr);
            const double a0 = noteNow.outStart + noteNow.outLength() * 0.35, a1 = noteNow.outStart + noteNow.outLength() * 0.65;
            const float drawnPitch = measure (audio, sr, a0, a1);
            std::printf ("Pencil: drew %+.0f st over %s -> measured %.2f (target %.2f), %d points\n", 1.0,
                         noteName ((int) std::round (centre)).toRawUTF8(), drawnPitch, centre + 1.0f, (int) noteNow.drawU.size());
            if (std::abs (drawnPitch - (centre + 1.0f)) > 0.12f) { std::printf ("FAIL: pencil\n"); ++failures; }

            snapshot (*editor, outDir.getChildFile ("gui_pencil.png"));

            stroke (vib.id, 0.0f, 1.0f, 0.0f, true);   // Alt = erase across the whole note
            bool cleared = true;
            for (const auto& n : proc->getNotes()) if (n.id == vib.id) cleared = ! n.hasDrawing();
            const auto erased = playThrough (*proc, ph, voice, sr);
            const float back = measure (erased, sr, a0, a1);
            std::printf ("Pencil: Alt-erase -> curve removed: %s, measured %.2f (target %.2f)\n", cleared ? "yes" : "NO", back, centre);
            if (! cleared || std::abs (back - centre) > 0.25f) { std::printf ("FAIL: erase\n"); ++failures; }

            proc->undo();   // erase
            proc->undo();   // draw
            roll->setTool (PianoRoll::toolMain);
        }

        proc->undo();   // stretch
        proc->undo();   // drag
        waitFor ([] { return false; }, 300);
    }

    // ---- undo restores the original -------------------------------------------------
    std::printf ("After undoing the edits the button shows the correction again: %s\n", proc->isAutoCorrected() ? "yes" : "NO");
    if (! proc->isAutoCorrected()) ++failures;
    proc->automaticCorrection();   // second click on the button = undo the correction
    waitFor ([] { return false; }, 400);
    const auto undone = playThrough (*proc, ph, voice, sr);
    same = true;
    for (size_t i = 0; i < undone.size(); ++i)
        if (std::abs (undone[i] - voice[i]) > 1.0e-6f) { same = false; break; }
    std::printf ("Undo gives back the original audio: %s\n", same ? "yes" : "NO");
    if (! same) ++failures;

    // ---- keys 1-9, BYPASS, AUTO KEY / MANUAL KEY, ruler click without ARA ---------------------
    {
        MainView* view = nullptr;
        PianoRoll* roll = nullptr;
        std::function<void (juce::Component*)> find = [&] (juce::Component* c)
        {
            if (auto* v = dynamic_cast<MainView*> (c)) view = v;
            if (auto* r = dynamic_cast<PianoRoll*> (c)) roll = r;
            for (auto* ch : c->getChildren()) find (ch);
        };
        find (editor.get());

        bool keysOk = true;
        for (int t = 0; t < PianoRoll::numTools; ++t)
        {
            view->keyPressed (juce::KeyPress ('1' + t, {}, (juce::juce_wchar) ('1' + t)));
            keysOk = keysOk && proc->toolIndex.load() == t;
        }
        view->keyPressed (juce::KeyPress ('1', {}, '1'));
        std::printf ("Keys 1-9 select the 9 tools: %s\n", keysOk ? "yes" : "NO");
        if (! keysOk) ++failures;

        // bypass: the corrected voice is not played, the input passes untouched
        proc->automaticCorrection();
        waitFor ([] { return false; }, 400);
        proc->params.getParameter ("bypass")->setValueNotifyingHost (1.0f);
        waitFor ([] { return false; }, 60);
        snapshot (*editor, outDir.getChildFile ("gui_bypass.png"));
        const auto byp = playThrough (*proc, ph, voice, sr);
        bool untouched = true;
        for (size_t i = 0; i < byp.size(); ++i) if (! juce::exactlyEqual (byp[i], voice[i])) { untouched = false; break; }
        proc->params.getParameter ("bypass")->setValueNotifyingHost (0.0f);
        const auto unbyp = playThrough (*proc, ph, voice, sr);
        bool differs = false;
        for (size_t i = 0; i < unbyp.size(); ++i) if (! juce::exactlyEqual (unbyp[i], voice[i])) { differs = true; break; }
        std::printf ("BYPASS on: original audio passes untouched: %s;  off: corrected again: %s\n", untouched ? "yes" : "NO", differs ? "yes" : "NO");
        if (! untouched || ! differs) ++failures;
        // A/B: hold B = original, release = corrected
        view->keyPressed (juce::KeyPress ('b', {}, 'b'));
        const bool keyWorks = proc->isHoldingOriginal();
        editor->repaint();
        snapshot (*editor, outDir.getChildFile ("gui_hold_original.png"));
        view->keyStateChanged (false);
        const bool keyReleases = ! proc->isHoldingOriginal();
        std::printf ("B key: pressed -> original %s, released -> corrected %s\n", keyWorks ? "yes" : "NO", keyReleases ? "yes" : "NO");
        if (! keyWorks || ! keyReleases) ++failures;

        proc->setHoldOriginal (true);   // (no real keyboard here: hold it directly while playing)
        const auto held = playThrough (*proc, ph, voice, sr);
        bool heldOriginal = true;
        for (size_t i = 0; i < held.size(); ++i) if (! juce::exactlyEqual (held[i], voice[i])) { heldOriginal = false; break; }
        proc->setHoldOriginal (false);
        const auto released = playThrough (*proc, ph, voice, sr);
        bool backCorrected = false;
        for (size_t i = 0; i < released.size(); ++i) if (! juce::exactlyEqual (released[i], voice[i])) { backCorrected = true; break; }
        std::printf ("Hold B: original voice: %s;  release: corrected again: %s\n", heldOriginal ? "yes" : "NO", backCorrected ? "yes" : "NO");
        if (! heldOriginal || ! backCorrected) ++failures;

        proc->automaticCorrection();   // undo, as the next section expects
        waitFor ([] { return false; }, 400);

        // manual key
        proc->params.getParameter ("keyMode")->setValueNotifyingHost (1.0f);
        auto* mk = proc->params.getParameter ("manualKey");
        mk->setValueNotifyingHost (mk->convertTo0to1 (2.0f));   // D major
        const bool manualOk = proc->getEffectiveKey() == 2 && proc->getKeySource() == ScribaPitchProcessor::KeySource::manual;
        waitFor ([] { return false; }, 100);
        snapshot (*editor, outDir.getChildFile ("gui_manual_key.png"));
        proc->params.getParameter ("keyMode")->setValueNotifyingHost (0.0f);
        const bool autoOk = proc->getEffectiveKey() == 21 && proc->getKeySource() == ScribaPitchProcessor::KeySource::vocal;
        std::printf ("MANUAL KEY -> %s, AUTO KEY -> %s: %s\n", keyLongName (2).toRawUTF8(), keyLongName (21).toRawUTF8(),
                     manualOk && autoOk ? "yes" : "NO");
        if (! manualOk || ! autoOk) ++failures;

        // ruler click without ARA: a message, nothing breaks
        auto source = juce::Desktop::getInstance().getMainMouseSource();
        const juce::Point<float> rp { 500.0f, 10.0f };
        roll->mouseDown (juce::MouseEvent (source, rp, juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, roll, roll,
                                           juce::Time::getCurrentTime(), rp, juce::Time::getCurrentTime(), 1, false));
        waitFor ([] { return false; }, 100);
        snapshot (*editor, outDir.getChildFile ("gui_ruler_no_ara.png"));
        std::printf ("Ruler click without ARA shows the ARA message (see gui_ruler_no_ara.png)\n");

        // REAPER without ARA: the ruler moves REAPER's cursor through its own API
        {
            fakereaper::Api api;
            proc->getVST3ClientExtensions()->setIHostApplication (&api);
            const juce::Point<float> rp2 { 700.0f, 10.0f };
            roll->mouseDown (juce::MouseEvent (source, rp2, juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, roll, roll,
                                               juce::Time::getCurrentTime(), rp2, juce::Time::getCurrentTime(), 1, false));
            waitFor ([] { return fakereaper::lastCursor >= 0.0; }, 1000);
            const bool moved = fakereaper::lastCursor > 0.0 && fakereaper::lastSeekPlay;
            std::printf ("REAPER (no ARA): ruler click moves REAPER's cursor to %.2f s: %s\n", fakereaper::lastCursor, moved ? "yes" : "NO");
            if (! moved) ++failures;
            proc->requestHostPlayToggle();
            waitFor ([] { return fakereaper::playPresses > 0; }, 1000);
            proc->requestHostPlayToggle();
            waitFor ([] { return fakereaper::stopPresses > 0; }, 1000);
            const bool transport = fakereaper::playPresses == 1 && fakereaper::stopPresses == 1;
            std::printf ("REAPER (no ARA): play / stop from the plugin: %s\n", transport ? "yes" : "NO");
            if (! transport) ++failures;
        }
    }

    // ---- save / reload session ----------------------------------------------------------
    proc->automaticCorrection();
    waitFor ([] { return false; }, 400);
    juce::MemoryBlock state;
    {
        // a pencil drawing must survive saving and reopening the project
        auto ns = proc->getNotes();
        auto& n = ns[2];
        n.drawSegment (0.2f, 0.8f, { { 0.2f, 0.5f }, { 0.5f, 0.8f }, { 0.8f, 0.5f } });
        proc->applyNotes (ns);
        waitFor ([] { return false; }, 400);
    }
    proc->getStateInformation (state);
    const auto expected = playThrough (*proc, ph, voice, sr);

    editor.reset();
    proc.reset();

    auto proc2 = std::make_unique<ScribaPitchProcessor>();
    proc2->setPlayHead (&ph);
    proc2->setRateAndBufferSizeDetails (sr, 512);
    proc2->prepareToPlay (sr, 512);
    proc2->setStateInformation (state.getData(), (int) state.getSize());
    if (! waitFor ([&] { return proc2->getStatus() == ScribaPitchProcessor::Status::ready; }, 8000))
    {
        std::printf ("FAIL: session did not reload (%s)\n", proc2->getStatusText().toRawUTF8());
        return 1;
    }
    waitFor ([] { return false; }, 500);
    const auto reloaded = playThrough (*proc2, ph, voice, sr);
    int drawnAfter = 0;
    float worstSt = 0.0f;
    for (const auto& n : proc2->getNotes())
    {
        drawnAfter += n.hasDrawing() ? 1 : 0;
        const float a = measure (expected, sr, n.outStart, n.outEnd), b = measure (reloaded, sr, n.outStart, n.outEnd);
        worstSt = std::max (worstSt, std::abs (a - b));
    }
    std::printf ("Reloaded session: %d notes, %d with a pencil drawing, worst pitch difference from before saving %.3f st\n",
                 (int) proc2->getNotes().size(), drawnAfter, worstSt);
    if (drawnAfter != 1 || worstSt > 0.03f) ++failures;

    juce::FileOutputStream (outDir.getChildFile ("plugin_corrected.raw")).write (expected.data(), expected.size() * sizeof (float));

    std::unique_ptr<juce::AudioProcessorEditor> editor2 (proc2->createEditor());
    editor2->setSize (1280, 800);
    waitFor ([] { return false; }, 200);
    snapshot (*editor2, outDir.getChildFile ("gui_reloaded.png"));
    editor2.reset();

    std::printf (failures == 0 ? "\nPLUGIN TEST PASSED\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
