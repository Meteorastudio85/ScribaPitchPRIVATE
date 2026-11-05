#pragma once

#include "Theme.h"
#include "../PluginProcessor.h"
#include <functional>

/** The note editor: Melodyne-style blobs with their pitch curve, drag to move,
    drag the edges to stretch, and the out-of-key notes flagged in orange. */
class PianoRoll  : public juce::Component
{
public:
    enum Tool { toolMain = 0, toolPitch, toolDraw, toolModulation, toolDrift, toolFormant, toolAmplitude, toolTiming, toolSeparation, numTools };
    static juce::String toolName (int tool);
    static juce::String toolHint (int tool);
    static const char* toolIcon (int tool);

    explicit PianoRoll (ScribaPitchProcessor&);

    void setTool (int t) { tool = t; updateCursor ({}); }
    void setSnap (int s) { snap = s; }

    /** Reloads notes, analysis, key and scale from the processor. */
    void refreshFromProcessor (bool fitView);
    void updatePlayhead();

    int getSelectedId() const noexcept { return selectedId; }
    const sp::Note* getSelectedNote() const noexcept;
    /** Changes the selected note (the caller pushes undo when an edit starts). */
    void editSelected (const std::function<void (sp::Note&)>& change);
    void clampTime (sp::Note& n, double newStart, double newEnd) const;

    std::function<void()> onSelectionChanged;
    std::function<void (double takeSeconds)> onSeek;   // click on the ruler
    std::function<void()> onPlayToggle;                // double-click on the ruler

    /** Shows a short message over the roll for a few seconds. */
    void flash (const juce::String& message);

    /** Where a note is drawn (component coordinates), or empty if unknown. */
    juce::Rectangle<float> getNoteBounds (int id) const;
    const std::vector<sp::Note>& getShownNotes() const noexcept { return notes; }
    /** Scrolls so that this time and pitch are in the middle of the roll. */
    void showAt (double seconds, float midi);

    void paint (juce::Graphics&) override;
    void resized() override {}
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Zone { none, body, left, right };
    struct Hit { int index = -1; Zone zone = Zone::none; };

    juce::Rectangle<float> gridArea() const;
    float  xForTime (double t) const;
    double timeForX (float x) const;
    float  yForMidi (float midi) const;
    float  midiForY (float y) const;
    juce::Rectangle<float> noteRect (const sp::Note&) const;
    Hit hitNote (juce::Point<float>) const;
    int indexOfId (int id) const;
    sp::Tuning tuningFor (const sp::Note&) const;
    void paintSummary (juce::Graphics&, juce::Rectangle<float> grid);
    static juce::Colour colourFor (sp::Tuning);
    float laneH() const noexcept { return chords.empty() ? 0.0f : 20.0f; }
    float topH() const noexcept  { return rulerH + laneH(); }
    void paintChords (juce::Graphics&, juce::Rectangle<float> grid);
    float snapPitch (float desired) const;
    void commit();
    void select (int id);
    void splitNoteAt (int index, double t);
    void updateCursor (juce::Point<float>);
    int  noteIndexAtTime (double t) const;
    /** Pencil: draws (or erases) the pitch curve along the mouse path a -> b. Returns the last note touched. */
    int  drawStroke (juce::Point<float> a, juce::Point<float> b, bool erase);
    void clampView();

    void paintRows (juce::Graphics&, juce::Rectangle<float> grid);
    void paintBeats (juce::Graphics&, juce::Rectangle<float> grid, juce::Rectangle<float> ruler);
    void paintNote (juce::Graphics&, const sp::Note&, bool selected);

    ScribaPitchProcessor& proc;
    std::vector<sp::Note> notes;
    std::shared_ptr<const AnalysisData> analysis;
    sp::Scale scale;
    int songKey = -1;
    std::vector<meteora::link::Chord> chords;   // from Scriba
    double totalSeconds = 0.0;
    double bpm = 120.0, startPpq = 0.0;
    int sigNum = 4, sigDen = 4;

    int tool = toolMain, snap = 0;
    int selectedId = -1, hoverId = -1;

    // view
    static constexpr float keysW = 56.0f, rulerH = 24.0f, rowH = 24.0f;
    double pxPerSec = 110.0, scrollSec = 0.0;
    float topMidi = 76.0f;
    float playheadX = -1.0f;

    juce::String flashText;
    juce::uint32 flashUntil = 0;

    // drag state
    bool drawing = false, drawErase = false;
    juce::Point<float> lastDrawPos;
    int drawLastId = -1;
    bool dragging = false, panning = false;
    Zone dragZone = Zone::none;
    sp::Note dragOrig;
    int lastPreviewSemi = 0;
    double panScroll = 0.0;
    float panTop = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};
