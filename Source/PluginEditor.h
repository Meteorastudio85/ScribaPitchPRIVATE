#pragma once

#include "PluginProcessor.h"
#include "UI/PianoRoll.h"
#include "UI/Widgets.h"

//==============================================================================
/** The whole interface, laid out at 1280 x 800 and scaled by the editor. */
class MainView  : public juce::Component
{
public:
    explicit MainView (ScribaPitchProcessor&);
    ~MainView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    void tick();   // called by the editor timer (30 Hz)

    static constexpr int designW = 1280, designH = 800;

private:
    using Attach = juce::AudioProcessorValueTreeState;

    void refresh (bool fitView);
    void updateInspector();
    void updateAutoSection();
    void setTool (int);
    void setSnap (int);
    void setSelectedLook (juce::TextButton&, bool on);
    void styleTextButton (juce::TextButton&, float fontSize);

    ScribaPitchProcessor& proc;
    juce::SharedResourcePointer<theme::FontSet> fonts;
    theme::LookAndFeel lnf;
    juce::Image logo;

    // header
    IconButton undoBtn { "Undo", "M9 14L4 9l5-5M4 9h11a5 5 0 0 1 0 10h-3", true };
    IconButton redoBtn { "Redo", "M15 14l5-5-5-5M20 9H9a5 5 0 0 0 0 10h3", true };
    HoldButton abButton;
    juce::TextButton analyzeBtn { "ANALYZE" };
    bool bKeyHeld = false;

    // left panel
    KeyWheel wheel;
    juce::TextButton keyAutoBtn { "AUTO KEY" }, keyManualBtn { "MANUAL KEY" };
    juce::ComboBox manualKeyBox, scaleBox;
    ToggleSwitch bypassSwitch;
    BigActionButton autoBtn;
    juce::Slider corrSlider, driftSlider, humanSlider;
    juce::ToggleButton keepVibBtn { "KEEP VIBRATO & SLIDES" };

    std::unique_ptr<Attach::ComboBoxAttachment> manualKeyAtt, scaleAtt;
    std::unique_ptr<Attach::ButtonAttachment> bypassAtt, keepVibAtt;
    void setKeyMode (bool manual);
    std::unique_ptr<Attach::SliderAttachment> corrAtt, driftAtt, humanAtt;

    // editor
    juce::OwnedArray<IconButton> toolButtons;
    juce::TextButton snapButtons[3] { juce::TextButton ("CHROMATIC"), juce::TextButton ("SCALE"), juce::TextButton ("OFF") };
    PianoRoll roll;

    // inspector
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
        juce::String label, valueText;
    };
    Knob knobs[6];
    ExportButton midiBtn { "DRAG MIDI", theme::tonic, true };
    ExportButton audioBtn { "DRAG AUDIO", theme::text, false };

    int lastCounter = -1;
    juce::String lastSignature;
    bool updatingInspector = false;
    bool hadNotes = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainView)
};

//==============================================================================
class ScribaPitchEditor  : public juce::AudioProcessorEditor,
                           public juce::DragAndDropContainer,
                           private juce::Timer
{
public:
    explicit ScribaPitchEditor (ScribaPitchProcessor&);
    ~ScribaPitchEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { view.tick(); }

    MainView view;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScribaPitchEditor)
};
