#pragma once

#include "Theme.h"
#include <functional>

/** Circle of fifths with the detected key (cyan) and its alternative (dashed). */
class KeyWheel  : public juce::Component
{
public:
    void setKey (int key, int alt, float confidence);
    void paint (juce::Graphics&) override;

private:
    int key = -1, alt = -1;
    float confidence = 0.0f;
};

/** ON/OFF pill switch. */
class ToggleSwitch  : public juce::Button
{
public:
    ToggleSwitch() : juce::Button ("switch") { setClickingTogglesState (true); }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    juce::Colour onColour { theme::accent };
};

/** The big two-line action button (AUTOMATIC CORRECTION). */
class BigActionButton  : public juce::Button
{
public:
    BigActionButton() : juce::Button ("action") {}
    void setTexts (const juce::String& title, const juce::String& sub);
    void setColours (juce::Colour background, juce::Colour foreground, juce::Colour border);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    juce::String title, sub;
    juce::Colour bgCol { theme::accent }, fgCol { theme::ink }, borderCol { juce::Colours::transparentBlack };
};

/** A/B: hold it down to hear the original voice, release to go back to the corrected one. */
class HoldButton  : public juce::Button
{
public:
    HoldButton() : juce::Button ("A/B") { setTooltip ("Hold to hear the original voice (or hold the B key)"); }
    std::function<void (bool)> onHold;
    void setHeldFromKey (bool held) { keyHeld = held; repaint(); }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void buttonStateChanged() override;

private:
    bool keyHeld = false, wasDown = false;
};

/** Square button with a stroked 24x24 icon. */
class IconButton  : public juce::Button
{
public:
    IconButton (const juce::String& name, const char* svgPath, bool framed);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    juce::Path icon;
    bool framed;
};

/** DRAG MIDI / DRAG AUDIO: drag it onto the DAW, or click to save the file. */
class ExportButton  : public juce::Button
{
public:
    ExportButton (const juce::String& label, juce::Colour colour, bool highlighted);
    std::function<juce::File()> makeFile;
    juce::String saveDialogTitle, saveWildcard;

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void mouseDrag (const juce::MouseEvent&) override;
    using juce::Button::clicked;
    void clicked() override;

private:
    juce::Colour colour;
    bool strong;
    bool dragging = false;
    std::unique_ptr<juce::FileChooser> chooser;
};
