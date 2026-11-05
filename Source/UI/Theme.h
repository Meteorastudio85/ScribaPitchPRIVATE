#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme
{
    // Scriba (Align) palette: deep navy, rounded bordered cards, teal accent
    const juce::Colour bg        { 0xff0b111c };
    const juce::Colour header    { 0xff0b111c };
    const juce::Colour panel     { 0xff111a29 };   // card
    const juce::Colour panel2    { 0xff16203a };   // raised element in a card
    const juce::Colour field     { 0xff0d1522 };
    const juce::Colour line      { 0xff1d2a40 };   // card border
    const juce::Colour line2     { 0xff27364f };
    const juce::Colour gridMinor { 0xff16213a };
    const juce::Colour text      { 0xffe8edf5 };
    const juce::Colour text2     { 0xffc5cedb };
    const juce::Colour text3     { 0xffa8b3c4 };
    const juce::Colour muted     { 0xff7d8aa0 };
    const juce::Colour dim       { 0xff5f6c82 };
    const juce::Colour rowKey    { 0xff121c2d };
    const juce::Colour rowOut    { 0xff0d1522 };
    const juce::Colour primary   { 0xff2bd4bd };   // teal: buttons, selection, switches
    const juce::Colour primaryInk{ 0xff06201d };   // text on teal
    const juce::Colour accent    { 0xffff9a3c };   // orange: only "off by a few cents" and warnings
    const juce::Colour tonic     { 0xff22d3ee };   // cyan: tonic row, chords
    const juce::Colour inKey     { 0xff9be4ff };   // light azure: note in the key and in tune
    const juce::Colour noKeyNote { 0xff8a97ab };   // grey: no key known yet
    const juce::Colour wrong     { 0xffef4452 };   // red: note out of the key
    const juce::Colour ink       { 0xff1a0f05 };   // text on orange
    const juce::Colour white     { 0xfff5f7fa };

    /** A Scriba card: rounded, filled, thin border. */
    void drawCard (juce::Graphics&, juce::Rectangle<float>, float radius = 12.0f);

    enum class Weight { semiBold, bold, black };

    /** The embedded Nunito typefaces (shared; the editor keeps one alive while open). */
    struct FontSet
    {
        FontSet();
        juce::Typeface::Ptr semiBold, bold, black;
    };

    juce::Font font (float height, Weight w = Weight::bold, float tracking = 0.0f);

    /** Draws text with Scriba's wide letter-spacing (tracking in em). */
    void drawText (juce::Graphics&, const juce::String&, juce::Rectangle<float>, float height,
                   Weight, juce::Colour, float tracking = 0.12f,
                   juce::Justification = juce::Justification::centredLeft);

    juce::Path iconPath (const char* svgPathData);   // 24x24 stroke icon

    //==========================================================================
    class LookAndFeel  : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();

        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider&) override;
        void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float sliderPos,
                               float minPos, float maxPos, juce::Slider::SliderStyle, juce::Slider&) override;
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
        void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
        juce::Font getComboBoxFont (juce::ComboBox&) override;
        void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
        juce::Font getPopupMenuFont() override;
        void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
        void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        juce::Font getLabelFont (juce::Label&) override;
    };
}
