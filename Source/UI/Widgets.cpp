#include "Widgets.h"

using namespace theme;

//==============================================================================
void KeyWheel::setKey (int k, int a, float c)
{
    if (k == key && a == alt && juce::approximatelyEqual (c, confidence))
        return;
    key = k; alt = a; confidence = c;
    repaint();
}

static int wheelIndexOf (int key)
{
    if (key < 0) return -1;
    const int pc = key % 12;
    const int majorPc = key < 12 ? pc : (pc + 3) % 12;
    return (majorPc * 7) % 12;
}

void KeyWheel::paint (juce::Graphics& g)
{
    // Scriba style: a ring of 12 keys with cyan ticks, the key big in the middle.
    const float s = (float) juce::jmin (getWidth(), getHeight()) / 200.0f;
    const auto c = getLocalBounds().toFloat().getCentre();
    const float rOut = 96.0f * s, rMid = 70.0f * s, rIn = 46.0f * s;

    auto sector = [&] (int i, float r0, float r1)
    {
        const float a0 = juce::degreesToRadians (-14.0f + (float) i * 30.0f);
        const float a1 = juce::degreesToRadians (14.0f + (float) i * 30.0f);
        juce::Path p;
        p.addPieSegment (c.x - r1, c.y - r1, r1 * 2.0f, r1 * 2.0f, a0, a1, r0 / r1);
        return p;
    };

    // outer tick ring
    for (int i = 0; i < 60; ++i)
    {
        const float ang = juce::degreesToRadians ((float) i * 6.0f);
        const bool major = i % 5 == 0;
        const auto p1 = c.getPointOnCircumference (rOut + 1.0f * s, ang);
        const auto p2 = c.getPointOnCircumference (rOut - (major ? 6.0f : 3.0f) * s, ang);
        g.setColour (major ? tonic.withAlpha (0.75f) : line2);
        g.drawLine ({ p1, p2 }, major ? 1.6f * s : 1.0f);
    }

    const int ki = wheelIndexOf (key);
    const bool keyMinor = key >= 12;
    const int ai = wheelIndexOf (alt);

    for (int i = 0; i < 12; ++i)
    {
        g.setColour (rowKey);
        g.fillPath (sector (i, rMid, rOut - 8.0f * s));
        g.fillPath (sector (i, rIn, rMid - 2.0f * s));
    }
    if (ki >= 0)
    {
        g.setColour (primary.withAlpha (0.9f));
        g.fillPath (keyMinor ? sector (ki, rIn, rMid - 2.0f * s) : sector (ki, rMid, rOut - 8.0f * s));
    }
    if (ai >= 0 && ai != ki)
    {
        juce::Path dashed;
        const float dashes[] = { 3.0f * s, 3.0f * s };
        juce::PathStrokeType (1.4f).createDashedStroke (dashed, alt >= 12 ? sector (ai, rIn, rMid - 2.0f * s)
                                                                        : sector (ai, rMid, rOut - 8.0f * s), dashes, 2);
        g.setColour (tonic.withAlpha (0.8f));
        g.fillPath (dashed);
    }

    static const char* majors[] = { "C", "G", "D", "A", "E", "B", "F#", "Db", "Ab", "Eb", "Bb", "F" };
    static const char* minors[] = { "Am", "Em", "Bm", "F#m", "C#m", "G#m", "D#m", "Bbm", "Fm", "Cm", "Gm", "Dm" };
    for (int i = 0; i < 12; ++i)
    {
        const float ang = juce::degreesToRadians ((float) i * 30.0f);
        const auto pm = c.getPointOnCircumference (79.0f * s, ang);
        const auto pn = c.getPointOnCircumference (58.0f * s, ang);
        const bool selMaj = (ki == i && ! keyMinor);
        const bool selMin = (ki == i && keyMinor);
        drawText (g, majors[i], juce::Rectangle<float> (30.0f * s, 14.0f * s).withCentre (pm), 10.5f * s,
                  Weight::black, selMaj ? primaryInk : text3, 0.0f, juce::Justification::centred);
        drawText (g, minors[i], juce::Rectangle<float> (30.0f * s, 14.0f * s).withCentre (pn), 8.5f * s,
                  Weight::black, selMin ? primaryInk : dim, 0.0f, juce::Justification::centred);
    }

    // centre: the key, big
    g.setColour (field);
    g.fillEllipse (c.x - rIn + 3.0f * s, c.y - rIn + 3.0f * s, (rIn - 3.0f * s) * 2.0f, (rIn - 3.0f * s) * 2.0f);
    g.setColour (line2);
    g.drawEllipse (c.x - rIn + 3.0f * s, c.y - rIn + 3.0f * s, (rIn - 3.0f * s) * 2.0f, (rIn - 3.0f * s) * 2.0f, 1.0f);
    if (key >= 0)
    {
        static const char* names[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        drawText (g, names[key % 12], juce::Rectangle<float> (rIn * 2.0f, 34.0f * s).withCentre ({ c.x, c.y - 9.0f * s }),
                  30.0f * s, Weight::black, text, 0.0f, juce::Justification::centred);
        drawText (g, keyMinor ? "MINOR" : "MAJOR", juce::Rectangle<float> (rIn * 2.0f, 12.0f * s).withCentre ({ c.x, c.y + 14.0f * s }),
                  8.0f * s, Weight::black, primary, 0.2f, juce::Justification::centred);
        static const char* longNames[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        const int rel = keyMinor ? (key % 12 + 3) % 12 : (key % 12 + 9) % 12;
        drawText (g, juce::String ("relative ") + longNames[rel] + (keyMinor ? " major" : " minor"),
                  juce::Rectangle<float> (rIn * 2.0f, 12.0f * s).withCentre ({ c.x, c.y + 26.0f * s }),
                  7.5f * s, Weight::bold, text3, 0.0f, juce::Justification::centred);
    }
    else
    {
        drawText (g, "--", juce::Rectangle<float> (rIn * 2.0f, 30.0f * s).withCentre (c), 24.0f * s,
                  Weight::black, dim, 0.0f, juce::Justification::centred);
    }
}

//==============================================================================
void ToggleSwitch::paintButton (juce::Graphics& g, bool, bool)
{
    const bool on = getToggleState();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (field);
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (on ? onColour : line2);
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);

    drawText (g, on ? "ON" : "OFF", r.withTrimmedLeft (10.0f).withWidth (30.0f), 10.0f, Weight::black,
              on ? onColour : muted, 0.12f);

    const auto track = juce::Rectangle<float> (r.getRight() - 42.0f, r.getCentreY() - 11.0f, 38.0f, 22.0f);
    g.setColour (on ? onColour : line2);
    g.fillRoundedRectangle (track, 11.0f);
    g.setColour (white);
    g.fillEllipse (on ? track.getRight() - 19.0f : track.getX() + 3.0f, track.getY() + 3.0f, 16.0f, 16.0f);
}

//==============================================================================
void BigActionButton::setTexts (const juce::String& t, const juce::String& s)
{
    if (t == title && s == sub) return;
    title = t; sub = s;
    repaint();
}

void BigActionButton::setColours (juce::Colour b, juce::Colour f, juce::Colour br)
{
    if (b == bgCol && f == fgCol && br == borderCol) return;
    bgCol = b; fgCol = f; borderCol = br;
    repaint();
}

void BigActionButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    auto c = bgCol;
    if (down) c = c.brighter (0.1f); else if (highlighted) c = c.brighter (0.05f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 10.0f);
    if (! borderCol.isTransparent())
    {
        g.setColour (borderCol);
        g.drawRoundedRectangle (r, 10.0f, 1.0f);
    }
    drawText (g, title, r.withHeight (r.getHeight() * 0.58f).withTrimmedTop (6.0f), 15.0f, Weight::black, fgCol, 0.12f,
              juce::Justification::centredBottom);
    drawText (g, sub, r.withTrimmedTop (r.getHeight() * 0.58f + 2.0f), 10.0f, Weight::black, fgCol.withAlpha (0.85f), 0.08f,
              juce::Justification::centredTop);
}

//==============================================================================
void HoldButton::buttonStateChanged()
{
    const bool down = isDown();
    if (down != wasDown)
    {
        wasDown = down;
        if (onHold) onHold (down);
    }
}

void HoldButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const bool on = isDown() || keyHeld;
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (on ? accent : (highlighted ? panel2.brighter (0.05f) : panel));
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (on ? accent : line2);
    g.drawRoundedRectangle (r, 10.0f, 1.0f);

    const auto col = on ? ink : theme::text;
    drawText (g, on ? "ORIGINAL" : "CORRECTED", r.withTrimmedBottom (r.getHeight() * 0.42f).withTrimmedTop (3.0f),
              11.5f, Weight::black, col, 0.12f, juce::Justification::centredBottom);
    drawText (g, on ? "RELEASE = CORRECTED" : "HOLD (B) = ORIGINAL", r.withTrimmedTop (r.getHeight() * 0.58f),
              8.5f, Weight::black, on ? ink.withAlpha (0.8f) : muted, 0.1f, juce::Justification::centredTop);
}

//==============================================================================
IconButton::IconButton (const juce::String& name, const char* svg, bool f)
    : juce::Button (name), icon (iconPath (svg)), framed (f)
{
    setTooltip (name);
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = getToggleState();

    if (on)
    {
        g.setColour (panel.interpolatedWith (primary, 0.14f));
        g.fillRoundedRectangle (r, 9.0f);
        g.setColour (primary);
        g.drawRoundedRectangle (r.reduced (0.5f), 9.0f, 1.2f);
    }
    else if (framed)       { g.setColour (panel2); g.fillRoundedRectangle (r, 8.0f); g.setColour (line2); g.drawRoundedRectangle (r, 8.0f, 1.0f); }
    else if (highlighted || down) { g.setColour (line2.withAlpha (0.6f)); g.fillRoundedRectangle (r, 7.0f); }

    auto ic = icon;
    const float size = juce::jmin (20.0f, r.getHeight() - 14.0f);
    ic.applyTransform (ic.getTransformToScaleToFit (0, 0, size, size, true)
                         .translated (r.getCentreX() - size * 0.5f, r.getCentreY() - size * 0.5f));
    // keep stroke width independent of the 24 px source box
    juce::Colour col = on ? primary : text2;
    if (! isEnabled()) col = col.withAlpha (0.35f);
    g.setColour (col);
    g.strokePath (ic, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
ExportButton::ExportButton (const juce::String& label, juce::Colour c, bool s)
    : juce::Button (label), colour (c), strong (s)
{
    setTooltip ("Drag onto your DAW, or click to save");
}

void ExportButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    auto fill = strong ? panel.interpolatedWith (colour, 0.12f) : panel;
    if (down) fill = fill.brighter (0.1f); else if (highlighted) fill = fill.brighter (0.05f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (strong ? colour : line2);
    g.drawRoundedRectangle (r, 10.0f, 1.0f);
    auto col = strong ? colour : theme::text;
    if (! isEnabled()) col = col.withAlpha (0.35f);
    drawText (g, getButtonText(), r, 12.0f, Weight::black, col, 0.12f, juce::Justification::centred);
}

void ExportButton::mouseDrag (const juce::MouseEvent& e)
{
    juce::Button::mouseDrag (e);
    if (dragging || ! isEnabled() || e.getDistanceFromDragStart() < 6 || makeFile == nullptr)
        return;

    const auto file = makeFile();
    if (! file.existsAsFile())
        return;

    dragging = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                [this] { dragging = false; });
}

void ExportButton::clicked()
{
    if (dragging || makeFile == nullptr)
        return;

    const auto file = makeFile();
    if (! file.existsAsFile())
        return;

    chooser = std::make_unique<juce::FileChooser> (saveDialogTitle,
                                                   juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (file.getFileName()),
                                                   saveWildcard);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [file] (const juce::FileChooser& fc)
                          {
                              const auto dest = fc.getResult();
                              if (dest != juce::File())
                                  file.copyFileTo (dest);
                          });
}
