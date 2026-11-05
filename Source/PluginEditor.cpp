#include "PluginEditor.h"
#include "BinaryData.h"

using namespace theme;

namespace
{
    constexpr int asideW = 300, headerH = 64;
    const juce::Rectangle<float> keyCard  { 16.0f, 76.0f, 284.0f, 376.0f };
    const juce::Rectangle<float> infoCard { 16.0f, 460.0f, 284.0f, 32.0f };
    const juce::Rectangle<float> corrCard { 16.0f, 500.0f, 284.0f, 288.0f };
    const juce::Rectangle<int> rollBounds { 316, 132, 948, 486 };
    const juce::Rectangle<int> selectedBox { 316, 628, 190, 156 };
    const juce::Rectangle<int> knobPanel { 520, 628, 580, 156 };

    const juce::String dot = juce::String::charToString ((juce::juce_wchar) 0x00b7);
    const juce::String versionText = "v" + juce::String (JucePlugin_VersionString);
    const juce::String tagline = "PITCH  " + dot + "  TIMING  " + dot + "  FORMANT  " + dot + "  AUTO KEY";
}

//==============================================================================
MainView::MainView (ScribaPitchProcessor& p) : proc (p), roll (p)
{
    setLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);
    logo = juce::ImageCache::getFromMemory (BinaryData::logo_light_png, BinaryData::logo_light_pngSize);

    // ---- header -------------------------------------------------------------
    for (auto* b : { &undoBtn, &redoBtn })
        addAndMakeVisible (b);
    undoBtn.onClick = [this] { proc.undo(); };
    redoBtn.onClick = [this] { proc.redo(); };

    abButton.onHold = [this] (bool held) { proc.setHoldOriginal (held || bKeyHeld); };
    addAndMakeVisible (abButton);

    styleTextButton (analyzeBtn, 13.0f);
    analyzeBtn.setColour (juce::TextButton::buttonColourId, primary);
    analyzeBtn.setColour (juce::TextButton::textColourOffId, primaryInk);
    analyzeBtn.getProperties().set ("radius", 12.0f);
    analyzeBtn.onClick = [this] { proc.pressAnalyze(); };
    addAndMakeVisible (analyzeBtn);

    // ---- left panel ---------------------------------------------------------
    addAndMakeVisible (wheel);

    for (auto* b : { &keyAutoBtn, &keyManualBtn })
    {
        styleTextButton (*b, 11.0f);
        b->getProperties().set ("radius", 8.0f);
        addAndMakeVisible (b);
    }
    keyAutoBtn.setTooltip ("The key comes from Scriba (if it is in the project) or from this vocal");
    keyManualBtn.setTooltip ("You choose the key");
    keyAutoBtn.onClick   = [this] { setKeyMode (false); };
    keyManualBtn.onClick = [this] { setKeyMode (true); };

    const auto keyChoices = dynamic_cast<juce::AudioParameterChoice*> (proc.params.getParameter ("manualKey"))->choices;
    manualKeyBox.addItemList (keyChoices, 1);
    scaleBox.addItemList (sp::scaleTypeNames(), 1);
    for (auto* b : { &manualKeyBox, &scaleBox })
        addAndMakeVisible (b);
    manualKeyAtt = std::make_unique<Attach::ComboBoxAttachment> (proc.params, "manualKey", manualKeyBox);
    scaleAtt     = std::make_unique<Attach::ComboBoxAttachment> (proc.params, "scale", scaleBox);

    addAndMakeVisible (bypassSwitch);
    bypassSwitch.setTooltip ("Bypass the whole plugin (the same as the DAW's bypass)");
    bypassAtt = std::make_unique<Attach::ButtonAttachment> (proc.params, "bypass", bypassSwitch);

    addAndMakeVisible (autoBtn);
    autoBtn.onClick = [this] { proc.automaticCorrection(); };

    for (auto* s : { &corrSlider, &driftSlider, &humanSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible (s);
    }
    corrAtt  = std::make_unique<Attach::SliderAttachment> (proc.params, "corrAmt", corrSlider);
    driftAtt = std::make_unique<Attach::SliderAttachment> (proc.params, "drift", driftSlider);
    humanAtt = std::make_unique<Attach::SliderAttachment> (proc.params, "humanize", humanSlider);
    for (auto* s : { &corrSlider, &driftSlider, &humanSlider })
        s->onValueChange = [this] { repaint (0, 600, asideW, 130); };

    keepVibBtn.setColour (juce::ToggleButton::tickColourId, primary);
    addAndMakeVisible (keepVibBtn);
    keepVibAtt = std::make_unique<Attach::ButtonAttachment> (proc.params, "keepVib", keepVibBtn);

    // ---- toolbar -------------------------------------------------------------
    for (int t = 0; t < PianoRoll::numTools; ++t)
    {
        auto* b = toolButtons.add (new IconButton (PianoRoll::toolName (t), PianoRoll::toolIcon (t), false));
        b->setTooltip (PianoRoll::toolName (t) + "   (key " + juce::String (t + 1) + ")");
        b->onClick = [this, t] { setTool (t); };
        addAndMakeVisible (b);
    }
    for (int i = 0; i < 3; ++i)
    {
        styleTextButton (snapButtons[i], 11.0f);
        snapButtons[i].getProperties().set ("radius", 8.0f);
        snapButtons[i].onClick = [this, i] { setSnap (i); };
        addAndMakeVisible (snapButtons[i]);
    }

    addAndMakeVisible (roll);
    roll.onSelectionChanged = [this] { updateInspector(); };
    roll.onSeek = [this] (double seconds)
    {
        if (! proc.requestHostPosition (seconds))
            roll.flash ("MOVING THE DAW CURSOR NEEDS ARA (LOGIC, CUBASE, STUDIO ONE) OR REAPER");
    };
    roll.onPlayToggle = [this]
    {
        if (! proc.requestHostPlayToggle())
            roll.flash ("PLAY / STOP FROM HERE NEEDS ARA");
    };

    // ---- inspector -----------------------------------------------------------
    struct KnobDef { const char* label; double lo, hi, step, def; bool bipolar; };
    static const KnobDef defs[] = {
        { "PITCH CENTER", 0, 100, 1, 0, false },
        { "PITCH DRIFT",  0, 100, 1, 0, false },
        { "VIBRATO",      0, 200, 1, 100, false },
        { "FORMANT",    -12,  12, 0.1, 0, true },
        { "GAIN",       -18,  18, 0.1, 0, true },
        { "TIMING",    -250, 250, 1, 0, true }
    };

    for (int i = 0; i < 6; ++i)
    {
        auto& k = knobs[i];
        k.label = defs[i].label;
        k.slider.setRange (defs[i].lo, defs[i].hi, defs[i].step);
        k.slider.setDoubleClickReturnValue (true, defs[i].def);
        k.slider.getProperties().set ("bipolar", defs[i].bipolar);
        k.slider.setColour (juce::Slider::rotarySliderFillColourId, defs[i].bipolar ? tonic : primary);
        k.slider.setRotaryParameters (juce::degreesToRadians (-135.0f), juce::degreesToRadians (135.0f), true);
        k.slider.setMouseDragSensitivity (180);
        k.slider.onDragStart = [this] { proc.pushUndo(); };
        k.slider.onDragEnd   = [this] { proc.dropUndoIfUnchanged(); };
        k.slider.onValueChange = [this, i]
        {
            if (updatingInspector) return;
            const double v = knobs[i].slider.getValue();
            roll.editSelected ([this, i, v] (sp::Note& n)
            {
                switch (i)
                {
                    case 0: n.centerAmt = (float) (v / 100.0); break;
                    case 1: n.driftAmt  = (float) (v / 100.0); break;
                    case 2: n.vibAmt    = (float) (v / 100.0); break;
                    case 3: n.formant   = (float) v; break;
                    case 4: n.gainDb    = (float) v; break;
                    case 5:
                    {
                        const double start = n.inStart + v / 1000.0;
                        roll.clampTime (n, start, start + n.outLength());
                        break;
                    }
                    default: break;
                }
            });
            updateInspector();
        };
        addAndMakeVisible (k.slider);
    }

    midiBtn.makeFile = [this] { return proc.exportMidiFile(); };
    midiBtn.saveDialogTitle = "Save the corrected melody as MIDI";
    midiBtn.saveWildcard = "*.mid";
    audioBtn.makeFile = [this] { return proc.exportAudioFile(); };
    audioBtn.saveDialogTitle = "Save the corrected vocal";
    audioBtn.saveWildcard = "*.wav";
    addAndMakeVisible (midiBtn);
    addAndMakeVisible (audioBtn);

    setTool (proc.toolIndex.load());
    setSnap (proc.snapMode.load());
    setSize (designW, designH);
    refresh (true);
}

MainView::~MainView()
{
    setLookAndFeel (nullptr);
}

void MainView::setSelectedLook (juce::TextButton& b, bool on)
{
    // Scriba: the chosen option is outlined in teal on a teal-tinted card
    b.setColour (juce::TextButton::buttonColourId, on ? panel.interpolatedWith (primary, 0.14f) : panel2);
    b.setColour (juce::TextButton::textColourOffId, on ? primary : muted);
    b.getProperties().set ("border", (juce::int64) (on ? primary : line2).getARGB());
    b.repaint();
}

void MainView::styleTextButton (juce::TextButton& b, float fontSize)
{
    b.getProperties().set ("fontSize", fontSize);
    b.setColour (juce::TextButton::buttonColourId, panel);
    b.setColour (juce::TextButton::textColourOffId, muted);
}

//==============================================================================
void MainView::resized()
{
    // header (right to left)
    analyzeBtn.setBounds (designW - 16 - 196, 12, 196, 40);
    abButton.setBounds (690, 14, 196, 36);
    redoBtn.setBounds (640, 14, 40, 36);
    undoBtn.setBounds (590, 14, 40, 36);

    // left column (cards)
    wheel.setBounds (58, 106, 200, 200);
    keyAutoBtn.setBounds (30, 376, 126, 30);
    keyManualBtn.setBounds (160, 376, 126, 30);
    manualKeyBox.setBounds (30, 412, 126, 28);
    scaleBox.setBounds (160, 412, 126, 28);
    bypassSwitch.setBounds (286 - 92, 510, 92, 30);
    autoBtn.setBounds (30, 550, 256, 58);
    corrSlider.setBounds (26, 636, 264, 20);
    driftSlider.setBounds (26, 676, 264, 20);
    humanSlider.setBounds (26, 716, 264, 20);
    keepVibBtn.setBounds (30, 744, 256, 24);

    // toolbar
    for (int t = 0; t < toolButtons.size(); ++t)
        toolButtons[t]->setBounds (320 + t * 48, 79, 44, 38);
    snapButtons[0].setBounds (1264 - 212, 81, 96, 34);
    snapButtons[1].setBounds (1264 - 116, 81, 64, 34);
    snapButtons[2].setBounds (1264 - 52, 81, 52, 34);

    roll.setBounds (rollBounds);

    for (int i = 0; i < 6; ++i)
        knobs[i].slider.setBounds (knobPanel.getX() + 10 + i * 94 + 21, knobPanel.getY() + 16, 52, 52);

    midiBtn.setBounds (1114, 628, 150, 74);
    audioBtn.setBounds (1114, 710, 150, 74);
}

//==============================================================================
void MainView::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    // ---- header ---------------------------------------------------------------
    g.setColour (line);
    g.fillRect (16, headerH - 1, designW - 32, 1);

    if (logo.isValid())
        g.drawImage (logo, juce::Rectangle<float> (18.0f, 11.0f, 40.0f, 42.0f), juce::RectanglePlacement::centred);
    {
        const auto titleFont = font (21.0f, Weight::black, 0.16f);
        const float scribaW = juce::GlyphArrangement::getStringWidth (titleFont, "SCRIBA");
        const float pitchW = juce::GlyphArrangement::getStringWidth (titleFont, "PITCH");
        drawText (g, "SCRIBA", { 70.0f, 10.0f, scribaW + 4.0f, 28.0f }, 21.0f, Weight::black, text, 0.16f);
        drawText (g, "PITCH", { 70.0f + scribaW + 8.0f, 10.0f, pitchW + 4.0f, 28.0f }, 21.0f, Weight::black, primary, 0.16f);
        drawText (g, tagline, { 71.0f, 36.0f, 360.0f, 16.0f }, 9.5f, Weight::black, muted, 0.2f);

        // version + ARA pill: is the DAW handing the track to the plugin?
        float x = 70.0f + scribaW + pitchW + 24.0f;
        const float vw = juce::GlyphArrangement::getStringWidth (font (10.0f, Weight::black, 0.1f), versionText) + 16.0f;
        const auto vp = juce::Rectangle<float> (x, 14.0f, vw, 20.0f);
        g.setColour (panel);
        g.fillRoundedRectangle (vp, 10.0f);
        g.setColour (line2);
        g.drawRoundedRectangle (vp.reduced (0.5f), 10.0f, 1.0f);
        drawText (g, versionText, vp, 10.0f, Weight::black, muted, 0.1f, juce::Justification::centred);
        x += vw + 6.0f;
        const bool ara = proc.isAraMode();
        const auto pill = juce::Rectangle<float> (x, 14.0f, ara ? 42.0f : 62.0f, 20.0f);
        g.setColour (ara ? primary.withAlpha (0.12f) : panel);
        g.fillRoundedRectangle (pill, 10.0f);
        g.setColour (ara ? primary : line2);
        g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, 1.0f);
        drawText (g, ara ? "ARA" : "NO ARA", pill, 9.5f, Weight::black, ara ? primary : muted, 0.12f, juce::Justification::centred);
    }

    // status pill (Scriba: "● DONE")
    {
        const auto chip = juce::Rectangle<float> (898.0f, 14.0f, 154.0f, 36.0f);
        const auto st = proc.getStatus();
        juce::Colour dotCol = primary;
        if (st == ScribaPitchProcessor::Status::recording || st == ScribaPitchProcessor::Status::armed
            || st == ScribaPitchProcessor::Status::missingTake)
            dotCol = accent;
        else if (st == ScribaPitchProcessor::Status::idle)
            dotCol = muted;
        if (proc.isBypassed()) dotCol = accent;
        g.setColour (dotCol.withAlpha (0.10f).overlaidWith (panel.withAlpha (0.9f)));
        g.fillRoundedRectangle (chip, 18.0f);
        g.setColour (dotCol.withAlpha (0.55f));
        g.drawRoundedRectangle (chip.reduced (0.5f), 18.0f, 1.0f);
        g.setColour (dotCol.withAlpha (0.25f));
        g.fillEllipse (chip.getX() + 11.0f, chip.getCentreY() - 7.0f, 14.0f, 14.0f);
        g.setColour (dotCol);
        g.fillEllipse (chip.getX() + 14.0f, chip.getCentreY() - 4.0f, 8.0f, 8.0f);
        juce::String status = proc.getStatusText();
        if (st == ScribaPitchProcessor::Status::ready)
        {
            const int secs = (int) proc.getTakeSeconds();
            status << "  " << dot << "  " << secs / 60 << ":" << juce::String (secs % 60).paddedLeft ('0', 2);
        }
        if (proc.isAraMode() && st == ScribaPitchProcessor::Status::ready)
        {
            const int secs = (int) proc.getTakeSeconds();
            status = "READY  " + dot + "  " + juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2);
        }
        if (proc.isSampleRateMismatch()) status = "SAMPLE RATE CHANGED";
        if (proc.isBypassed()) status = "BYPASSED";
        drawText (g, status, chip.withTrimmedLeft (32.0f), 11.0f, Weight::black, text, 0.1f);
    }

    // ---- key card ----------------------------------------------------------------
    drawCard (g, keyCard);
    drawText (g, "KEY OF THIS TRACK", { 30.0f, 86.0f, 160.0f, 16.0f }, 10.0f, Weight::black, muted, 0.18f);
    const int key = proc.getEffectiveKey();
    const auto det = proc.getDetectedKey();
    const auto linked = proc.getLinked();
    const auto source = proc.getKeySource();
    {
        juce::String chipText = "NO KEY YET";
        juce::Colour chipCol = muted;
        switch (source)
        {
            case ScribaPitchProcessor::KeySource::manual: chipText = "MANUAL";      chipCol = accent; break;
            case ScribaPitchProcessor::KeySource::scriba: chipText = "SCRIBA LINK"; chipCol = primary; break;
            case ScribaPitchProcessor::KeySource::vocal:  chipText = "AUTO";        chipCol = primary; break;
            case ScribaPitchProcessor::KeySource::none:
            default: break;
        }
        const float cw = juce::GlyphArrangement::getStringWidth (font (9.0f, Weight::black, 0.12f), chipText) + 18.0f;
        const auto chip = juce::Rectangle<float> (286.0f - cw, 84.0f, cw, 20.0f);
        g.setColour (chipCol.withAlpha (0.12f));
        g.fillRoundedRectangle (chip, 10.0f);
        g.setColour (chipCol.withAlpha (0.7f));
        g.drawRoundedRectangle (chip.reduced (0.5f), 10.0f, 1.0f);
        drawText (g, chipText, chip, 9.0f, Weight::black, chipCol, 0.12f, juce::Justification::centred);
    }

    // relative key + confidence bar (Scriba: "A minor 62% ———— 38% C major")
    {
        int alt = det.alt;
        float conf = det.confidence;
        if (source == ScribaPitchProcessor::KeySource::scriba) { alt = linked.alt; conf = linked.confidence; }
        if (source == ScribaPitchProcessor::KeySource::manual) { alt = det.valid() && det.key != key ? det.key : -1; conf = 1.0f; }

        juce::String rel = "PRESS ANALYZE AND PLAY THE SONG";
        if (key >= 0)
        {
            const int relKey = key < 12 ? ((key + 9) % 12) + 12 : ((key % 12) + 3) % 12;
            rel = "relative " + sp::keyLongName (relKey);
        }
        if (key < 0)
            drawText (g, rel, { 30.0f, 310.0f, 256.0f, 16.0f }, 10.0f, Weight::black, muted, 0.08f, juce::Justification::centred);

        const auto bar = juce::Rectangle<float> (30.0f, 316.0f - 4.0f, 256.0f, 5.0f);
        g.setColour (field);
        g.fillRoundedRectangle (bar, 3.0f);
        if (key >= 0)
        {
            const float c = juce::jlimit (0.0f, 1.0f, conf);
            const int pc = juce::roundToInt (c * 100.0f);
            g.setColour (primary);
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (6.0f, bar.getWidth() * c)), 3.0f);
            auto niceName = [] (int k)
            {
                static const char* nm[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
                return juce::String (nm[k % 12]) + (k >= 12 ? " minor" : " major");
            };
            drawText (g, niceName (key) + "  " + juce::String (pc) + "%", { 30.0f, 326.0f, 170.0f, 18.0f },
                      13.0f, Weight::black, text, 0.0f);
            if (alt >= 0 && alt != key)
                drawText (g, juce::String (100 - pc) + "%  " + niceName (alt), { 120.0f, 326.0f, 166.0f, 18.0f },
                          13.0f, Weight::semiBold, muted, 0.0f, juce::Justification::centredRight);
        }
    }

    // ---- info line ---------------------------------------------------------------
    drawCard (g, infoCard, 10.0f);
    {
        juce::String info = "Scriba not found in this project";
        juce::Colour dc = muted;
        switch (source)
        {
            case ScribaPitchProcessor::KeySource::manual:
                info = det.valid() ? "Manual key  " + dot + "  the vocal says " + sp::keyLongName (det.key) : juce::String ("Manual key");
                dc = accent;
                break;
            case ScribaPitchProcessor::KeySource::scriba:
                info = "Linked to Scriba: " + linked.sources.joinIntoString (" + ");
                if (det.valid() && det.key != linked.key) info << "  " << dot << "  vocal " << sp::keyShortName (det.key);
                dc = primary;
                break;
            case ScribaPitchProcessor::KeySource::vocal:
                info = "Scriba not found  " + dot + "  key from the vocal";
                dc = accent;
                break;
            case ScribaPitchProcessor::KeySource::none:
            default: break;
        }
        g.setColour (dc);
        g.fillEllipse (infoCard.getX() + 12.0f, infoCard.getCentreY() - 3.5f, 7.0f, 7.0f);
        drawText (g, info, infoCard.withTrimmedLeft (28.0f).withTrimmedRight (8.0f), 10.5f, Weight::bold, text2, 0.0f);
    }

    // ---- correction card -------------------------------------------------------
    drawCard (g, corrCard);
    const bool bypassed = proc.isBypassed();
    drawText (g, "BYPASS", { 30.0f, 510.0f, 150.0f, 30.0f }, 10.0f, Weight::black, bypassed ? accent : muted, 0.18f);

    {
        const struct { const char* label; juce::Slider* s; float y; } rows[] = {
            { "CORRECTION AMOUNT", &corrSlider, 620.0f }, { "PITCH DRIFT", &driftSlider, 660.0f }, { "HUMANIZE", &humanSlider, 700.0f }
        };
        for (const auto& r : rows)
        {
            drawText (g, r.label, { 30.0f, r.y, 200.0f, 14.0f }, 9.5f, Weight::black, muted, 0.16f);
            drawText (g, juce::String (juce::roundToInt (r.s->getValue())) + "%", { 286.0f - 60.0f, r.y, 60.0f, 14.0f },
                      11.0f, Weight::black, text, 0.0f, juce::Justification::centredRight);
        }
    }

    // note colour legend
    {
        g.setColour (line);
        g.fillRect (30.0f, 772.0f - 8.0f, 256.0f, 1.0f);
        const struct { const char* label; juce::Colour c; } items[] = {
            { "IN KEY", inKey }, { "OFF BY CENTS", accent }, { "OUT OF KEY", wrong }
        };
        float x = 30.0f;
        for (const auto& it : items)
        {
            g.setColour (it.c);
            g.fillEllipse (x, 773.0f, 7.0f, 7.0f);
            const float w = juce::GlyphArrangement::getStringWidth (font (8.5f, Weight::black, 0.08f), it.label);
            drawText (g, it.label, { x + 11.0f, 769.0f, w + 4.0f, 15.0f }, 8.5f, Weight::black, text3, 0.08f);
            x += 11.0f + w + 14.0f;
        }
    }

    // ---- toolbar ---------------------------------------------------------------
    {
        const auto group = juce::Rectangle<float> (316.0f, 75.0f, 8.0f + toolButtons.size() * 48.0f - 4.0f, 46.0f);
        drawCard (g, group, 12.0f);

        const float tx = group.getRight() + 16.0f;
        drawText (g, "TOOL: ", { tx, 80.0f, 60.0f, 18.0f }, 11.0f, Weight::black, muted, 0.14f);
        drawText (g, PianoRoll::toolName (proc.toolIndex.load()), { tx + 46.0f, 80.0f, 200.0f, 18.0f }, 11.0f, Weight::black, text, 0.14f);
        drawText (g, PianoRoll::toolHint (proc.toolIndex.load()), { tx, 99.0f, 1002.0f - tx - 4.0f, 16.0f }, 9.0f, Weight::bold, muted, 0.08f);

        drawText (g, "SNAP", { 1264.0f - 212.0f - 50.0f, 81.0f, 44.0f, 34.0f }, 10.0f, Weight::black, muted, 0.14f);
        g.setColour (line2);
        g.drawRoundedRectangle (juce::Rectangle<float> (1264.0f - 212.0f, 81.0f, 212.0f, 34.0f).expanded (0.5f), 8.0f, 1.0f);
    }

    // frame of the piano roll (a card like the others)
    g.setColour (line);
    g.drawRoundedRectangle (rollBounds.toFloat().expanded (3.5f), 12.0f, 1.0f);

    // ---- inspector -------------------------------------------------------------
    for (auto r : { selectedBox, knobPanel })
        drawCard (g, r.toFloat(), 12.0f);

    const auto* sel = roll.getSelectedNote();
    const auto sb = selectedBox.toFloat().reduced (14.0f, 0.0f);
    drawText (g, "SELECTED NOTE", { sb.getX(), 650.0f, sb.getWidth(), 14.0f }, 10.0f, Weight::black, muted, 0.16f);
    if (sel != nullptr)
    {
        const float c = sel->newCenter();
        const int cents = juce::roundToInt ((c - std::round (c)) * 100.0f);
        const auto scale = proc.getScale();

        double bpm, startPpq; int num, den; bool hasPpq;
        proc.getTempo (bpm, startPpq, num, den, hasPpq);
        if (bpm <= 0.0) bpm = 120.0;
        if (! hasPpq) startPpq = 0.0;
        const double beatQ = 4.0 / juce::jmax (1, den), barQ = beatQ * juce::jmax (1, num);
        const double q = startPpq + sel->outStart * bpm / 60.0;
        const int bar = (int) std::floor (q / barQ) + 1;
        const int beat = (int) std::floor (std::fmod (q, barQ) / beatQ) + 1;
        const double lenBeats = sel->outLength() * bpm / 60.0 / beatQ;

        drawText (g, sp::noteName ((int) std::round (c)), { sb.getX(), 666.0f, sb.getWidth(), 34.0f }, 26.0f, Weight::black, text, 0.04f);
        drawText (g, "BAR " + juce::String (bar) + "." + juce::String (beat) + "  " + dot + "  " + juce::String (lenBeats, 2) + " BEATS",
                  { sb.getX(), 704.0f, sb.getWidth(), 16.0f }, 11.0f, Weight::bold, text2, 0.0f);
        const auto tuning = sp::tuningOf (*sel, scale);
        const juce::String centsStr = (cents > 0 ? "+" : "") + juce::String (cents) + " ct";
        juce::String centsText = juce::String ("IN TUNE  ") + dot + "  " + centsStr;
        juce::Colour centsCol = inKey;
        if (tuning == sp::Tuning::outOfKey)      { centsText = "OUT OF KEY  " + dot + "  -> " + sp::noteName (scale.nearest (c)); centsCol = wrong; }
        else if (tuning == sp::Tuning::offCents) { centsText = centsStr + " OFF THE NOTE"; centsCol = accent; }
        drawText (g, centsText, { sb.getX(), 722.0f, sb.getWidth(), 16.0f }, 11.0f, Weight::bold, centsCol, 0.0f);
    }
    else
    {
        drawText (g, "CLICK A NOTE", { sb.getX(), 676.0f, sb.getWidth(), 34.0f }, 18.0f, Weight::black, dim, 0.06f);
    }

    for (int i = 0; i < 6; ++i)
    {
        const auto kb = knobs[i].slider.getBounds().toFloat();
        const auto col = sel != nullptr ? muted : muted.withAlpha (0.4f);
        drawText (g, knobs[i].label, { kb.getCentreX() - 47.0f, kb.getBottom() + 6.0f, 94.0f, 14.0f }, 9.0f, Weight::black, col, 0.12f,
                  juce::Justification::centred);
        if (sel != nullptr)
            drawText (g, knobs[i].valueText, { kb.getCentreX() - 47.0f, kb.getBottom() + 22.0f, 94.0f, 16.0f }, 11.0f, Weight::bold, text, 0.0f,
                      juce::Justification::centred);
    }
}

//==============================================================================
void MainView::setTool (int t)
{
    proc.toolIndex = t;
    roll.setTool (t);
    for (int i = 0; i < toolButtons.size(); ++i)
        toolButtons[i]->setToggleState (i == t, juce::dontSendNotification);
    repaint (300, 70, 720, 56);
}

void MainView::setSnap (int s)
{
    proc.snapMode = s;
    roll.setSnap (s);
    for (int i = 0; i < 3; ++i)
    {
        setSelectedLook (snapButtons[i], i == s);
    }
}

void MainView::refresh (bool fitView)
{
    roll.refreshFromProcessor (fitView);
    const auto det = proc.getDetectedKey();
    const int key = proc.getEffectiveKey();
    const auto linked = proc.getLinked();
    switch (proc.getKeySource())
    {
        case ScribaPitchProcessor::KeySource::manual: wheel.setKey (key, det.key, 1.0f); break;
        case ScribaPitchProcessor::KeySource::scriba: wheel.setKey (key, linked.alt, linked.confidence); break;
        case ScribaPitchProcessor::KeySource::vocal:
        case ScribaPitchProcessor::KeySource::none:
        default:                                      wheel.setKey (key, det.alt, det.confidence); break;
    }
    updateInspector();
    updateAutoSection();
    repaint();
}

void MainView::updateInspector()
{
    const auto* sel = roll.getSelectedNote();
    const juce::ScopedValueSetter<bool> svs (updatingInspector, true);

    for (auto& k : knobs)
        k.slider.setEnabled (sel != nullptr);

    if (sel != nullptr)
    {
        const double values[] = { sel->centerAmt * 100.0, sel->driftAmt * 100.0, sel->vibAmt * 100.0,
                                  sel->formant, sel->gainDb, (sel->outStart - sel->inStart) * 1000.0 };
        for (int i = 0; i < 6; ++i)
            knobs[i].slider.setValue (values[i], juce::dontSendNotification);

        knobs[0].valueText = juce::String (juce::roundToInt (values[0])) + "%";
        knobs[1].valueText = juce::String (juce::roundToInt (values[1])) + "%";
        knobs[2].valueText = juce::String (juce::roundToInt (values[2])) + "%";
        knobs[3].valueText = (values[3] > 0.05 ? "+" : "") + juce::String (values[3], 1) + " st";
        knobs[4].valueText = (values[4] > 0.05 ? "+" : "") + juce::String (values[4], 1) + " dB";
        knobs[5].valueText = (values[5] > 0.5 ? "+" : "") + juce::String (juce::roundToInt (values[5])) + " ms";
    }
    repaint (selectedBox.getUnion (knobPanel));
}

void MainView::updateAutoSection()
{
    const bool ready = proc.getStatus() == ScribaPitchProcessor::Status::ready;
    const auto notes = proc.getNotes();
    const auto scale = proc.getScale();

    // key mode switch
    const bool manual = proc.isKeyManual();
    setSelectedLook (keyAutoBtn, ! manual);
    setSelectedLook (keyManualBtn, manual);
    manualKeyBox.setEnabled (manual);
    manualKeyBox.setAlpha (manual ? 1.0f : 0.4f);
    keyAutoBtn.repaint();
    keyManualBtn.repaint();

    if (proc.isAutoCorrected())
    {
        autoBtn.setTexts ("SONG CORRECTED", juce::String (proc.getLastMovedCount())
                                               + " NOTES MOVED  " + dot + "  CURVES FIXED  " + dot + "  CLICK TO UNDO");
        autoBtn.setColours (panel.interpolatedWith (primary, 0.16f), primary, primary.withAlpha (0.6f));
        return;
    }

    int outOfKey = 0, offCents = 0;
    for (const auto& n : notes)
    {
        const auto t = sp::tuningOf (n, scale);
        outOfKey += t == sp::Tuning::outOfKey ? 1 : 0;
        offCents += t == sp::Tuning::offCents ? 1 : 0;
    }

    juce::String sub;
    if (! ready || notes.empty()) sub = "ANALYZE A VOCAL FIRST";
    else if (! scale.valid)       sub = "CHOOSE THE KEY (MANUAL KEY)";
    else sub = "WHOLE SONG  " + dot + "  " + juce::String (outOfKey) + " OUT OF KEY  " + dot + "  " + juce::String (offCents) + " OFF BY CENTS";

    autoBtn.setTexts ("AUTOMATIC CORRECTION", sub);
    autoBtn.setColours (primary, primaryInk, juce::Colours::transparentBlack);
}

void MainView::tick()
{
    const int counter = proc.getChangeCounter();
    if (counter != lastCounter)
    {
        lastCounter = counter;
        const bool hasNotes = ! proc.getNotes().empty();
        refresh (hasNotes && ! hadNotes);   // new analysis: frame the melody
        hadNotes = hasNotes;
    }

    const auto det = proc.getDetectedKey();
    const juce::String sig = juce::String (proc.params.getParameter ("keyMode")->getValue())
                           + "/" + juce::String (proc.params.getParameter ("manualKey")->getValue())
                           + "/" + juce::String (proc.params.getParameter ("scale")->getValue())
                           + "/" + juce::String ((int) proc.isBypassed()) + juce::String ((int) proc.isAraMode())
                           + "/" + juce::String (det.key) + "/" + proc.getStatusText()
                           + "/" + juce::String ((int) proc.isSampleRateMismatch())
                           + "/" + juce::String (proc.getLinked().key) + juce::String ((int) proc.getLinked().chords.size());
    if (sig != lastSignature)
    {
        lastSignature = sig;
        refresh (false);
    }

    analyzeBtn.setButtonText (proc.getAnalyzeButtonText());
    // safety: B released while the focus was elsewhere
    if (bKeyHeld && ! juce::KeyPress::isKeyCurrentlyDown ('b') && ! juce::KeyPress::isKeyCurrentlyDown ('B'))
        keyStateChanged (false);

    undoBtn.setEnabled (proc.canUndo());
    redoBtn.setEnabled (proc.canRedo());
    const bool ready = proc.getStatus() == ScribaPitchProcessor::Status::ready;
    midiBtn.setEnabled (! proc.getNotes().empty());
    audioBtn.setEnabled (ready);

    roll.updatePlayhead();
}

bool MainView::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (mods.isCommandDown() && k.getKeyCode() == 'Z')
    {
        if (mods.isShiftDown()) proc.redo(); else proc.undo();
        return true;
    }
    if (mods.isCommandDown() && k.getKeyCode() == 'Y')
    {
        proc.redo();
        return true;
    }

    // 1..9 = the tools, in the order of the toolbar
    const auto c = k.getTextCharacter();
    if (! mods.isCommandDown() && ! mods.isAltDown() && c >= '1' && c <= '9')
    {
        const int t = (int) (c - '1');
        if (t < PianoRoll::numTools)
        {
            setTool (t);
            return true;
        }
    }

    // B held = hear the original voice
    if ((c == 'b' || c == 'B') && ! mods.isAnyModifierKeyDown())
    {
        if (! bKeyHeld)
        {
            bKeyHeld = true;
            abButton.setHeldFromKey (true);
            proc.setHoldOriginal (true);
        }
        return true;
    }

    // space = play / stop the DAW (ARA)
    if (k.getKeyCode() == juce::KeyPress::spaceKey && ! mods.isAnyModifierKeyDown())
        return proc.requestHostPlayToggle();

    return false;
}

bool MainView::keyStateChanged (bool isKeyDown)
{
    if (! isKeyDown && bKeyHeld && ! juce::KeyPress::isKeyCurrentlyDown ('b') && ! juce::KeyPress::isKeyCurrentlyDown ('B'))
    {
        bKeyHeld = false;
        abButton.setHeldFromKey (false);
        proc.setHoldOriginal (abButton.isDown());
        return true;
    }
    return false;
}

void MainView::setKeyMode (bool manual)
{
    if (manual && ! proc.isKeyManual())
    {
        // start from the key in use, so switching to manual changes nothing until you pick another
        const int key = proc.getEffectiveKey();
        if (key >= 0)
            if (auto* p = proc.params.getParameter ("manualKey"))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) key));
    }
    if (auto* p = proc.params.getParameter ("keyMode"))
        p->setValueNotifyingHost (manual ? 1.0f : 0.0f);
    refresh (false);
}

//==============================================================================
ScribaPitchEditor::ScribaPitchEditor (ScribaPitchProcessor& p)
    : AudioProcessorEditor (&p), view (p)
{
    addAndMakeVisible (view);
    setResizable (true, true);
    setResizeLimits (MainView::designW * 3 / 4, MainView::designH * 3 / 4,
                     MainView::designW * 2, MainView::designH * 2);
    getConstrainer()->setFixedAspectRatio ((double) MainView::designW / MainView::designH);
    setSize (MainView::designW, MainView::designH);
    startTimerHz (30);
}

ScribaPitchEditor::~ScribaPitchEditor()
{
    stopTimer();
}

void ScribaPitchEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

void ScribaPitchEditor::resized()
{
    view.setBounds (0, 0, MainView::designW, MainView::designH);
    view.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) MainView::designW));
}
