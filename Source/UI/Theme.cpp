#include "Theme.h"
#include "BinaryData.h"

namespace theme
{

FontSet::FontSet()
    : semiBold (juce::Typeface::createSystemTypefaceFor (BinaryData::NunitoSemiBold_ttf, BinaryData::NunitoSemiBold_ttfSize)),
      bold     (juce::Typeface::createSystemTypefaceFor (BinaryData::NunitoBold_ttf, BinaryData::NunitoBold_ttfSize)),
      black    (juce::Typeface::createSystemTypefaceFor (BinaryData::NunitoBlack_ttf, BinaryData::NunitoBlack_ttfSize))
{
}

static juce::Typeface::Ptr typefaceFor (Weight w)
{
    const juce::SharedResourcePointer<FontSet> fonts;   // kept alive by the open editor
    switch (w)
    {
        case Weight::semiBold: return fonts->semiBold;
        case Weight::black:    return fonts->black;
        case Weight::bold:
        default:               return fonts->bold;
    }
}

juce::Font font (float height, Weight w, float tracking)
{
    return juce::Font (juce::FontOptions (typefaceFor (w)).withHeight (height)).withExtraKerningFactor (tracking);
}

void drawText (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, float height,
               Weight w, juce::Colour c, float tracking, juce::Justification j)
{
    g.setColour (c);
    g.setFont (font (height, w, tracking));
    g.drawText (s, r, j, false);
}

void drawCard (juce::Graphics& g, juce::Rectangle<float> r, float radius)
{
    g.setColour (panel);
    g.fillRoundedRectangle (r, radius);
    g.setColour (line);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

juce::Path iconPath (const char* svg)
{
    return juce::Drawable::parseSVGPath (svg);
}

//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (juce::PopupMenu::backgroundColourId, panel2);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, line2);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::ComboBox::backgroundColourId, field);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, line2);
    setColour (juce::ComboBox::arrowColourId, muted);
    setColour (juce::Label::textColourId, text);
    setColour (juce::TextButton::buttonColourId, panel2);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TooltipWindow::backgroundColourId, panel2);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, line2);
    setColour (juce::Slider::rotarySliderFillColourId, primary);
    setColour (juce::Slider::trackColourId, primary);
}

void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                    float a0, float a1, juce::Slider& s)
{
    const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
    const float radius = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f - 3.0f;
    const auto c = r.getCentre();
    const bool bipolar = s.getProperties()["bipolar"];
    const auto col = s.findColour (juce::Slider::rotarySliderFillColourId);

    juce::Path bgArc;
    bgArc.addCentredArc (c.x, c.y, radius, radius, 0.0f, a0, a1, true);
    g.setColour (line2);
    g.strokePath (bgArc, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float angle = a0 + pos * (a1 - a0);
    const float from = bipolar ? (a0 + a1) * 0.5f : a0;
    if (std::abs (angle - from) > 0.01f)
    {
        juce::Path v;
        v.addCentredArc (c.x, c.y, radius, radius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (s.isEnabled() ? col : col.withAlpha (0.4f));
        g.strokePath (v, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float knobR = radius * 0.72f;
    g.setColour (panel2);
    g.fillEllipse (c.x - knobR, c.y - knobR, knobR * 2.0f, knobR * 2.0f);
    g.setColour (line2);
    g.drawEllipse (c.x - knobR, c.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.0f);

    const auto p1 = c.getPointOnCircumference (knobR * 0.25f, angle);
    const auto p2 = c.getPointOnCircumference (knobR * 0.85f, angle);
    g.setColour (text);
    g.drawLine ({ p1, p2 }, 2.0f);
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos,
                                    float, float, juce::Slider::SliderStyle, juce::Slider& s)
{
    const float cy = (float) y + (float) h * 0.5f;
    const auto track = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) w, 4.0f);
    g.setColour (line2);
    g.fillRoundedRectangle (track, 2.0f);

    const auto col = s.findColour (juce::Slider::trackColourId);
    g.setColour (s.isEnabled() ? col : col.withAlpha (0.4f));
    g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);

    g.setColour (white);
    g.fillEllipse (sliderPos - 7.0f, cy - 7.0f, 14.0f, 14.0f);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool, bool)
{
    const auto box = juce::Rectangle<float> (0.0f, ((float) b.getHeight() - 16.0f) * 0.5f, 16.0f, 16.0f);
    const auto col = b.findColour (juce::ToggleButton::tickColourId);
    if (b.getToggleState())
    {
        g.setColour (col);
        g.fillRoundedRectangle (box, 4.0f);
        juce::Path tick;
        tick.startNewSubPath (box.getX() + 4.0f, box.getCentreY());
        tick.lineTo (box.getX() + 7.0f, box.getBottom() - 4.5f);
        tick.lineTo (box.getRight() - 3.5f, box.getY() + 4.5f);
        g.setColour (primaryInk);
        g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        g.setColour (line2);
        g.drawRoundedRectangle (box.reduced (0.5f), 4.0f, 1.5f);
    }

    drawText (g, b.getButtonText(), b.getLocalBounds().toFloat().withTrimmedLeft (26.0f), 11.0f,
              Weight::black, text2, 0.14f);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    juce::Path arrow;
    const float ax = (float) w - 16.0f, ay = (float) h * 0.5f;
    arrow.startNewSubPath (ax - 4.0f, ay - 2.0f);
    arrow.lineTo (ax, ay + 2.0f);
    arrow.lineTo (ax + 4.0f, ay - 2.0f);
    g.setColour (muted);
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox&) { return font (13.0f, Weight::bold); }

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 1, box.getWidth() - 26, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

juce::Font LookAndFeel::getPopupMenuFont() { return font (14.0f, Weight::bold); }

void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (panel2);
    g.setColour (line2);
    g.drawRect (0, 0, w, h);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgColour, bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = b.getProperties().getWithDefault ("radius", 8.0f);
    auto c = bgColour;
    if (! b.isEnabled()) c = c.withMultipliedAlpha (0.5f);
    else if (down) c = c.brighter (0.08f);
    else if (highlighted) c = c.brighter (0.04f);
    g.setColour (c);
    g.fillRoundedRectangle (r, radius);

    if (b.getProperties().contains ("border"))
    {
        g.setColour (juce::Colour ((juce::uint32) (juce::int64) b.getProperties()["border"]));
        g.drawRoundedRectangle (r, radius, 1.0f);
    }
}

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const float size = b.getProperties().getWithDefault ("fontSize", 12.0f);
    const float tracking = b.getProperties().getWithDefault ("tracking", 0.12f);
    auto col = b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    if (! b.isEnabled()) col = col.withMultipliedAlpha (0.5f);
    drawText (g, b.getButtonText(), b.getLocalBounds().toFloat(), size, Weight::black, col, tracking,
              juce::Justification::centred);
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int) { return font (12.0f, Weight::black, 0.12f); }
juce::Font LookAndFeel::getLabelFont (juce::Label&) { return font (12.0f, Weight::bold); }

} // namespace theme
