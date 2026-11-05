#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Engine/Renderer.h"
#include "Link/MeteoraLink.h"
#include "ARA/ScribaARA.h"
#include "Host/ReaperHost.h"

//==============================================================================
/** A captured vocal take plus its corrected render (double-buffered). */
struct Take
{
    std::vector<float> data;                 // captured mono audio (preallocated)
    std::atomic<juce::int64> length { 0 };   // samples captured so far
    double sampleRate = 44100.0;
    juce::int64 startSample = 0;             // host timeline position of sample 0
    double startPpq = 0.0, bpm = 120.0;
    int sigNum = 4, sigDen = 4;
    bool hasPpq = false;
    juce::String id;

    std::vector<float> cache[2];             // corrected audio; the audio thread reads cache[front]
    std::atomic<int> front { 0 };
    std::atomic<bool> cacheReady { false };

    double seconds() const noexcept { return (double) length.load() / sampleRate; }
};

/** Pitch analysis of the take, shared read-only between threads. */
struct AnalysisData
{
    sp::PitchTrack track;
    sp::Marks marks;
};

//==============================================================================
class ScribaPitchProcessor  : public juce::AudioProcessor,
                             #if JucePlugin_Enable_ARA
                              public juce::AudioProcessorARAExtension,
                             #endif
                              private juce::Timer
{
public:
    enum class Status { idle, armed, recording, analyzing, ready, loading, missingTake, reading };

    ScribaPitchProcessor();
    ~ScribaPitchProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Scriba Pitch"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &reaperHost; }

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==========================================================================
    // Message-thread API used by the editor
    juce::AudioProcessorValueTreeState params;

    Status getStatus() const noexcept { return status.load(); }
    juce::String getStatusText() const;
    juce::String getAnalyzeButtonText() const;
    void pressAnalyze();

    std::vector<sp::Note> getNotes() const;
    /** Replaces the notes and renders what changed in the background. */
    void applyNotes (std::vector<sp::Note> newNotes, bool keepAutoCorrectedFlag = false);
    void pushUndo();
    void dropUndoIfUnchanged();
    void undo();
    void redo();
    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }

    void automaticCorrection();
    bool isAutoCorrected() const noexcept { return autoCorrected; }
    int  getLastMovedCount() const noexcept { return lastMovedCount; }

    sp::KeyResult getDetectedKey() const;   // from this vocal
    int getEffectiveKey() const;          // -1 if unknown
    bool isKeyManual() const;

    enum class KeySource { none, manual, scriba, vocal };
    KeySource getKeySource() const;
    /** Key and chords published by the Scriba instances in this DAW. */
    meteora::link::Combined getLinked() const;
    /** Reads the other instances now (normally done once a second by the timer). */
    void pollLink();
    sp::Scale getScale() const;
    bool isBypassed() const;

    /** A/B: while true the original voice is heard (hold the button or the B key). */
    void setHoldOriginal (bool on) noexcept { holdOriginal = on; }
    bool isHoldingOriginal() const noexcept  { return holdOriginal.load(); }

    //==========================================================================
    // ARA2 (instant reading of the track, transport control)
    /** True when the DAW gives this instance its track through ARA. */
    bool isAraMode() const noexcept;
    /** Moves the DAW cursor to this time of the take. False if the DAW does not allow it (no ARA). */
    bool requestHostPosition (double takeSeconds);
    /** Starts or stops the DAW. False without ARA. */
    bool requestHostPlayToggle();
    /** Re-reads and re-analyses the track (ARA). */
    void rereadTrack();
    /** One line about the ARA connection, for the screen (empty when all is fine). */
    juce::String getAraDiagnostics() const;
    /** Log of the ARA steps, to send when something does not work. */
    static juce::File getLogFile();

    std::shared_ptr<const AnalysisData> getAnalysis() const;
    double getTakeSeconds() const;
    void   getTempo (double& bpm, double& startPpq, int& num, int& den, bool& hasPpq) const;

    double getPlayheadSeconds() const noexcept { return playheadSeconds.load(); }
    bool   isTransportPlaying() const noexcept { return transportPlaying.load(); }
    bool   isSampleRateMismatch() const noexcept { return rateMismatch.load(); }

    /** Like Melodyne: plays a clean tone at the note's pitch while it is edited
        (not the audio of that part of the take). hold = keeps sounding until stopPreview(). */
    void previewNote (const sp::Note& note, float targetMidi, bool hold = false);
    void stopPreview() noexcept { toneHold = false; }
    /** Plays the corrected render of this note as soon as its background render is done. */
    void previewRendered (const sp::Note& note);

    int getChangeCounter() const noexcept { return changeCounter.load(); }

    juce::File exportMidiFile() const;
    juce::File exportAudioFile() const;

    std::atomic<int> snapMode { 0 };   // 0 chromatic (default, like Melodyne), 1 scale, 2 off
    std::atomic<int> toolIndex { 0 };

    static juce::File getTakesFolder();

private:
    //==========================================================================
    class Worker;
    friend class Worker;

    void timerCallback() override;
    void beginAnalysis();
    void adoptPendingTake();

    // worker-thread jobs
    void runAnalysis();
    void runLoad();
    void runRender();
    void runAraRead();
    /** Analyses tk on the worker. Keeps savedNotes when given (session reload). */
    bool analyseTake (const std::shared_ptr<Take>& tk, const std::vector<sp::Note>* savedNotes);
    void saveTakeToDisk (const Take&) const;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    //==========================================================================
    std::atomic<Status> status { Status::idle };
    Status statusBeforeArm = Status::idle;

    // Takes: the message thread owns the shared_ptrs; the audio thread uses raw
    // pointers that are only swapped while holding audioLock.
    std::shared_ptr<Take> take, pendingTake;
    Take* audioTake = nullptr;
    Take* audioPending = nullptr;
    juce::SpinLock audioLock;

    mutable juce::CriticalSection dataLock;   // notes / analysis / render queue
    std::vector<sp::Note> notes;
    std::shared_ptr<const AnalysisData> analysis;
    std::shared_ptr<Take> workerTake;          // take the worker analyses / renders
    sp::KeyResult detectedKey;
    double dirtyFrom = 0.0, dirtyTo = -1.0;
    bool renderPending = false;
    double syncFrom = 0.0, syncTo = -1.0;      // range where back buffer is stale

    struct PendingSession
    {
        juce::String takeId;
        std::vector<sp::Note> notes;
        juce::int64 startSample = 0;
        double startPpq = 0, bpm = 120;
        int num = 4, den = 4;
        bool hasPpq = false;
    } pendingSession;

    struct UndoEntry { std::vector<sp::Note> notes; bool autoCorrected = false; int moved = 0; };
    std::vector<UndoEntry> undoStack, redoStack;
    bool autoCorrected = false;
    int lastMovedCount = 0;

    std::atomic<bool> stopRequested { false };
    std::atomic<bool> cancelAnalysis { false };
    std::atomic<int>  changeCounter { 0 };
    std::atomic<double> playheadSeconds { 0.0 };
    std::atomic<bool> transportPlaying { false };
    std::atomic<bool> holdOriginal { false };
    std::atomic<bool> rateMismatch { false };
    double currentSampleRate = 44100.0;
    juce::int64 fallbackPosition = 0;

    // Preview voice
    // Reference tone (note preview)
    std::atomic<float> toneTarget { 60.0f };
    std::atomic<int>   toneTrigger { 0 };
    std::atomic<bool>  toneHold { false };
    int    toneSeen = 0;
    bool   toneActive = false, toneHeldMode = false;
    double tonePhase = 0.0, toneAge = 0.0, toneLength = 0.0;
    float  toneMidiNow = 60.0f, toneEnv = 0.0f;

    std::atomic<int> previewTrigger { 0 };
    std::atomic<double> previewStart { 0.0 }, previewLength { 0.0 };
    std::atomic<float> previewRatio { 1.0f };
    std::atomic<bool>  previewFromCache { false };
    bool   previewCacheNow = false;
    double renderedPreviewStart = 0.0, renderedPreviewLength = 0.0;
    bool   renderedPreviewPending = false;
    std::atomic<bool> rendering { false };
    int    previewSeen = 0;
    bool   previewActive = false;
    double previewSrcStart = 0.0, previewOut = 0.0, previewOutLength = 0.0;
    float  previewRatioNow = 1.0f;

    std::unique_ptr<Worker> worker;

    // Non-ARA hosts: arm automatically after insertion, so pressing play is enough.
    int  ticksSinceStart = 0;
    bool autoArmDone = false, userDisarmed = false;

    // ARA: what is on the track, and the job that reads it
    struct AraRegion
    {
        void* source = nullptr;
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::int64 songStart = 0, length = 0;   // samples at the playback rate
        juce::int64 sourceStart = 0;             // samples at the source rate
        double sourceRate = 44100.0;
    };
    struct AraJob
    {
        std::vector<AraRegion> regions;
        juce::int64 start = 0, length = 0;
        double sampleRate = 44100.0;
        juce::String signature, contentSignature;
        std::atomic<bool> finished { false };
    };
    std::shared_ptr<AraJob> araJob;
    juce::String araSignature, araContentSig, araSeenSignature, savedAraSignature;
    std::vector<sp::Note> savedAraNotes;
    int araGeneration = -1, araTicks = 0;
    bool araForceReread = false;
    std::atomic<bool> araBound { false }, araFailed { false };
    std::atomic<double> hostBpm { 120.0 };
    std::atomic<int> hostSigNum { 4 }, hostSigDen { 4 };
    void pollAra();
    void araLog (const juce::String& line) const;
    // what the DAW gave us (message thread)
    bool diagBound = false, diagRenderer = false;
    int  diagRegions = 0, diagReadable = 0;
    juce::String lastLoggedState;
    std::atomic<int> araReadFailures { 0 };
    /** Where the clips are (changes when a clip moves) and what they contain
        (stable across sessions: built from the DAW's persistent file IDs). */
    juce::String currentAraSignature (bool& allReadable, juce::String& content, juce::int64& firstStart) const;

    ReaperHost reaperHost;
    meteora::link::Publisher linkPublisher;
    meteora::link::Combined linked;          // guarded by dataLock
    juce::String lastLinkSignature, lastPublished;
    int linkTicks = 0;
    void publishToLink();

    juce::AudioParameterBool*   bypassParam = nullptr;
    juce::AudioParameterChoice* keyModeParam = nullptr;    // 0 auto, 1 manual
    juce::AudioParameterChoice* manualKeyParam = nullptr;  // 0..23
    juce::AudioParameterChoice* scaleParam = nullptr;
    juce::AudioParameterFloat*  corrParam = nullptr;
    juce::AudioParameterFloat*  driftParam = nullptr;
    juce::AudioParameterFloat*  humanizeParam = nullptr;
    juce::AudioParameterBool*   keepVibParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScribaPitchProcessor)
};
