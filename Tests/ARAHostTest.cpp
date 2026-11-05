// A minimal ARA2 host ("DAW") for testing Scriba Pitch the way Logic / Cubase /
// Studio One / Reaper use it: the plugin is given the vocal as an audio file on
// the timeline and must show the notes without the song being played.
//
//   ScribaPitchARATest <path to "Scriba Pitch.vst3"> [output folder]

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine/Renderer.h"
#include <ARA_API/ARAInterface.h>
#include <ARA_Library/Dispatch/ARAHostDispatch.h>
#include <cstdio>

using namespace juce;

//==============================================================================
// Host model (adapted from JUCE's AudioPluginHost ARA test host)
class FileAudioSource
{
public:
    FileAudioSource (ARA::Host::DocumentController& dc, const File& file)
        : formatReader ([&file]
          {
              auto r = rawToUniquePtr (WavAudioFormat().createMemoryMappedReader (file));
              r->mapEntireFile();
              return r;
          }()),
          audioSource (Converter::toHostRef (this), dc, properties())
    {
        audioSource.enableAudioSourceSamplesAccess (true);
    }

    bool readAudioSamples (float* const* buffers, int64 start, int64 num)
    {
        return formatReader->read (buffers, (int) formatReader->numChannels, start, (int) num);
    }

    MemoryMappedAudioFormatReader& getFormatReader() const { return *formatReader; }
    auto& getSource() { return audioSource; }
    using Converter = ARAHostModel::ConversionFunctions<FileAudioSource*, ARA::ARAAudioSourceHostRef>;

private:
    ARA::ARAAudioSourceProperties properties() const
    {
        auto p = ARAHostModel::AudioSource::getEmptyProperties();
        p.name = "Lead vocal";
        p.persistentID = "lead-vocal-take-1";
        p.sampleCount = formatReader->lengthInSamples;
        p.sampleRate = formatReader->sampleRate;
        p.channelCount = (int) formatReader->numChannels;
        p.merits64BitSamples = false;
        return p;
    }

    std::unique_ptr<MemoryMappedAudioFormatReader> formatReader;
    ARAHostModel::AudioSource audioSource;
};

class AudioAccessController final : public ARA::Host::AudioAccessControllerInterface
{
public:
    ARA::ARAAudioReaderHostRef createAudioReaderForSource (ARA::ARAAudioSourceHostRef src, bool use64) noexcept override
    {
        auto r = std::make_unique<Reader> (Reader { src, use64 });
        auto ref = Converter::toHostRef (r.get());
        readers.emplace (r.get(), std::move (r));
        return ref;
    }
    bool readAudioSamples (ARA::ARAAudioReaderHostRef ref, ARA::ARASamplePosition pos, ARA::ARASampleCount num, void* const* buffers) noexcept override
    {
        auto* r = Converter::fromHostRef (ref);
        if (r->use64) return false;
        return FileAudioSource::Converter::fromHostRef (r->src)->readAudioSamples (reinterpret_cast<float* const*> (buffers), pos, num);
    }
    void destroyAudioReader (ARA::ARAAudioReaderHostRef ref) noexcept override { readers.erase (Converter::fromHostRef (ref)); }

private:
    struct Reader { ARA::ARAAudioSourceHostRef src; bool use64; };
    using Converter = ARAHostModel::ConversionFunctions<Reader*, ARA::ARAAudioReaderHostRef>;
    std::map<Reader*, std::unique_ptr<Reader>> readers;
};

class ArchivingController final : public ARA::Host::ArchivingControllerInterface
{
public:
    ARA::ARASize getArchiveSize (ARA::ARAArchiveReaderHostRef) noexcept override { return 0; }
    bool readBytesFromArchive (ARA::ARAArchiveReaderHostRef, ARA::ARASize, ARA::ARASize, ARA::ARAByte*) noexcept override { return false; }
    bool writeBytesToArchive (ARA::ARAArchiveWriterHostRef, ARA::ARASize, ARA::ARASize, const ARA::ARAByte*) noexcept override { return true; }
    void notifyDocumentArchivingProgress (float) noexcept override {}
    void notifyDocumentUnarchivingProgress (float) noexcept override {}
    ARA::ARAPersistentID getDocumentArchiveID (ARA::ARAArchiveReaderHostRef) noexcept override { return nullptr; }
};

class ContentAccessController final : public ARA::Host::ContentAccessControllerInterface
{
public:
    bool isMusicalContextContentAvailable (ARA::ARAMusicalContextHostRef, ARA::ARAContentType) noexcept override { return false; }
    ARA::ARAContentGrade getMusicalContextContentGrade (ARA::ARAMusicalContextHostRef, ARA::ARAContentType) noexcept override { return 0; }
    ARA::ARAContentReaderHostRef createMusicalContextContentReader (ARA::ARAMusicalContextHostRef, ARA::ARAContentType, const ARA::ARAContentTimeRange*) noexcept override { return nullptr; }
    bool isAudioSourceContentAvailable (ARA::ARAAudioSourceHostRef, ARA::ARAContentType) noexcept override { return false; }
    ARA::ARAContentGrade getAudioSourceContentGrade (ARA::ARAAudioSourceHostRef, ARA::ARAContentType) noexcept override { return 0; }
    ARA::ARAContentReaderHostRef createAudioSourceContentReader (ARA::ARAAudioSourceHostRef, ARA::ARAContentType, const ARA::ARAContentTimeRange*) noexcept override { return nullptr; }
    ARA::ARAInt32 getContentReaderEventCount (ARA::ARAContentReaderHostRef) noexcept override { return 0; }
    const void* getContentReaderDataForEvent (ARA::ARAContentReaderHostRef, ARA::ARAInt32) noexcept override { return nullptr; }
    void destroyContentReader (ARA::ARAContentReaderHostRef) noexcept override {}
};

class ModelUpdateController final : public ARA::Host::ModelUpdateControllerInterface
{
public:
    void notifyAudioSourceAnalysisProgress (ARA::ARAAudioSourceHostRef, ARA::ARAAnalysisProgressState, float) noexcept override {}
    void notifyAudioSourceContentChanged (ARA::ARAAudioSourceHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
    void notifyAudioModificationContentChanged (ARA::ARAAudioModificationHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
    void notifyPlaybackRegionContentChanged (ARA::ARAPlaybackRegionHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
};

/** Records what the plugin asks the DAW's transport to do. */
struct TransportLog
{
    std::atomic<int> starts { 0 }, stops { 0 };
    std::atomic<double> lastPosition { -1.0 };
};

class PlaybackController final : public ARA::Host::PlaybackControllerInterface
{
public:
    explicit PlaybackController (TransportLog& l) : log (l) {}
    void requestStartPlayback() noexcept override { ++log.starts; }
    void requestStopPlayback() noexcept override { ++log.stops; }
    void requestSetPlaybackPosition (ARA::ARATimePosition t) noexcept override { log.lastPosition = t; }
    void requestSetCycleRange (ARA::ARATimePosition, ARA::ARATimeDuration) noexcept override {}
    void requestEnableCycle (bool) noexcept override {}
private:
    TransportLog& log;
};

struct PlayHead final : public AudioPlayHead
{
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setTimeInSamples (pos);
        p.setIsPlaying (playing);
        p.setBpm (100.0);
        p.setTimeSignature (TimeSignature { 4, 4 });
        p.setPpqPosition ((double) pos / 44100.0 * 100.0 / 60.0);
        return p;
    }
    int64 pos = 0;
    bool playing = false;
};

//==============================================================================
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
    Random rng (7);
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
            if (n.vibrato) m += 0.3 * std::sin (2.0 * MathConstants<double>::pi * 5.5 * t) * std::min (1.0, t / 0.15);
            const double f0 = 440.0 * std::pow (2.0, (m - 69.0) / 12.0);
            phase += f0 / sr;
            double s = 0.0;
            for (int k = 1; k * f0 < 7000.0; ++k)
                s += formant (k * f0) / std::sqrt ((double) k) * std::sin (2.0 * MathConstants<double>::pi * k * phase);
            const double env = std::min (1.0, std::min (t / 0.02, (n.dur - t) / 0.03));
            out.push_back ((float) (0.25 * s * env));
        }
        for (int i = 0; i < (int) (0.09 * sr); ++i) out.push_back (0.01f * (rng.nextFloat() - 0.5f));
    }
    out.resize (out.size() + (size_t) (0.5 * sr), 0.0f);
    return out;
}

static float measure (const std::vector<float>& x, double sr, double a, double b)
{
    const auto tr = sp::trackPitch (x.data(), (int) x.size(), sr);
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

static void pump (int ms)
{
    MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

// The JUCE VST3 host wraps the plugin state: <VST3PluginState><IComponent>base64</IComponent>.
// Inside it: our copyXmlToBinary block, then JUCE's private data.
static MemoryBlock componentBytes (AudioPluginInstance& p, std::unique_ptr<XmlElement>& outer)
{
    MemoryBlock mb;
    p.getStateInformation (mb);
    outer = AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
    MemoryBlock comp;
    if (outer != nullptr)
        if (auto* c = outer->getChildByName ("IComponent"))
            comp.fromBase64Encoding (c->getAllSubText());
    return comp;
}

static std::unique_ptr<XmlElement> pluginState (AudioPluginInstance& p)
{
    std::unique_ptr<XmlElement> outer;
    const auto comp = componentBytes (p, outer);
    return AudioProcessor::getXmlFromBinary (comp.getData(), (int) comp.getSize());
}

static void setPluginState (AudioPluginInstance& p, const XmlElement& ours)
{
    std::unique_ptr<XmlElement> outer;
    const auto comp = componentBytes (p, outer);
    const auto oldSize = 8 + (size_t) static_cast<const uint32*> (comp.getData())[1] + 1;   // magic, size, text, 0
    MemoryBlock fresh;
    AudioProcessor::copyXmlToBinary (ours, fresh);
    if (comp.getSize() > oldSize)
        fresh.append (addBytesToPointer (comp.getData(), oldSize), comp.getSize() - oldSize);
    outer->getChildByName ("IComponent")->deleteAllTextElements();
    outer->getChildByName ("IComponent")->addTextElement (fresh.toBase64Encoding());
    MemoryBlock all;
    AudioProcessor::copyXmlToBinary (*outer, all);
    p.setStateInformation (all.getData(), (int) all.getSize());
}

static std::vector<float> render (AudioPluginInstance& p, PlayHead& ph, int64 from, int64 to)
{
    constexpr int block = 512;
    AudioBuffer<float> buf (2, block);
    MidiBuffer midi;
    std::vector<float> out;
    ph.playing = true;
    for (int64 pos = from; pos < to; pos += block)
    {
        ph.pos = pos;
        buf.clear();
        p.processBlock (buf, midi);
        out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
    }
    ph.playing = false;
    return out;
}

//==============================================================================
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::printf ("usage: ScribaPitchARATest <Scriba Pitch.vst3> [out]\n"); return 2; }
    const File plugin { String (argv[1]) };
    const File outDir = argc > 2 ? File { String (argv[2]) } : File::getCurrentWorkingDirectory();
    outDir.createDirectory();
    const double sr = 44100.0;
    int failures = 0;

    // the vocal file the "DAW" has on the track
    const auto voice = synthVoice (sr);
    const auto wavFile = outDir.getChildFile ("ara_lead_vocal.wav");
    {
        wavFile.deleteFile();
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (new FileOutputStream (wavFile), sr, 1, 24, {}, 0));
        const float* ch[] = { voice.data() };
        w->writeFromFloatArrays (ch, 1, (int) voice.size());
    }

    // load the VST3
    VST3PluginFormat format;
    OwnedArray<PluginDescription> descs;
    format.findAllTypesForFile (descs, plugin.getFullPathName());
    if (descs.isEmpty()) { std::printf ("FAIL: could not load %s\n", plugin.getFullPathName().toRawUTF8()); return 1; }
    const auto& desc = *descs[0];
    std::printf ("Plugin: %s by %s, ARA: %s\n", desc.name.toRawUTF8(), desc.manufacturerName.toRawUTF8(), desc.hasARAExtension ? "yes" : "NO");
    if (! desc.hasARAExtension) return 1;

    String error;
    auto instance = format.createInstanceFromDescription (desc, sr, 512, error);
    if (instance == nullptr) { std::printf ("FAIL: %s\n", error.toRawUTF8()); return 1; }

    ARAFactoryWrapper factory;
    bool gotFactory = false;
    createARAFactoryAsync (*instance, [&] (ARAFactoryWrapper f) { factory = std::move (f); gotFactory = true; });
    for (int i = 0; i < 200 && ! gotFactory; ++i) pump (10);
    if (factory.get() == nullptr) { std::printf ("FAIL: no ARA factory\n"); return 1; }

    TransportLog transport;
    auto dc = ARAHostDocumentController::create (std::move (factory), "Test song",
                                                 std::make_unique<AudioAccessController>(),
                                                 std::make_unique<ArchivingController>(),
                                                 std::make_unique<ContentAccessController>(),
                                                 std::make_unique<ModelUpdateController>(),
                                                 std::make_unique<PlaybackController> (transport));
    if (dc == nullptr) { std::printf ("FAIL: document controller\n"); return 1; }

    const auto roles = ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole | ARA::kARAEditorViewRole;
    auto ext = dc->bindDocumentToPluginInstance (*instance, roles, roles);
    auto playbackRenderer = ext.getPlaybackRendererInterface();

    // the track: the vocal clip starts at 2.0 s in the song
    const double clipStart = 2.0;
    auto& hostDc = dc->getDocumentController();
    std::unique_ptr<ARAHostModel::MusicalContext> context;
    std::unique_ptr<ARAHostModel::RegionSequence> sequence;
    std::unique_ptr<FileAudioSource> source;
    std::unique_ptr<ARAHostModel::AudioModification> modification;
    std::unique_ptr<ARAHostModel::PlaybackRegion> region;

    auto regionProps = [&] (double start)
    {
        auto p = ARAHostModel::PlaybackRegion::getEmptyProperties();
        p.transformationFlags = ARA::kARAPlaybackTransformationNoChanges;
        p.startInModificationTime = 0.0;
        p.durationInModificationTime = (double) voice.size() / sr;
        p.startInPlaybackTime = start;
        p.durationInPlaybackTime = p.durationInModificationTime;
        p.musicalContextRef = context->getPluginRef();
        p.regionSequenceRef = sequence->getPluginRef();
        return p;
    };
    {
        const ARAEditGuard guard (hostDc);
        auto cp = ARAHostModel::MusicalContext::getEmptyProperties();
        cp.name = "Song";
        context = std::make_unique<ARAHostModel::MusicalContext> (nullptr, hostDc, cp);
        auto sp_ = ARAHostModel::RegionSequence::getEmptyProperties();
        sp_.name = "Vocal track";
        sp_.musicalContextRef = context->getPluginRef();
        sequence = std::make_unique<ARAHostModel::RegionSequence> (nullptr, hostDc, sp_);
        source = std::make_unique<FileAudioSource> (hostDc, wavFile);
        auto mp = ARAHostModel::AudioModification::getEmptyProperties();
        mp.persistentID = "lead-vocal-mod-1";
        modification = std::make_unique<ARAHostModel::AudioModification> (nullptr, hostDc, source->getSource(), mp);
        region = std::make_unique<ARAHostModel::PlaybackRegion> (nullptr, hostDc, *modification, regionProps (clipStart));
    }
    playbackRenderer.add (*region);

    PlayHead ph;
    instance->setPlayHead (&ph);
    instance->prepareToPlay (sr, 512);

    // ---- 1. the notes appear by themselves, without playing --------------------
    const auto t0 = Time::getMillisecondCounterHiRes();
    std::unique_ptr<XmlElement> st;
    int numNotes = 0;
    while (Time::getMillisecondCounterHiRes() - t0 < 20000.0)
    {
        pump (50);
        st = pluginState (*instance);
        if (st != nullptr)
            if (auto* s = st->getChildByName ("Session"))
                if (s->getStringAttribute ("araSig").isNotEmpty())
                    if (auto* nx = s->getChildByName ("Notes"))
                        if ((numNotes = nx->getNumChildElements()) > 0) break;
    }
    const double secs = (Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    std::printf ("ARA: %d notes on screen %.2f s after inserting the plugin, without pressing play (%.1f s of vocal)\n",
                 numNotes, secs, (double) voice.size() / sr);
    if (numNotes != 13 || secs > 5.0) { std::printf ("FAIL: ARA reading\n"); ++failures; }

    // ---- 2. playback through ARA: the plugin plays the clip at its place ------
    pump (300);
    const auto clipSamples = (int64) (clipStart * sr);
    auto out = render (*instance, ph, 0, clipSamples + (int64) voice.size());
    double err = 0.0, ref = 0.0;
    for (size_t i = 0; i < voice.size(); ++i)
    {
        const double d = out[(size_t) clipSamples + i] - voice[i];
        err += d * d; ref += (double) voice[i] * voice[i];
    }
    double before = 0.0;
    for (int64 i = 0; i < clipSamples; ++i) before += std::abs (out[(size_t) i]);
    std::printf ("ARA playback: clip plays at %.1f s, difference from the file %.1f dB, silence before the clip: %s\n",
                 clipStart, 10.0 * std::log10 (err / ref + 1.0e-20), before < 1.0e-6 ? "yes" : "NO");
    if (err / ref > 1.0e-6 || before > 1.0e-6) { std::printf ("FAIL: ARA playback\n"); ++failures; }

    // ---- 3. correct the notes (as AUTOMATIC CORRECTION does) through the saved state
    {
        auto s = pluginState (*instance);
        auto* sess = s->getChildByName ("Session");
        const auto scale = sp::makeScale (21, sp::scaleOfKey);
        for (auto* e : sess->getChildByName ("Notes")->getChildIterator())
        {
            const float orig = (float) e->getDoubleAttribute ("orig");
            e->setAttribute ("shift", (double) ((float) scale.nearest (orig) - std::round (orig)));
            e->setAttribute ("ca", 1.0);
        }
        setPluginState (*instance, *s);
        pump (800);

        out = render (*instance, ph, 0, clipSamples + (int64) voice.size());
        std::vector<float> clip (out.begin() + clipSamples, out.end());
        int good = 0, total = 0;
        for (auto* e : sess->getChildByName ("Notes")->getChildIterator())
        {
            const float target = (float) scale.nearest ((float) e->getDoubleAttribute ("orig"));
            const float got = measure (clip, sr, e->getDoubleAttribute ("outS"), e->getDoubleAttribute ("outE"));
            ++total;
            good += std::abs (got - target) < 0.12f ? 1 : 0;
        }
        std::printf ("ARA corrected playback: %d/%d notes in tune and in key\n", good, total);
        if (good != total || total == 0) ++failures;
    }

    // ---- 4. the clip is moved in the DAW: the corrections move with it ---------
    {
        const ARAEditGuard guard (hostDc);
        region->update (regionProps (clipStart + 1.0));
    }
    pump (1500);
    auto s2 = pluginState (*instance);
    int kept = 0;
    for (auto* e : s2->getChildByName ("Session")->getChildByName ("Notes")->getChildIterator())
        kept += std::abs (e->getDoubleAttribute ("ca") - 1.0) < 1.0e-6 ? 1 : 0;
    const auto moved = (int64) ((clipStart + 1.0) * sr);
    out = render (*instance, ph, 0, moved + (int64) voice.size());
    std::vector<float> clip2 (out.begin() + moved, out.end());
    const float firstNote = measure (clip2, sr, 0.30, 0.85);
    std::printf ("Clip moved by +1 s: %d notes keep their correction, first note at the new place measures %.2f (target 69)\n",
                 kept, firstNote);
    if (kept != 13 || std::abs (firstNote - 69.0f) > 0.12f) ++failures;

    // ---- 5. reopening the project: state goes into a fresh instance ----------
    {
        MemoryBlock saved;
        instance->getStateInformation (saved);   // what the DAW stores in the project

        auto inst2 = format.createInstanceFromDescription (desc, sr, 512, error);
        ARAFactoryWrapper f2; bool got2 = false;
        createARAFactoryAsync (*inst2, [&] (ARAFactoryWrapper f) { f2 = std::move (f); got2 = true; });
        for (int i = 0; i < 200 && ! got2; ++i) pump (10);
        TransportLog t2;
        auto dc2 = ARAHostDocumentController::create (std::move (f2), "Reopened",
                                                      std::make_unique<AudioAccessController>(), std::make_unique<ArchivingController>(),
                                                      std::make_unique<ContentAccessController>(), std::make_unique<ModelUpdateController>(),
                                                      std::make_unique<PlaybackController> (t2));
        inst2->setStateInformation (saved.getData(), (int) saved.getSize());   // DAWs restore the state first
        auto ext2 = dc2->bindDocumentToPluginInstance (*inst2, roles, roles);
        auto pr2 = ext2.getPlaybackRendererInterface();
        auto& h2 = dc2->getDocumentController();
        std::unique_ptr<ARAHostModel::MusicalContext> c2;
        std::unique_ptr<ARAHostModel::RegionSequence> q2;
        std::unique_ptr<FileAudioSource> src2;
        std::unique_ptr<ARAHostModel::AudioModification> m2;
        std::unique_ptr<ARAHostModel::PlaybackRegion> r2;
        {
            const ARAEditGuard guard (h2);
            auto cp = ARAHostModel::MusicalContext::getEmptyProperties();
            c2 = std::make_unique<ARAHostModel::MusicalContext> (nullptr, h2, cp);
            auto sq = ARAHostModel::RegionSequence::getEmptyProperties();
            sq.musicalContextRef = c2->getPluginRef();
            q2 = std::make_unique<ARAHostModel::RegionSequence> (nullptr, h2, sq);
            src2 = std::make_unique<FileAudioSource> (h2, wavFile);
            auto mp = ARAHostModel::AudioModification::getEmptyProperties();
            mp.persistentID = "lead-vocal-mod-1";
            m2 = std::make_unique<ARAHostModel::AudioModification> (nullptr, h2, src2->getSource(), mp);
            auto p = ARAHostModel::PlaybackRegion::getEmptyProperties();
            p.transformationFlags = ARA::kARAPlaybackTransformationNoChanges;
            p.durationInModificationTime = (double) voice.size() / sr;
            p.startInPlaybackTime = clipStart + 1.0;
            p.durationInPlaybackTime = p.durationInModificationTime;
            p.musicalContextRef = c2->getPluginRef();
            p.regionSequenceRef = q2->getPluginRef();
            r2 = std::make_unique<ARAHostModel::PlaybackRegion> (nullptr, h2, *m2, p);
        }
        pr2.add (*r2);
        PlayHead ph2;
        inst2->setPlayHead (&ph2);
        inst2->prepareToPlay (sr, 512);
        pump (3500);
        auto o2 = render (*inst2, ph2, 0, moved + (int64) voice.size());
        std::vector<float> clip3 (o2.begin() + moved, o2.end());
        const float n5 = measure (clip3, sr, 3.40, 3.86);   // the F#4 that became G4
        std::printf ("Project reopened: the corrected F#4 still plays as %.2f (target 67)\n", n5);
        if (std::abs (n5 - 67.0f) > 0.12f) ++failures;

        inst2->releaseResources();
        pr2.remove (*r2);
        {
            const ARAEditGuard guard (h2);
            r2.reset(); m2.reset(); src2.reset(); q2.reset(); c2.reset();
        }
        inst2.reset();
    }

    instance->releaseResources();
    playbackRenderer.remove (*region);
    {
        const ARAEditGuard guard (hostDc);
        region.reset(); modification.reset(); source.reset(); sequence.reset(); context.reset();
    }
    instance.reset();

    std::printf (failures == 0 ? "\nARA TEST PASSED\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
