#pragma once

// ARA2: the DAW gives Scriba Pitch direct access to the audio of the track it is
// inserted on, so the vocal is read and analysed in a few seconds without
// playing the song, and the plugin can move the DAW's cursor.
//
// The engine does not change: the processor reads the playback regions into a
// "take" laid out on the song timeline (the same thing capture produces) and the
// corrected voice is played from the render cache, as without ARA.

#include <juce_audio_processors/juce_audio_processors.h>

#if JucePlugin_Enable_ARA


class ScribaPlaybackRenderer  : public juce::ARAPlaybackRenderer
{
public:
    using juce::ARAPlaybackRenderer::ARAPlaybackRenderer;

    // The processor writes the voice from its cache right after this.
    bool processBlock (juce::AudioBuffer<float>& buffer, juce::AudioProcessor::Realtime,
                       const juce::AudioPlayHead::PositionInfo&) noexcept override
    {
        buffer.clear();
        return true;
    }

    using juce::ARAPlaybackRenderer::processBlock;

    ARA::PlugIn::HostPlaybackController* getHostTransport() const noexcept
    {
        auto* dc = getDocumentController();
        return dc != nullptr ? dc->getHostPlaybackController() : nullptr;
    }
};

class ScribaDocumentController  : public juce::ARADocumentControllerSpecialisation
{
public:
    using juce::ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

    /** Increases after every edit of the ARA model (regions moved, files replaced...). */
    static std::atomic<int>& modelGeneration() noexcept
    {
        static std::atomic<int> g { 0 };
        return g;
    }

protected:
    void didEndEditing (juce::ARADocument*) override { ++modelGeneration(); }

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override
    {
        return new ScribaPlaybackRenderer (getDocumentController());
    }

    // The notes are saved with the plugin state, not in the ARA archive.
    bool doRestoreObjectsFromStream (juce::ARAInputStream&, const juce::ARARestoreObjectsFilter*) noexcept override { return true; }
    bool doStoreObjectsToStream (juce::ARAOutputStream&, const juce::ARAStoreObjectsFilter*) noexcept override { return true; }
};

#endif
