#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr double maxTakeSeconds = 8.0 * 60.0;
    constexpr size_t maxUndo = 100;

    juce::StringArray keyChoices()
    {
        juce::StringArray s;
        for (int k = 0; k < 24; ++k)
        {
            const auto shortName = sp::keyShortName (k);
            s.add (k < 12 ? shortName + " major" : shortName.dropLastCharacters (1) + " minor");
        }
        return s;
    }

    inline float readLinear (const float* x, juce::int64 n, double p) noexcept
    {
        if (p < 0.0 || p >= (double) (n - 1)) return 0.0f;
        const auto i = (juce::int64) p;
        const float f = (float) (p - (double) i);
        return x[i] + (x[i + 1] - x[i]) * f;
    }
}

//==============================================================================
class ScribaPitchProcessor::Worker  : public juce::Thread
{
public:
    explicit Worker (ScribaPitchProcessor& p) : juce::Thread ("Scriba Pitch worker"), owner (p) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            event.wait (250);
            if (threadShouldExit()) break;

            if (wantAnalysis.exchange (false)) owner.runAnalysis();
            if (wantLoad.exchange (false))     owner.runLoad();
            if (wantAraRead.exchange (false))  owner.runAraRead();
            owner.runRender();
        }
    }

    void poke() { event.signal(); }

    std::atomic<bool> wantAnalysis { false }, wantLoad { false }, wantAraRead { false };

private:
    ScribaPitchProcessor& owner;
    juce::WaitableEvent event;
};

//==============================================================================
ScribaPitchProcessor::ScribaPitchProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "PARAMS", createLayout())
{
    bypassParam   = dynamic_cast<juce::AudioParameterBool*>   (params.getParameter ("bypass"));
    keyModeParam  = dynamic_cast<juce::AudioParameterChoice*> (params.getParameter ("keyMode"));
    manualKeyParam= dynamic_cast<juce::AudioParameterChoice*> (params.getParameter ("manualKey"));
    scaleParam    = dynamic_cast<juce::AudioParameterChoice*> (params.getParameter ("scale"));
    corrParam     = dynamic_cast<juce::AudioParameterFloat*>  (params.getParameter ("corrAmt"));
    driftParam    = dynamic_cast<juce::AudioParameterFloat*>  (params.getParameter ("drift"));
    humanizeParam = dynamic_cast<juce::AudioParameterFloat*>  (params.getParameter ("humanize"));
    keepVibParam  = dynamic_cast<juce::AudioParameterBool*>   (params.getParameter ("keepVib"));

    worker = std::make_unique<Worker> (*this);
    worker->startThread (juce::Thread::Priority::low);
    pollLink();
    startTimerHz (20);
}

ScribaPitchProcessor::~ScribaPitchProcessor()
{
    stopTimer();
    cancelAnalysis = true;
    worker->stopThread (5000);
}

juce::AudioProcessorValueTreeState::ParameterLayout ScribaPitchProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (juce::roundToInt (v)) + "%"; });

    l.add (std::make_unique<AudioParameterBool>   (ParameterID { "bypass", 1 }, "Bypass", false));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "keyMode", 1 }, "Key Mode", StringArray { "Auto Key", "Manual Key" }, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "manualKey", 1 }, "Manual Key", keyChoices(), 21));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", sp::scaleTypeNames(), 0));
    l.add (std::make_unique<AudioParameterFloat>  (ParameterID { "corrAmt", 1 }, "Correction Amount", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f, pct));
    l.add (std::make_unique<AudioParameterFloat>  (ParameterID { "drift", 1 }, "Pitch Drift", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 40.0f, pct));
    l.add (std::make_unique<AudioParameterFloat>  (ParameterID { "humanize", 1 }, "Humanize", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 15.0f, pct));
    l.add (std::make_unique<AudioParameterBool>   (ParameterID { "keepVib", 1 }, "Keep Vibrato", true));
    return l;
}

//==============================================================================
void ScribaPitchProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
   #if JucePlugin_Enable_ARA
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
   #else
    juce::ignoreUnused (samplesPerBlock);
   #endif
}

void ScribaPitchProcessor::releaseResources()
{
   #if JucePlugin_Enable_ARA
    releaseResourcesForARA();
   #endif
}

bool ScribaPitchProcessor::isAraMode() const noexcept
{
   #if JucePlugin_Enable_ARA
    return isBoundToARA() && getPlaybackRenderer<ScribaPlaybackRenderer>() != nullptr;
   #else
    return false;
   #endif
}

bool ScribaPitchProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ScribaPitchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const bool ara = isAraMode();
   #if JucePlugin_Enable_ARA
    if (ara)
        processBlockForARA (buffer, isRealtime(), getPlayHead());   // clears: the voice comes from the take below
   #endif
    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, n);

    juce::Optional<juce::AudioPlayHead::PositionInfo> pos;
    if (auto* ph = getPlayHead())
        pos = ph->getPosition();

    const bool playing = pos.hasValue() && pos->getIsPlaying();
    juce::int64 hostPos = fallbackPosition;
    if (pos.hasValue() && pos->getTimeInSamples().hasValue())
        hostPos = *pos->getTimeInSamples();
    else if (playing)
        fallbackPosition += n;
    transportPlaying = playing;
    if (pos.hasValue())
    {
        if (auto bpm = pos->getBpm()) hostBpm = *bpm;
        if (auto sig = pos->getTimeSignature()) { hostSigNum = sig->numerator; hostSigDen = sig->denominator; }
    }

    const juce::SpinLock::ScopedTryLockType lock (audioLock);
    if (! lock.isLocked())
        return;   // a take is being swapped right now: pass the input through

    auto st = status.load();

    // Start capturing when the transport starts (not with ARA: the DAW gives us the track).
    if (! ara && st == Status::armed && playing && audioPending != nullptr)
    {
        audioTake = audioPending;
        audioPending = nullptr;
        audioTake->startSample = hostPos;
        audioTake->sampleRate = currentSampleRate;
        audioTake->length = 0;
        if (pos.hasValue())
        {
            if (auto bpm = pos->getBpm()) audioTake->bpm = *bpm;
            if (auto ppq = pos->getPpqPosition()) { audioTake->startPpq = *ppq; audioTake->hasPpq = true; }
            if (auto sig = pos->getTimeSignature()) { audioTake->sigNum = sig->numerator; audioTake->sigDen = sig->denominator; }
        }
        status = st = Status::recording;
    }

    Take* tk = audioTake;

    if (! ara && st == Status::recording && tk != nullptr)
    {
        if (playing)
        {
            const juce::int64 idx = hostPos - tk->startSample;
            const auto cap = (juce::int64) tk->data.size();
            if (idx >= 0 && idx + n <= cap)
            {
                float* dst = tk->data.data() + idx;
                const float g = 1.0f / (float) juce::jmax (1, numIn);
                for (int i = 0; i < n; ++i)
                {
                    float s = 0.0f;
                    for (int ch = 0; ch < numIn; ++ch) s += buffer.getSample (ch, i);
                    dst[i] = s * g;
                }
                if (idx + n > tk->length.load()) tk->length = idx + n;
            }
            else if (idx + n > cap)
                stopRequested = true;
        }
        else
            stopRequested = true;
    }

    if (tk != nullptr)
    {
        playheadSeconds = (double) (hostPos - tk->startSample) / tk->sampleRate;
        rateMismatch = std::abs (tk->sampleRate - currentSampleRate) > 1.0;
    }

    const bool bypassed = bypassParam->get();

    if (ara)
    {
        // ARA: this plugin plays the track. Corrected voice, or the original when bypassed / A-B.
        if (playing && tk != nullptr && tk->cacheReady.load())
        {
            const bool corrected = ! bypassed && ! holdOriginal.load() && ! rateMismatch.load();
            const auto& src = corrected ? tk->cache[tk->front.load()] : tk->data;
            const auto len = juce::jmin ((juce::int64) src.size(), tk->length.load());
            const juce::int64 idx = hostPos - tk->startSample;
            for (int i = 0; i < n; ++i)
            {
                const juce::int64 p = idx + i;
                if (p < 0 || p >= len) continue;
                const float v = src[(size_t) p];
                for (int ch = 0; ch < numOut; ++ch)
                    buffer.setSample (ch, i, v);
            }
        }
    }
    // Corrected playback: the audio thread only reads samples rendered in advance.
    else if (! bypassed && st == Status::ready && tk != nullptr && tk->cacheReady.load() && ! holdOriginal.load()
             && playing && ! rateMismatch.load())
    {
        const auto& cache = tk->cache[tk->front.load()];
        const auto len = (juce::int64) cache.size();
        const juce::int64 idx = hostPos - tk->startSample;
        if (idx < len && idx + n > 0)
        {
            for (int i = 0; i < n; ++i)
            {
                const juce::int64 p = idx + i;
                if (p < 0 || p >= len) continue;
                const float v = cache[(size_t) p];
                for (int ch = 0; ch < numOut; ++ch)
                    buffer.setSample (ch, i, v);
            }
        }
    }

    // Preview of the note being edited.
    const int trig = previewTrigger.load();
    if (trig != previewSeen)
    {
        previewSeen = trig;
        if (tk != nullptr && st == Status::ready)
        {
            previewActive = true;
            previewSrcStart = previewStart.load() * tk->sampleRate;
            previewOutLength = juce::jmin (previewLength.load(), previewFromCache.load() ? 2.0 : 0.7) * currentSampleRate;
            previewOut = 0.0;
            previewRatioNow = previewRatio.load() * (float) (tk->sampleRate / currentSampleRate);
            previewCacheNow = previewFromCache.load() && tk->cacheReady.load();
        }
    }

    // Reference tone: a soft, rounded tone (sine + a little 2nd and 3rd harmonic)
    // at the pitch of the note being moved; it glides to each new note in 25 ms.
    {
        const int tt = toneTrigger.load();
        if (tt != toneSeen)
        {
            toneSeen = tt;
            if (! toneActive || toneEnv < 0.01f) { toneMidiNow = toneTarget.load(); tonePhase = 0.0; }
            toneActive = true;
            toneAge = 0.0;
            toneLength = 0.45 * currentSampleRate;   // without hold: a short note
            toneHeldMode = toneHold.load();
        }
        if (toneActive)
        {
            const float target = toneTarget.load();
            const bool hold = toneHold.load();
            const float glide = 1.0f - std::exp (-1.0f / (float) (0.025 * currentSampleRate / 4.0));
            const float att = 1.0f - std::exp (-1.0f / (float) (0.006 * currentSampleRate));
            const float rel = 1.0f - std::exp (-1.0f / (float) (0.035 * currentSampleRate));
            for (int i = 0; i < n; ++i)
            {
                toneMidiNow += (target - toneMidiNow) * glide;
                const bool on = toneHeldMode ? (hold && toneAge < 6.0 * currentSampleRate) : toneAge < toneLength;
                toneEnv += ((on ? 1.0f : 0.0f) - toneEnv) * (on ? att : rel);
                const double f = 440.0 * std::pow (2.0, (toneMidiNow - 69.0) / 12.0);
                tonePhase += juce::MathConstants<double>::twoPi * f / currentSampleRate;
                if (tonePhase > juce::MathConstants<double>::twoPi) tonePhase -= juce::MathConstants<double>::twoPi;
                const float v = (float) (std::sin (tonePhase) + 0.22 * std::sin (2.0 * tonePhase) + 0.08 * std::sin (3.0 * tonePhase));
                for (int ch = 0; ch < numOut; ++ch)
                    buffer.addSample (ch, i, v * toneEnv * 0.22f);
                toneAge += 1.0;
                if (! on && toneEnv < 1.0e-4f) { toneActive = false; toneEnv = 0.0f; break; }
            }
        }
    }

    if (previewActive && tk != nullptr && st == Status::ready)
    {
        const auto& cacheBuf = tk->cache[tk->front.load()];
        const float* d = previewCacheNow ? cacheBuf.data() : tk->data.data();
        const auto len = previewCacheNow ? (juce::int64) cacheBuf.size() : tk->length.load();
        const double attack = 0.005 * currentSampleRate, release = 0.04 * currentSampleRate;
        for (int i = 0; i < n; ++i)
        {
            if (previewOut >= previewOutLength) { previewActive = false; break; }
            const float v = readLinear (d, len, previewSrcStart + previewOut * previewRatioNow);
            const double env = juce::jmin (1.0, previewOut / attack, (previewOutLength - previewOut) / release);
            for (int ch = 0; ch < numOut; ++ch)
                buffer.addSample (ch, i, v * (float) env * 0.9f);
            previewOut += 1.0;
        }
    }
}

//==============================================================================
void ScribaPitchProcessor::timerCallback()
{
    if (++linkTicks >= 20)   // once a second: a few tiny files, message thread only
    {
        linkTicks = 0;
        pollLink();
    }

    // ARA: look at the track twice a second, or at once after the DAW edited it.
   #if JucePlugin_Enable_ARA
    if (++araTicks >= 10 || ScribaDocumentController::modelGeneration().load() != araGeneration)
    {
        araTicks = 0;
        araGeneration = ScribaDocumentController::modelGeneration().load();
        pollAra();
    }
   #endif
    {
        bool jobDone = false;
        {
            const juce::ScopedLock sl (dataLock);
            jobDone = araJob != nullptr && araJob->finished.load();
        }
        if (jobDone)
        {
            std::shared_ptr<AraJob> old;
            {
                const juce::ScopedLock sl (dataLock);
                old = std::move (araJob);
            }
            old.reset();   // the ARA readers are released on the message thread
        }
    }

    if (araFailed.exchange (false))
    {
        araSignature.clear();
        araContentSig.clear();
    }

    // ARA: follow the DAW tempo for the bar grid (the take was read before playing)
    if (take != nullptr && take->id == "ara" && std::abs (take->bpm - hostBpm.load()) > 1.0e-3)
    {
        take->bpm = hostBpm.load();
        take->sigNum = hostSigNum.load();
        take->sigDen = hostSigDen.load();
        take->startPpq = (double) take->startSample / take->sampleRate * take->bpm / 60.0;
        ++changeCounter;
    }

    // Without ARA: arm by itself a moment after insertion, so pressing play is enough.
    if (! autoArmDone && ++ticksSinceStart > 50)
    {
        autoArmDone = true;
        bool hasSession;
        {
            const juce::ScopedLock sl (dataLock);
            hasSession = pendingSession.takeId.isNotEmpty() || savedAraSignature.isNotEmpty();
        }
        if (! isAraMode() && ! hasSession && ! userDisarmed && status.load() == Status::idle)
            pressAnalyze();
    }

    if (renderedPreviewPending)
    {
        bool waiting;
        {
            const juce::ScopedLock sl (dataLock);
            waiting = renderPending || rendering.load();
        }
        if (! waiting)
        {
            renderedPreviewPending = false;
            previewStart = renderedPreviewStart;
            previewLength = renderedPreviewLength;
            previewRatio = 1.0f;
            previewFromCache = true;
            ++previewTrigger;
        }
    }

    const auto st = status.load();

    // The audio thread started capturing: adopt the new take.
    if (st == Status::recording)
        adoptPendingTake();

    if (stopRequested.exchange (false) && status.load() == Status::recording)
        beginAnalysis();

    // A take finished loading from disk.
    std::shared_ptr<Take> loaded;
    {
        const juce::ScopedLock sl (dataLock);
        const auto stNow = status.load();
        if ((stNow == Status::loading || stNow == Status::reading) && workerTake != nullptr && workerTake != take && workerTake->cacheReady.load())
            loaded = workerTake;
    }
    if (loaded != nullptr)
    {
        if (status.load() == Status::reading)   // a new track from ARA: a fresh edit history
        {
            undoStack.clear();
            redoStack.clear();
            autoCorrected = false;
        }
        take = loaded;
        {
            const juce::SpinLock::ScopedLockType l (audioLock);
            audioTake = take.get();
        }
        status = Status::ready;
        ++changeCounter;
    }
}

void ScribaPitchProcessor::adoptPendingTake()
{
    if (pendingTake == nullptr)
        return;

    take = std::move (pendingTake);
    {
        const juce::ScopedLock sl (dataLock);
        notes.clear();
        analysis.reset();
        workerTake = take;
        renderPending = false;
    }
    undoStack.clear();
    redoStack.clear();
    autoCorrected = false;
    ++changeCounter;
}

void ScribaPitchProcessor::pressAnalyze()
{
    const auto st = status.load();

    if (st == Status::analyzing || st == Status::loading || st == Status::reading)
        return;

    if (isAraMode())
    {
        rereadTrack();
        return;
    }

    if (st == Status::recording)
    {
        adoptPendingTake();
        beginAnalysis();
        return;
    }

    if (st == Status::armed)   // cancel
    {
        userDisarmed = true;
        {
            const juce::SpinLock::ScopedLockType l (audioLock);
            audioPending = nullptr;
        }
        pendingTake.reset();
        status = statusBeforeArm;
        ++changeCounter;
        return;
    }

    cancelAnalysis = true;
    auto t = std::make_shared<Take>();
    t->sampleRate = currentSampleRate;
    t->data.assign ((size_t) (maxTakeSeconds * currentSampleRate), 0.0f);
    t->id = juce::Uuid().toString();
    pendingTake = t;
    statusBeforeArm = (st == Status::missingTake) ? Status::idle : st;
    {
        const juce::SpinLock::ScopedLockType l (audioLock);
        audioPending = t.get();
    }
    status = Status::armed;
    ++changeCounter;
}

void ScribaPitchProcessor::beginAnalysis()
{
    if (take == nullptr || take->length.load() < (juce::int64) (0.5 * take->sampleRate))
    {
        status = Status::idle;
        ++changeCounter;
        return;
    }
    status = Status::analyzing;
    cancelAnalysis = false;
    worker->wantAnalysis = true;
    worker->poke();
    ++changeCounter;
}

//==============================================================================
void ScribaPitchProcessor::runAnalysis()
{
    std::shared_ptr<Take> tk;
    {
        const juce::ScopedLock sl (dataLock);
        tk = workerTake;
    }
    if (tk == nullptr) return;
    if (! analyseTake (tk, nullptr)) return;

    status = Status::ready;
    ++changeCounter;
    saveTakeToDisk (*tk);
}

bool ScribaPitchProcessor::analyseTake (const std::shared_ptr<Take>& tk, const std::vector<sp::Note>* savedNotes)
{
    const int len = (int) tk->length.load();
    const double sr = tk->sampleRate;

    auto an = std::make_shared<AnalysisData>();
    an->track = sp::trackPitch (tk->data.data(), len, sr, &cancelAnalysis);
    if (cancelAnalysis.load()) return false;

    auto ns = savedNotes != nullptr ? *savedNotes : sp::segmentNotes (an->track, len / sr);
    const auto key = sp::detectKey (ns);
    an->marks = sp::computeMarks (tk->data.data(), len, sr, an->track);

    tk->data.resize ((size_t) len);   // release the space reserved for capture
    tk->data.shrink_to_fit();
    tk->cache[0].assign (tk->data.begin(), tk->data.begin() + len);
    tk->cache[1] = tk->cache[0];
    tk->front = 0;

    bool anyEdit = false;
    for (const auto& n : ns) anyEdit = anyEdit || ! n.isIdentity();

    {
        const juce::ScopedLock sl (dataLock);
        if (workerTake != tk) return false;
        analysis = an;
        notes = std::move (ns);
        detectedKey = key;
        syncFrom = 0.0;  syncTo = -1.0;
        dirtyFrom = 0.0; dirtyTo = anyEdit ? tk->seconds() : -1.0;
        renderPending = anyEdit;
    }
    tk->cacheReady = true;
    return true;
}

void ScribaPitchProcessor::runLoad()
{
    PendingSession ps;
    {
        const juce::ScopedLock sl (dataLock);
        ps = pendingSession;
    }

    const auto file = getTakesFolder().getChildFile (ps.takeId + ".flac");
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (file.existsAsFile() ? fm.createReaderFor (file) : nullptr);

    if (reader == nullptr)
    {
        const juce::ScopedLock sl (dataLock);
        notes = ps.notes;
        analysis.reset();
        status = Status::missingTake;
        ++changeCounter;
        return;
    }

    auto tk = std::make_shared<Take>();
    const int len = (int) reader->lengthInSamples;
    tk->sampleRate = reader->sampleRate;
    tk->data.resize ((size_t) len);
    {
        juce::AudioBuffer<float> tmp (1, len);
        reader->read (&tmp, 0, len, 0, true, false);
        std::copy (tmp.getReadPointer (0), tmp.getReadPointer (0) + len, tk->data.begin());
    }
    tk->length = len;
    tk->id = ps.takeId;
    tk->startSample = ps.startSample;
    tk->startPpq = ps.startPpq;
    tk->bpm = ps.bpm;
    tk->sigNum = ps.num;
    tk->sigDen = ps.den;
    tk->hasPpq = ps.hasPpq;

    auto an = std::make_shared<AnalysisData>();
    an->track = sp::trackPitch (tk->data.data(), len, tk->sampleRate);
    an->marks = sp::computeMarks (tk->data.data(), len, tk->sampleRate, an->track);
    tk->cache[0] = tk->data;
    tk->cache[1] = tk->data;
    tk->front = 0;

    bool anyEdit = false;
    for (const auto& n : ps.notes) anyEdit = anyEdit || ! n.isIdentity();

    {
        const juce::ScopedLock sl (dataLock);
        analysis = an;
        notes = ps.notes;
        workerTake = tk;
        syncFrom = 0.0; syncTo = -1.0;
        if (anyEdit)
        {
            dirtyFrom = 0.0;
            dirtyTo = tk->seconds();
            renderPending = true;
        }
    }
    tk->cacheReady = true;   // the timer adopts the take now
    ++changeCounter;
}

//==============================================================================
juce::String ScribaPitchProcessor::currentAraSignature (bool& allReadable, juce::String& content, juce::int64& firstStart) const
{
    allReadable = true;
    content.clear();
    firstStart = 0;
    juce::String sig;
   #if JucePlugin_Enable_ARA
    auto* renderer = getPlaybackRenderer<ScribaPlaybackRenderer>();
    if (renderer == nullptr) return {};

    struct R { juce::String id; juce::int64 start, len, srcStart, count; double rate; int chans; };
    std::vector<R> list;
    for (auto* region : renderer->getPlaybackRegions())
    {
        auto* src = region->getAudioModification()->getAudioSource();
        allReadable = allReadable && src->isSampleAccessEnabled();
        const auto range = region->getSampleRange (currentSampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
        list.push_back ({ juce::String (src->getPersistentID()), range.getStart(), range.getLength(),
                          region->getStartInAudioModificationSamples(), src->getSampleCount(), src->getSampleRate(),
                          src->getChannelCount() });
    }
    if (list.empty()) return {};

    std::sort (list.begin(), list.end(), [] (const R& a, const R& b) { return a.start < b.start; });
    firstStart = list.front().start;
    for (const auto& r : list)
    {
        content << r.id << ":" << (r.start - firstStart) << "+" << r.len << "@" << r.srcStart << ":"
                << r.count << ":" << r.rate << ":" << r.chans << ";";
    }
    content << "sr" << currentSampleRate;
    sig << content << "|at" << firstStart;
   #endif
    return sig;
}

juce::File ScribaPitchProcessor::getLogFile()
{
    return getTakesFolder().getParentDirectory().getChildFile ("ScribaPitch_log.txt");
}

void ScribaPitchProcessor::araLog (const juce::String& line) const
{
    auto f = getLogFile();
    f.getParentDirectory().createDirectory();
    if (f.getSize() > 300 * 1024) f.deleteFile();
    f.appendText (juce::Time::getCurrentTime().toString (true, true, true, true) + "  ["
                  + juce::String::toHexString ((juce::pointer_sized_int) this).getLastCharacters (6) + "]  " + line + "\n");
}

juce::String ScribaPitchProcessor::getAraDiagnostics() const
{
   #if JucePlugin_Enable_ARA
    if (! diagBound)    return "NO ARA: THE DAW IS NOT GIVING THIS INSERT ITS TRACK (CAPTURE MODE)";
    if (! diagRenderer) return "ARA LINKED, BUT THE DAW DID NOT MAKE SCRIBA PITCH PLAY THE TRACK";
    if (diagRegions == 0) return "ARA OK, BUT THE DAW HAS NOT GIVEN ANY AUDIO CLIP YET";
    if (diagReadable < diagRegions)
        return "ARA: " + juce::String (diagRegions) + " CLIPS, AUDIO ACCESS " + juce::String (diagReadable) + "/"
               + juce::String (diagRegions) + " (WAITING FOR THE DAW)";
    if (araReadFailures.load() > 0)
        return "ARA: " + juce::String (diagRegions) + " CLIPS, READING THE AUDIO FAILED " + juce::String (araReadFailures.load())
               + " TIMES (RETRYING)";
    return {};
   #else
    return "THIS BUILD HAS NO ARA";
   #endif
}

void ScribaPitchProcessor::pollAra()
{
   #if JucePlugin_Enable_ARA
    diagBound = isBoundToARA();
    diagRenderer = isAraMode();
    diagRegions = diagReadable = 0;
    if (diagRenderer)
        for (auto* region : getPlaybackRenderer<ScribaPlaybackRenderer>()->getPlaybackRegions())
        {
            ++diagRegions;
            diagReadable += region->getAudioModification()->getAudioSource()->isSampleAccessEnabled() ? 1 : 0;
        }
    {
        const juce::String state = juce::String ("bound=") + (diagBound ? "yes" : "no") + " renderer=" + (diagRenderer ? "yes" : "no")
                                 + " clips=" + juce::String (diagRegions) + " readable=" + juce::String (diagReadable)
                                 + " sr=" + juce::String (currentSampleRate) + " status=" + getStatusText();
        if (state != lastLoggedState)
        {
            lastLoggedState = state;
            araLog (state);
        }
    }

    const bool bound = isAraMode();
    araBound = bound;
    if (! bound) return;

    if (status.load() == Status::armed)   // armed before the DAW bound ARA: not needed any more
    {
        {
            const juce::SpinLock::ScopedLockType l (audioLock);
            audioPending = nullptr;
        }
        pendingTake.reset();
        status = Status::idle;
        ++changeCounter;
    }

    bool readable = false;
    juce::String content;
    juce::int64 firstStart = 0;
    const auto sig = currentAraSignature (readable, content, firstStart);
    if (sig.isEmpty() || ! readable) return;
    if (sig == araSignature && ! araForceReread) return;   // already read

    {
        const juce::ScopedLock sl (dataLock);
        if (araJob != nullptr) return;   // still reading the previous version
    }

    if (sig != araSeenSignature && ! araForceReread)
    {
        araSeenSignature = sig;   // wait one more look: the DAW may still be moving things
        return;
    }

    // Only moved in the song (same audio, same layout): move the take, keep every edit.
    if (! araForceReread && take != nullptr && take->id == "ara" && content == araContentSig)
    {
        {
            const juce::SpinLock::ScopedLockType l (audioLock);
            take->startSample = firstStart;
        }
        take->startPpq = (double) firstStart / take->sampleRate * take->bpm / 60.0;
        araSignature = sig;
        ++changeCounter;
        return;
    }

    auto job = std::make_shared<AraJob>();
    job->sampleRate = currentSampleRate;
    job->signature = sig;
    job->contentSignature = content;
    juce::int64 lo = std::numeric_limits<juce::int64>::max(), hi = 0;

    for (auto* region : getPlaybackRenderer<ScribaPlaybackRenderer>()->getPlaybackRegions())
    {
        auto* src = region->getAudioModification()->getAudioSource();
        const auto range = region->getSampleRange (currentSampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
        if (range.getLength() <= 0) continue;
        AraRegion r;
        r.source = src;
        r.reader = std::make_unique<juce::ARAAudioSourceReader> (src);
        r.songStart = range.getStart();
        r.length = range.getLength();
        r.sourceStart = region->getStartInAudioModificationSamples();
        r.sourceRate = src->getSampleRate();
        lo = juce::jmin (lo, r.songStart);
        hi = juce::jmax (hi, r.songStart + r.length);
        job->regions.push_back (std::move (r));
    }
    if (job->regions.empty()) return;

    job->start = lo;
    job->length = juce::jmin (hi - lo, (juce::int64) (15.0 * 60.0 * currentSampleRate));

    // re-reading the same audio (READ AGAIN): keep the edits
    if (take != nullptr && take->id == "ara" && content == araContentSig)
    {
        const juce::ScopedLock sl (dataLock);
        savedAraSignature = content;
        savedAraNotes = notes;
    }

    araSignature = sig;
    araContentSig = content;
    araForceReread = false;
    cancelAnalysis = false;
    {
        const juce::ScopedLock sl (dataLock);
        araJob = job;
        notes.clear();
        analysis.reset();
    }
    araLog ("reading " + juce::String (job->regions.size()) + " clips, " + juce::String ((double) job->length / job->sampleRate, 1) + " s");
    status = Status::reading;
    worker->wantAraRead = true;
    worker->poke();
    ++changeCounter;
   #endif
}

void ScribaPitchProcessor::rereadTrack()
{
    araForceReread = true;
    pollAra();
}

void ScribaPitchProcessor::runAraRead()
{
    std::shared_ptr<AraJob> job;
    {
        const juce::ScopedLock sl (dataLock);
        job = araJob;
    }
    if (job == nullptr) return;
    const juce::ScopeGuard done { [&job] { job->finished = true; } };

    auto tk = std::make_shared<Take>();
    tk->sampleRate = job->sampleRate;
    tk->startSample = job->start;
    tk->id = "ara";
    tk->data.assign ((size_t) job->length, 0.0f);

    bool ok = true;
    for (auto& r : job->regions)
    {
        auto& reader = *r.reader;
        const int chans = (int) juce::jmax (1u, reader.numChannels);
        const juce::int64 dstStart = r.songStart - job->start;
        const juce::int64 n = juce::jmin (r.length, job->length - dstStart);
        if (n <= 0) continue;

        const double ratio = r.sourceRate / job->sampleRate;
        const auto srcLen = (juce::int64) std::ceil ((double) n * ratio) + 8;
        std::vector<float> mono ((size_t) srcLen, 0.0f);

        constexpr int chunk = 65536;
        juce::AudioBuffer<float> buf (chans, chunk);
        for (juce::int64 off = 0; off < srcLen; off += chunk)
        {
            const int num = (int) juce::jmin ((juce::int64) chunk, srcLen - off);
            if (! reader.read (&buf, 0, num, r.sourceStart + off, true, true))
            {
                araLog ("read failed: clip at " + juce::String ((double) r.songStart / job->sampleRate, 2) + " s, source offset "
                        + juce::String (r.sourceStart + off) + ", " + juce::String (num) + " samples, source length "
                        + juce::String (reader.lengthInSamples) + ", channels " + juce::String (chans));
                ok = false;
                break;
            }
            for (int i = 0; i < num; ++i)
            {
                float v = 0.0f;
                for (int ch = 0; ch < chans; ++ch) v += buf.getSample (ch, i);
                mono[(size_t) (off + i)] = v / (float) chans;
            }
        }
        if (! ok) break;

        float* dst = tk->data.data() + dstStart;
        if (std::abs (ratio - 1.0) < 1.0e-9)
        {
            for (juce::int64 i = 0; i < n; ++i) dst[i] += mono[(size_t) i];
        }
        else
        {
            std::vector<float> res ((size_t) n);
            juce::LagrangeInterpolator li;
            li.process (ratio, mono.data(), res.data(), (int) n);
            for (juce::int64 i = 0; i < n; ++i) dst[i] += res[(size_t) i];
        }
    }

    if (! ok)
    {
        ++araReadFailures;
        araFailed = true;   // the timer clears the signature: try again on the next look
        status = Status::idle;
        ++changeCounter;
        return;
    }

    tk->length = job->length;
    tk->bpm = hostBpm.load();
    tk->sigNum = hostSigNum.load();
    tk->sigDen = hostSigDen.load();
    tk->startPpq = (double) job->start / job->sampleRate * tk->bpm / 60.0;   // constant tempo from bar 1
    tk->hasPpq = true;

    std::vector<sp::Note> saved;
    bool useSaved = false;
    {
        const juce::ScopedLock sl (dataLock);
        workerTake = tk;
        if (savedAraSignature == job->contentSignature && ! savedAraNotes.empty())
        {
            saved = savedAraNotes;
            useSaved = true;
        }
    }

    if (! analyseTake (tk, useSaved ? &saved : nullptr))
    {
        araFailed = true;
        status = Status::idle;
        ++changeCounter;
        return;
    }
    araReadFailures = 0;
    araLog ("read " + juce::String (job->regions.size()) + " clips, " + juce::String ((double) job->length / job->sampleRate, 1)
            + " s, " + juce::String ((int) getNotes().size()) + " notes");
    ++changeCounter;   // status stays "reading": the timer adopts the take
}

bool ScribaPitchProcessor::requestHostPosition (double takeSeconds)
{
   #if JucePlugin_Enable_ARA
    if (auto* r = getPlaybackRenderer<ScribaPlaybackRenderer>())
        if (auto* transport = r->getHostTransport())
        {
            const double start = take != nullptr ? (double) take->startSample / take->sampleRate : 0.0;
            transport->requestSetPlaybackPosition (juce::jmax (0.0, start + takeSeconds));
            return true;
        }
   #endif
    // REAPER without ARA: its own API moves the edit cursor
    if (reaperHost.isAvailable())
    {
        const double start = take != nullptr ? (double) take->startSample / take->sampleRate : 0.0;
        return reaperHost.moveCursorTo (start + takeSeconds);
    }
    return false;
}

bool ScribaPitchProcessor::requestHostPlayToggle()
{
   #if JucePlugin_Enable_ARA
    if (auto* r = getPlaybackRenderer<ScribaPlaybackRenderer>())
        if (auto* transport = r->getHostTransport())
        {
            if (transportPlaying.load()) transport->requestStopPlayback();
            else                         transport->requestStartPlayback();
            return true;
        }
   #endif
    return reaperHost.togglePlay();
}

void ScribaPitchProcessor::runRender()
{
    std::vector<sp::Note> ns;
    std::shared_ptr<const AnalysisData> an;
    std::shared_ptr<Take> tk;
    double from = 0, to = -1, sFrom = 0, sTo = -1;
    {
        const juce::ScopedLock sl (dataLock);
        if (! renderPending) return;
        renderPending = false;
        rendering = true;   // set under the lock: the preview timer never sees a gap
        ns = notes;
        an = analysis;
        tk = workerTake;
        from = dirtyFrom; to = dirtyTo;
        dirtyFrom = 0.0; dirtyTo = -1.0;
        sFrom = syncFrom; sTo = syncTo;
        syncFrom = 0.0; syncTo = -1.0;   // consumed below
    }
    const juce::ScopeGuard doneRendering { [this] { rendering = false; } };
    if (an == nullptr || tk == nullptr || ! tk->cacheReady.load() || to < from)
        return;

    sp::sortNotes (ns);
    const int len = (int) tk->cache[0].size();
    const double sr = tk->sampleRate;
    const int frontIdx = tk->front.load();
    const int backIdx = 1 - frontIdx;
    auto& back = tk->cache[backIdx];
    const auto& frontBuf = tk->cache[frontIdx];

    // Bring the back buffer up to date where the previous render changed the front.
    if (sTo >= sFrom)
    {
        const int a = juce::jlimit (0, len, (int) (sFrom * sr));
        const int b = juce::jlimit (0, len, (int) std::ceil (sTo * sr));
        std::copy (frontBuf.begin() + a, frontBuf.begin() + b, back.begin() + a);
    }

    const sp::RenderSource src { tk->data.data(), len, sr, &an->marks, &an->track };
    juce::int64 wFrom = 0, wTo = 0;
    sp::renderRange (src, ns, (juce::int64) (from * sr), (juce::int64) std::ceil (to * sr), back.data(), len, &wFrom, &wTo);

    {
        const juce::SpinLock::ScopedLockType l (audioLock);
        tk->front = backIdx;
    }
    {
        const juce::ScopedLock sl (dataLock);
        // what this render wrote must be copied into the other buffer before the next render
        const double wf = (double) wFrom / sr, wt = (double) wTo / sr;
        if (syncTo < syncFrom) { syncFrom = wf; syncTo = wt; }
        else { syncFrom = juce::jmin (syncFrom, wf); syncTo = juce::jmax (syncTo, wt); }
    }
    ++changeCounter;
}

void ScribaPitchProcessor::saveTakeToDisk (const Take& tk) const
{
    const auto folder = getTakesFolder();
    folder.createDirectory();
    const auto file = folder.getChildFile (tk.id + ".flac");
    file.deleteFile();

    juce::FlacAudioFormat flac;
    auto stream = std::make_unique<juce::FileOutputStream> (file);
    if (! stream->openedOk()) return;
    std::unique_ptr<juce::AudioFormatWriter> w (flac.createWriterFor (stream.get(), tk.sampleRate, 1, 24, {}, 0));
    if (w == nullptr) return;
    stream.release();
    const float* chans[] = { tk.data.data() };
    w->writeFromFloatArrays (chans, 1, (int) tk.length.load());
}

juce::File ScribaPitchProcessor::getTakesFolder()
{
   #if JUCE_MAC
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
   #else
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif
    return base.getChildFile ("Meteora Plugins").getChildFile ("Scriba Pitch").getChildFile ("Takes");
}

//==============================================================================
juce::String ScribaPitchProcessor::getStatusText() const
{
    switch (status.load())
    {
        case Status::idle:        return "READY";
        case Status::armed:       return "WAITING FOR PLAY";
        case Status::recording:   return "REC";
        case Status::analyzing:   return "ANALYZING";
        case Status::ready:       return "DONE";
        case Status::loading:     return "LOADING";
        case Status::missingTake: return "TAKE MISSING";
        case Status::reading:     return "READING TRACK";
    }
    return {};
}

juce::String ScribaPitchProcessor::getAnalyzeButtonText() const
{
    switch (status.load())
    {
        case Status::armed:     return "PRESS PLAY IN YOUR DAW";
        case Status::recording: return "STOP & ANALYZE";
        case Status::analyzing: return "ANALYZING...";
        case Status::loading:   return "LOADING...";
        case Status::reading:   return "READING THE TRACK...";
        case Status::ready:     return isAraMode() ? "READ AGAIN" : "ANALYZE AGAIN";
        case Status::idle:
        case Status::missingTake:
        default:                return isAraMode() ? "READ TRACK" : "ANALYZE";
    }
}

std::vector<sp::Note> ScribaPitchProcessor::getNotes() const
{
    const juce::ScopedLock sl (dataLock);
    return notes;
}

void ScribaPitchProcessor::applyNotes (std::vector<sp::Note> newNotes, bool keepAutoCorrectedFlag)
{
    sp::sortNotes (newNotes);
    {
        const juce::ScopedLock sl (dataLock);
        double f, t;
        const double total = workerTake != nullptr ? workerTake->seconds() : 0.0;
        if (sp::changedRange (notes, newNotes, total, f, t))
        {
            if (dirtyTo < dirtyFrom) { dirtyFrom = f; dirtyTo = t; }
            else { dirtyFrom = std::min (dirtyFrom, f); dirtyTo = std::max (dirtyTo, t); }
            renderPending = true;
        }
        notes = std::move (newNotes);
    }
    if (! keepAutoCorrectedFlag)
        autoCorrected = false;
    ++changeCounter;
    worker->poke();
}

void ScribaPitchProcessor::pushUndo()
{
    undoStack.push_back ({ getNotes(), autoCorrected, lastMovedCount });
    if (undoStack.size() > maxUndo)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void ScribaPitchProcessor::dropUndoIfUnchanged()
{
    if (undoStack.empty()) return;
    const auto current = getNotes();
    const auto& last = undoStack.back().notes;
    if (last.size() != current.size()) return;
    for (size_t i = 0; i < last.size(); ++i)
        if (! last[i].sameEdits (current[i])) return;
    undoStack.pop_back();
}

void ScribaPitchProcessor::undo()
{
    if (undoStack.empty()) return;
    redoStack.push_back ({ getNotes(), autoCorrected, lastMovedCount });
    auto prev = std::move (undoStack.back());
    undoStack.pop_back();
    applyNotes (std::move (prev.notes), true);
    autoCorrected = prev.autoCorrected;
    lastMovedCount = prev.moved;
}

void ScribaPitchProcessor::redo()
{
    if (redoStack.empty()) return;
    undoStack.push_back ({ getNotes(), autoCorrected, lastMovedCount });
    auto next = std::move (redoStack.back());
    redoStack.pop_back();
    applyNotes (std::move (next.notes), true);
    autoCorrected = next.autoCorrected;
    lastMovedCount = next.moved;
}

void ScribaPitchProcessor::automaticCorrection()
{
    if (autoCorrected && canUndo())
    {
        undo();
        return;
    }

    const auto scale = getScale();
    if (! scale.valid || getStatus() != Status::ready)
        return;

    pushUndo();
    auto ns = getNotes();
    const float amount = corrParam->get() / 100.0f;
    const float drift = driftParam->get() / 100.0f;
    const float human = humanizeParam->get() / 100.0f;
    const bool keepVib = keepVibParam->get();
    juce::Random rng (12345);

    int moved = 0;
    for (auto& note : ns)
    {
        const float centre = note.newCenter();
        const int target = scale.nearest (centre);
        if (target != (int) std::round (centre)) ++moved;

        note.shift = (float) target - std::round (note.origMidi) + (rng.nextFloat() * 2.0f - 1.0f) * human * 0.08f;
        note.centerAmt = amount;
        note.driftAmt = drift;
        note.vibAmt = keepVib ? 1.0f : 0.35f;
    }

    applyNotes (std::move (ns), true);
    autoCorrected = true;
    lastMovedCount = moved;
}

//==============================================================================
sp::KeyResult ScribaPitchProcessor::getDetectedKey() const
{
    const juce::ScopedLock sl (dataLock);
    return detectedKey;
}

bool ScribaPitchProcessor::isKeyManual() const { return keyModeParam->getIndex() == 1; }

int ScribaPitchProcessor::getEffectiveKey() const
{
    if (isKeyManual()) return manualKeyParam->getIndex();
    const auto l = getLinked();
    if (l.key >= 0) return l.key;   // the instruments analysed by Scriba win over the voice
    return getDetectedKey().key;
}

ScribaPitchProcessor::KeySource ScribaPitchProcessor::getKeySource() const
{
    if (isKeyManual())                return KeySource::manual;
    if (getLinked().key >= 0)         return KeySource::scriba;
    if (getDetectedKey().valid())     return KeySource::vocal;
    return KeySource::none;
}

meteora::link::Combined ScribaPitchProcessor::getLinked() const
{
    const juce::ScopedLock sl (dataLock);
    return linked;
}

void ScribaPitchProcessor::pollLink()
{
    const auto others = meteora::link::readOthers (linkPublisher.getInstanceId());
    auto c = meteora::link::combine (others, "Scriba");

    juce::String sig;
    sig << c.key << "/" << c.alt << "/" << juce::roundToInt (c.confidence * 100.0f) << "/" << c.sources.joinIntoString ("+");
    for (const auto& ch : c.chords) sig << "/" << ch.ppq << ch.name;

    if (sig != lastLinkSignature)
    {
        lastLinkSignature = sig;
        {
            const juce::ScopedLock sl (dataLock);
            linked = std::move (c);
        }
        ++changeCounter;
    }
    publishToLink();
}

void ScribaPitchProcessor::publishToLink()
{
    // Publish this vocal's own key, so Scriba (snare, chords...) can use it too.
    const auto det = getDetectedKey();
    if (! det.valid())
    {
        if (lastPublished.isNotEmpty()) { linkPublisher.withdraw(); lastPublished.clear(); }
        return;
    }
    meteora::link::Info info;
    info.plugin = "Scriba Pitch";
    info.instrument = "VOCALS";
    info.key = det.key;
    info.alt = det.alt;
    info.confidence = det.confidence;
    const auto sig = juce::String (info.key) + "/" + juce::String (info.alt) + "/" + juce::String (info.confidence);
    if (sig == lastPublished) return;
    linkPublisher.publish (info);
    lastPublished = sig;
}

sp::Scale ScribaPitchProcessor::getScale() const
{
    return sp::makeScale (getEffectiveKey(), scaleParam->getIndex());
}

bool ScribaPitchProcessor::isBypassed() const { return bypassParam->get(); }

std::shared_ptr<const AnalysisData> ScribaPitchProcessor::getAnalysis() const
{
    const juce::ScopedLock sl (dataLock);
    return analysis;
}

double ScribaPitchProcessor::getTakeSeconds() const
{
    const juce::ScopedLock sl (dataLock);
    if (workerTake != nullptr && workerTake->cacheReady.load())
        return (double) workerTake->cache[0].size() / workerTake->sampleRate;
    return take != nullptr ? take->seconds() : 0.0;
}

void ScribaPitchProcessor::getTempo (double& bpm, double& startPpq, int& num, int& den, bool& hasPpq) const
{
    const juce::ScopedLock sl (dataLock);
    const Take* t = workerTake != nullptr ? workerTake.get() : take.get();
    if (t == nullptr) { bpm = 120.0; startPpq = 0.0; num = 4; den = 4; hasPpq = false; return; }
    bpm = t->bpm; startPpq = t->startPpq; num = t->sigNum; den = t->sigDen; hasPpq = t->hasPpq;
}

void ScribaPitchProcessor::previewRendered (const sp::Note& note)
{
    renderedPreviewStart = note.outStart;
    renderedPreviewLength = juce::jmax (0.1, note.outLength());
    renderedPreviewPending = true;   // the timer fires it once the render has caught up
}

void ScribaPitchProcessor::previewNote (const sp::Note&, float targetMidi, bool hold)
{
    toneTarget = juce::jlimit (24.0f, 108.0f, targetMidi);
    toneHold = hold;
    ++toneTrigger;
}

//==============================================================================
juce::File ScribaPitchProcessor::exportMidiFile() const
{
    auto ns = getNotes();
    sp::sortNotes (ns);
    double bpm, startPpq; int num, den; bool hasPpq;
    getTempo (bpm, startPpq, num, den, hasPpq);
    if (bpm <= 0.0) bpm = 120.0;

    constexpr int tpq = 960;
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / bpm)), 0.0);
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (num, den), 0.0);
    const int key = getEffectiveKey();
    if (key >= 0)
    {
        static const int sharpsMajor[] = { 0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5 };
        const int rel = key < 12 ? key : (key - 12 + 3) % 12;
        seq.addEvent (juce::MidiMessage::keySignatureMetaEvent (sharpsMajor[rel], key >= 12), 0.0);
    }

    for (const auto& n : ns)
    {
        const int pitch = juce::jlimit (0, 127, (int) std::round (n.newCenter()));
        const double on = n.outStart * bpm / 60.0 * tpq;
        const double off = n.outEnd * bpm / 60.0 * tpq;
        seq.addEvent (juce::MidiMessage::noteOn (1, pitch, (juce::uint8) 100), on);
        seq.addEvent (juce::MidiMessage::noteOff (1, pitch), off);
    }
    seq.updateMatchedPairs();

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (tpq);
    mf.addTrack (seq);

    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Scriba Pitch - Melody.mid");
    file.deleteFile();
    juce::FileOutputStream out (file);
    if (out.openedOk())
        mf.writeTo (out);
    return file;
}

juce::File ScribaPitchProcessor::exportAudioFile() const
{
    std::shared_ptr<Take> tk;
    {
        const juce::ScopedLock sl (dataLock);
        tk = workerTake;
    }
    if (tk == nullptr || ! tk->cacheReady.load()) return {};

    const std::vector<float> copy = tk->cache[tk->front.load()];
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Scriba Pitch - Vocal.wav");
    file.deleteFile();

    juce::WavAudioFormat wav;
    auto stream = std::make_unique<juce::FileOutputStream> (file);
    if (! stream->openedOk()) return {};
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream.get(), tk->sampleRate, 1, 24, {}, 0));
    if (w == nullptr) return {};
    stream.release();
    const float* chans[] = { copy.data() };
    w->writeFromFloatArrays (chans, 1, (int) copy.size());
    return file;
}

//==============================================================================
void ScribaPitchProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement root ("ScribaPitchState");
    root.setAttribute ("version", 1);
    if (auto p = params.copyState().createXml())
        root.addChildElement (p.release());

    auto* s = root.createNewChildElement ("Session");
    s->setAttribute ("snap", snapMode.load());
    s->setAttribute ("tool", toolIndex.load());

    std::shared_ptr<Take> tk;
    std::vector<sp::Note> ns;
    sp::KeyResult key;
    PendingSession ps;
    {
        const juce::ScopedLock sl (dataLock);
        tk = workerTake;
        ns = notes;
        key = detectedKey;
        ps = pendingSession;
    }

    const bool haveTake = tk != nullptr && tk->cacheReady.load();
    const bool araTake = haveTake && tk->id == "ara";
    const bool keepPending = ! haveTake && ps.takeId.isNotEmpty();
    juce::String keepAraSig;
    std::vector<sp::Note> keepAraNotes;
    if (! haveTake)
    {
        const juce::ScopedLock sl (dataLock);
        keepAraSig = savedAraSignature;
        keepAraNotes = savedAraNotes;
    }

    auto writeNotes = [s] (const std::vector<sp::Note>& list)
    {
        auto* nx = s->createNewChildElement ("Notes");
        for (const auto& n : list)
        {
            auto* e = nx->createNewChildElement ("N");
            e->setAttribute ("id", n.id);
            e->setAttribute ("inS", n.inStart);   e->setAttribute ("inE", n.inEnd);
            e->setAttribute ("outS", n.outStart); e->setAttribute ("outE", n.outEnd);
            e->setAttribute ("orig", n.origMidi); e->setAttribute ("shift", n.shift);
            e->setAttribute ("ca", n.centerAmt);  e->setAttribute ("da", n.driftAmt);
            e->setAttribute ("va", n.vibAmt);     e->setAttribute ("fo", n.formant);
            e->setAttribute ("ga", n.gainDb);
            if (n.hasDrawing())
            {
                juce::String d;
                for (size_t k = 0; k < n.drawU.size(); ++k)
                    d << juce::String (n.drawU[k], 5) << ":" << juce::String (n.drawOff[k], 4) << ";";
                e->setAttribute ("dr", d);
            }
        }
    };

    if (araTake || (! haveTake && keepAraSig.isNotEmpty()))
    {
        // ARA: the audio comes from the DAW's own files; only the notes are saved
        s->setAttribute ("araSig", araTake ? araContentSig : keepAraSig);
        writeNotes (araTake ? ns : keepAraNotes);
    }
    else if (haveTake || keepPending)
    {
        s->setAttribute ("takeId", haveTake ? tk->id : ps.takeId);
        s->setAttribute ("start", juce::String (haveTake ? tk->startSample : ps.startSample));
        s->setAttribute ("startPpq", haveTake ? tk->startPpq : ps.startPpq);
        s->setAttribute ("bpm", haveTake ? tk->bpm : ps.bpm);
        s->setAttribute ("num", haveTake ? tk->sigNum : ps.num);
        s->setAttribute ("den", haveTake ? tk->sigDen : ps.den);
        s->setAttribute ("hasPpq", haveTake ? tk->hasPpq : ps.hasPpq);
        s->setAttribute ("key", key.key);
        s->setAttribute ("alt", key.alt);
        s->setAttribute ("conf", key.confidence);
        writeNotes (ns);
    }

    copyXmlToBinary (root, dest);
}

void ScribaPitchProcessor::setStateInformation (const void* data, int size)
{
    auto root = getXmlFromBinary (data, size);
    if (root == nullptr || ! root->hasTagName ("ScribaPitchState"))
        return;

    if (auto* p = root->getChildByName (params.state.getType()))
        params.replaceState (juce::ValueTree::fromXml (*p));

    auto* s = root->getChildByName ("Session");
    if (s == nullptr) return;

    snapMode = s->getIntAttribute ("snap", 0);
    toolIndex = s->getIntAttribute ("tool", 0);

    auto readNotes = [s]
    {
        std::vector<sp::Note> list;
        if (auto* nx = s->getChildByName ("Notes"))
        {
            for (auto* e : nx->getChildIterator())
            {
                sp::Note n;
                n.id = e->getIntAttribute ("id");
                n.inStart = e->getDoubleAttribute ("inS");   n.inEnd = e->getDoubleAttribute ("inE");
                n.outStart = e->getDoubleAttribute ("outS"); n.outEnd = e->getDoubleAttribute ("outE");
                n.origMidi = (float) e->getDoubleAttribute ("orig", 60.0);
                n.shift = (float) e->getDoubleAttribute ("shift");
                n.centerAmt = (float) e->getDoubleAttribute ("ca");
                n.driftAmt = (float) e->getDoubleAttribute ("da");
                n.vibAmt = (float) e->getDoubleAttribute ("va", 1.0);
                n.formant = (float) e->getDoubleAttribute ("fo");
                n.gainDb = (float) e->getDoubleAttribute ("ga");
                for (const auto& pt : juce::StringArray::fromTokens (e->getStringAttribute ("dr"), ";", {}))
                {
                    if (! pt.containsChar (':')) continue;
                    n.drawU.push_back (pt.upToFirstOccurrenceOf (":", false, false).getFloatValue());
                    n.drawOff.push_back (pt.fromFirstOccurrenceOf (":", false, false).getFloatValue());
                }
                if (n.drawU.size() < 2) n.clearDrawing();
                list.push_back (n);
            }
        }
        return list;
    };

    // ARA project: the notes wait for the DAW to hand over the same track again.
    const auto araSig = s->getStringAttribute ("araSig");
    if (araSig.isNotEmpty())
    {
        auto list = readNotes();
        bool applyNow = false;
        {
            const juce::ScopedLock sl (dataLock);
            savedAraSignature = araSig;
            savedAraNotes = list;
            applyNow = workerTake != nullptr && workerTake->cacheReady.load() && workerTake->id == "ara" && araSig == araContentSig;
        }
        if (applyNow)
            applyNotes (std::move (list));
        ++changeCounter;
        return;
    }

    const auto takeId = s->getStringAttribute ("takeId");
    if (takeId.isEmpty()) return;

    PendingSession ps;
    ps.takeId = takeId;
    ps.startSample = s->getStringAttribute ("start").getLargeIntValue();
    ps.startPpq = s->getDoubleAttribute ("startPpq");
    ps.bpm = s->getDoubleAttribute ("bpm", 120.0);
    ps.num = s->getIntAttribute ("num", 4);
    ps.den = s->getIntAttribute ("den", 4);
    ps.hasPpq = s->getBoolAttribute ("hasPpq");
    ps.notes = readNotes();

    {
        const juce::ScopedLock sl (dataLock);
        pendingSession = ps;
        detectedKey.key = s->getIntAttribute ("key", -1);
        detectedKey.alt = s->getIntAttribute ("alt", -1);
        detectedKey.confidence = (float) s->getDoubleAttribute ("conf");
        notes = ps.notes;
    }

    status = Status::loading;
    worker->wantLoad = true;
    worker->poke();
    ++changeCounter;
}

//==============================================================================
juce::AudioProcessorEditor* ScribaPitchProcessor::createEditor()
{
    return new ScribaPitchEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScribaPitchProcessor();
}
