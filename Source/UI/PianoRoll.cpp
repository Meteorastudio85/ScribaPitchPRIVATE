#include "PianoRoll.h"
#include <algorithm>
#include <cmath>

using namespace theme;

namespace
{
    constexpr double minNoteSeconds = 0.03;
    constexpr double neighbourGap = 0.002;

    bool isBlackKey (int midi) { const int pc = ((midi % 12) + 12) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

    float medianPitch (const sp::PitchTrack& tr, double a, double b, float fallback)
    {
        std::vector<float> v;
        for (size_t i = 0; i < tr.midi.size(); ++i)
        {
            const double t = tr.frameTime ((int) i);
            if (t >= a && t <= b && tr.midi[i] > 0.0f) v.push_back (tr.midi[i]);
        }
        if (v.empty()) return fallback;
        std::nth_element (v.begin(), v.begin() + (long) (v.size() / 2), v.end());
        return v[v.size() / 2];
    }
}

juce::String PianoRoll::toolName (int t)
{
    static const char* names[] = { "MAIN", "PITCH", "PENCIL", "PITCH MODULATION", "PITCH DRIFT", "FORMANT", "AMPLITUDE", "TIMING", "NOTE SEPARATION" };
    return names[juce::jlimit (0, numTools - 1, t)];
}

juce::String PianoRoll::toolHint (int t)
{
    static const char* hints[] = {
        "DRAG = MOVE  \xc2\xb7  EDGES = STRETCH  \xc2\xb7  ALT = NO SNAP",
        "DRAG UP / DOWN = CHANGE NOTE  \xc2\xb7  ALT = NO SNAP",
        "DRAW THE CURVE  \xc2\xb7  ALT = ERASE",
        "DRAG UP = MORE VIBRATO",
        "DRAG UP = STRAIGHTER NOTE",
        "DRAG UP / DOWN = TIMBRE",
        "DRAG UP / DOWN = VOLUME",
        "DRAG = MOVE  \xc2\xb7  EDGES = STRETCH",
        "CLICK = SPLIT THE NOTE"
    };
    return juce::CharPointer_UTF8 (hints[juce::jlimit (0, numTools - 1, t)]);
}

static juce::MouseCursor pencilCursor()
{
    static const juce::MouseCursor cursor = []
    {
        juce::Image img (juce::Image::ARGB, 24, 24, true);
        juce::Graphics g (img);
        juce::Path p = juce::Drawable::parseSVGPath ("M3 21l1.5-5L16 4.5l3.5 3.5L8 19.5zM14 6.5l3.5 3.5");
        g.setColour (juce::Colours::black);
        g.strokePath (p, juce::PathStrokeType (3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (juce::Colours::white);
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return juce::MouseCursor (img, 3, 21);
    }();
    return cursor;
}

const char* PianoRoll::toolIcon (int t)
{
    static const char* icons[] = {
        "M5 3l14 8-6 2-2 6z",
        "M12 4v16M8 8l4-4 4 4M8 16l4 4 4-4",
        "M4 20l1.5-5L16.5 4l3.5 3.5L9 18.5zM14 6.5l3.5 3.5",
        "M3 12c2-4 4-4 6 0s4 4 6 0 4-4 6 0",
        "M3 16c4 0 5-8 9-8s5 8 9 8",
        "M4 18l4-12 4 8 4-4 4 8",
        "M4 11v2M8 7v10M12 4v16M16 8v8M20 11v2",
        "M12 7v5l3 3M12 21a9 9 0 1 0 0-18a9 9 0 0 0 0 18z",
        "M12 3v18M7 8l-4 4 4 4M17 8l4 4-4 4"
    };
    return icons[juce::jlimit (0, numTools - 1, t)];
}

PianoRoll::PianoRoll (ScribaPitchProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    setOpaque (true);
}

//==============================================================================
void PianoRoll::refreshFromProcessor (bool fitView)
{
    if (! dragging)
        notes = proc.getNotes();
    analysis = proc.getAnalysis();
    scale = proc.getScale();
    songKey = proc.getEffectiveKey();
    chords = proc.getLinked().chords;
    totalSeconds = proc.getTakeSeconds();
    bool hasPpq = false;
    proc.getTempo (bpm, startPpq, sigNum, sigDen, hasPpq);
    if (bpm <= 0.0) bpm = 120.0;
    if (! hasPpq) startPpq = 0.0;

    if (indexOfId (selectedId) < 0)
        selectedId = -1;

    if (fitView && ! notes.empty())
    {
        float lo = 200.0f, hi = -1.0f;
        for (const auto& n : notes) { lo = std::min (lo, n.newCenter()); hi = std::max (hi, n.newCenter()); }
        const auto grid = gridArea();
        const float rows = grid.getHeight() / rowH;
        topMidi = std::ceil ((lo + hi) * 0.5f + rows * 0.5f);
        const double visible = juce::jlimit (4.0, 16.0, notes.back().outEnd - notes.front().outStart + 1.0);
        pxPerSec = grid.getWidth() / visible;
        scrollSec = std::max (0.0, notes.front().outStart - 0.3);
        clampView();
    }
    repaint();
}

void PianoRoll::updatePlayhead()
{
    float x = -1.0f;
    if (totalSeconds > 0.0)
    {
        const auto grid = gridArea();
        const float px = xForTime (proc.getPlayheadSeconds());
        if (px >= grid.getX() && px <= grid.getRight()) x = px;
    }
    if (std::abs (x - playheadX) < 0.5f) return;

    const auto h = (float) getHeight();
    if (playheadX >= 0.0f) repaint (juce::Rectangle<float> (playheadX - 9.0f, 0.0f, 18.0f, h).toNearestInt());
    playheadX = x;
    if (playheadX >= 0.0f) repaint (juce::Rectangle<float> (playheadX - 9.0f, 0.0f, 18.0f, h).toNearestInt());
}

const sp::Note* PianoRoll::getSelectedNote() const noexcept
{
    const int i = indexOfId (selectedId);
    return i >= 0 ? &notes[(size_t) i] : nullptr;
}

void PianoRoll::editSelected (const std::function<void (sp::Note&)>& change)
{
    const int i = indexOfId (selectedId);
    if (i < 0) return;
    change (notes[(size_t) i]);
    commit();
}

//==============================================================================
juce::Rectangle<float> PianoRoll::gridArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft (keysW).withTrimmedTop (topH());
}

float  PianoRoll::xForTime (double t) const { return gridArea().getX() + (float) ((t - scrollSec) * pxPerSec); }
double PianoRoll::timeForX (float x) const  { return scrollSec + (double) (x - gridArea().getX()) / pxPerSec; }
float  PianoRoll::yForMidi (float m) const  { return gridArea().getY() + (topMidi - m + 0.5f) * rowH; }
float  PianoRoll::midiForY (float y) const  { return topMidi + 0.5f - (y - gridArea().getY()) / rowH; }

juce::Rectangle<float> PianoRoll::noteRect (const sp::Note& n) const
{
    const float x0 = xForTime (n.outStart), x1 = xForTime (n.outEnd);
    const float yc = yForMidi (n.newCenter());
    return { x0, yc - (rowH - 6.0f) * 0.5f, juce::jmax (2.0f, x1 - x0), rowH - 6.0f };
}

juce::Rectangle<float> PianoRoll::getNoteBounds (int id) const
{
    const int i = indexOfId (id);
    return i >= 0 ? noteRect (notes[(size_t) i]) : juce::Rectangle<float>();
}

int PianoRoll::indexOfId (int id) const
{
    for (size_t i = 0; i < notes.size(); ++i)
        if (notes[i].id == id) return (int) i;
    return -1;
}

sp::Tuning PianoRoll::tuningFor (const sp::Note& n) const
{
    return sp::tuningOf (n, scale);
}

juce::Colour PianoRoll::colourFor (sp::Tuning t)
{
    switch (t)
    {
        case sp::Tuning::outOfKey: return wrong;
        case sp::Tuning::offCents: return accent;
        case sp::Tuning::inTune:
        default:                   return inKey;
    }
}

float PianoRoll::snapPitch (float desired) const
{
    if (snap == 0) return std::round (desired);
    if (snap == 1) return (float) (scale.valid ? scale.nearest (desired) : (int) std::round (desired));
    return desired;
}

PianoRoll::Hit PianoRoll::hitNote (juce::Point<float> p) const
{
    Hit h;
    if (! gridArea().contains (p)) return h;
    for (int i = (int) notes.size(); --i >= 0;)
    {
        const auto r = noteRect (notes[(size_t) i]).expanded (0.0f, 2.0f);
        if (! r.contains (p)) continue;
        h.index = i;
        const float edge = juce::jmin (9.0f, r.getWidth() * 0.25f);
        const bool edgesActive = (tool == toolMain || tool == toolTiming);
        if (edgesActive && p.x < r.getX() + edge) h.zone = Zone::left;
        else if (edgesActive && p.x > r.getRight() - edge) h.zone = Zone::right;
        else h.zone = Zone::body;
        return h;
    }
    return h;
}

void PianoRoll::clampTime (sp::Note& n, double newStart, double newEnd) const
{
    double lo = 0.0, hi = totalSeconds > 0.0 ? totalSeconds : 1.0e9;
    for (const auto& o : notes)
    {
        if (o.id == n.id) continue;
        if (o.outEnd <= n.outStart + 1.0e-9) lo = std::max (lo, o.outEnd + neighbourGap);
        if (o.outStart >= n.outEnd - 1.0e-9) hi = std::min (hi, o.outStart - neighbourGap);
    }

    const double len = newEnd - newStart;
    if (newStart < lo) { newStart = lo; if (std::abs (len - (n.outEnd - n.outStart)) < 1.0e-9) newEnd = lo + len; }
    if (newEnd > hi)   { newEnd = hi;   if (std::abs (len - (n.outEnd - n.outStart)) < 1.0e-9) newStart = hi - len; }
    newStart = std::max (newStart, lo);
    newEnd = std::min (newEnd, hi);
    if (newEnd - newStart < minNoteSeconds) return;
    n.outStart = newStart;
    n.outEnd = newEnd;
}

void PianoRoll::commit()
{
    proc.applyNotes (notes);
    repaint();
}

void PianoRoll::select (int id)
{
    if (selectedId == id) return;
    selectedId = id;
    repaint();
    if (onSelectionChanged) onSelectionChanged();
}

void PianoRoll::showAt (double seconds, float midi)
{
    const auto grid = gridArea();
    scrollSec = seconds - grid.getWidth() * 0.5 / pxPerSec;
    topMidi = midi + grid.getHeight() * 0.5f / rowH;
    clampView();
    repaint();
}

void PianoRoll::clampView()
{
    pxPerSec = juce::jlimit (15.0, 2500.0, pxPerSec);
    const double visible = gridArea().getWidth() / pxPerSec;
    scrollSec = juce::jlimit (-0.5, std::max (0.0, totalSeconds - visible * 0.5), scrollSec);
    topMidi = juce::jlimit (30.0f, 100.0f, topMidi);
}

void PianoRoll::splitNoteAt (int index, double t)
{
    auto& a = notes[(size_t) index];
    if (t - a.outStart < minNoteSeconds || a.outEnd - t < minNoteSeconds) return;

    int maxId = 0;
    for (const auto& n : notes) maxId = std::max (maxId, n.id);

    const double frac = (t - a.outStart) / a.outLength();
    const double tin = a.inStart + frac * a.inLength();

    sp::Note b = a;
    b.id = maxId + 1;
    b.outStart = t;
    b.inStart = tin;
    a.outEnd = t;
    a.inEnd = tin;

    if (a.hasDrawing())
        a.splitDrawing ((float) frac, b.drawU, b.drawOff);
    else
        b.clearDrawing();

    if (analysis != nullptr)   // each half shows its own sung pitch, with the same edits
    {
        const float ca = a.newCenter(), cb = b.newCenter();
        a.origMidi = medianPitch (analysis->track, a.inStart, a.inEnd, a.origMidi);
        b.origMidi = medianPitch (analysis->track, b.inStart, b.inEnd, b.origMidi);
        for (auto& o : a.drawOff) o += ca - a.newCenter();   // the drawn pitch stays where it was
        for (auto& o : b.drawOff) o += cb - b.newCenter();
    }
    notes.insert (notes.begin() + index + 1, b);
    commit();
}

//==============================================================================
void PianoRoll::flash (const juce::String& message)
{
    flashText = message;
    flashUntil = juce::Time::getMillisecondCounter() + 3500;
    repaint();
    juce::Timer::callAfterDelay (3600, [safe = juce::Component::SafePointer<PianoRoll> (this)]
    {
        if (safe != nullptr) safe->repaint();
    });
}

void PianoRoll::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    // ruler: move the DAW cursor there (ARA)
    if (e.position.y < topH() && e.position.x > keysW)
    {
        if (e.getNumberOfClicks() >= 2) { if (onPlayToggle) onPlayToggle(); }
        else if (onSeek) onSeek (juce::jmax (0.0, timeForX (e.position.x)));
        return;
    }

    if (tool == toolDraw && gridArea().contains (e.position))
    {
        proc.pushUndo();
        drawing = true;
        drawErase = e.mods.isAltDown();
        lastDrawPos = e.position;
        drawLastId = drawStroke (e.position, e.position, drawErase);
        if (drawLastId >= 0) select (drawLastId);
        return;
    }

    const auto hit = hitNote (e.position);

    if (hit.index < 0)
    {
        select (-1);
        panning = true;
        panScroll = scrollSec;
        panTop = topMidi;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }

    auto& n = notes[(size_t) hit.index];
    select (n.id);

    if (tool == toolSeparation)
    {
        proc.pushUndo();
        splitNoteAt (hit.index, timeForX (e.position.x));
        return;
    }

    proc.pushUndo();
    dragging = true;
    dragZone = hit.zone;
    dragOrig = n;
    lastPreviewSemi = (int) std::round (n.newCenter());
    proc.previewNote (n, n.newCenter(), true);   // sounds while the mouse is held
}

void PianoRoll::mouseDrag (const juce::MouseEvent& e)
{
    const float dx = (float) e.getDistanceFromDragStartX();
    const float dy = (float) e.getDistanceFromDragStartY();

    if (drawing)
    {
        const int id = drawStroke (lastDrawPos, e.position, drawErase);
        lastDrawPos = e.position;
        if (id >= 0 && id != drawLastId)
        {
            drawLastId = id;
            select (id);
        }
        commit();
        return;
    }

    if (panning)
    {
        scrollSec = panScroll - dx / pxPerSec;
        topMidi = panTop + dy / rowH;
        clampView();
        repaint();
        return;
    }

    if (! dragging) return;
    const int i = indexOfId (dragOrig.id);
    if (i < 0) return;

    sp::Note n = dragOrig;
    const double dt = dx / pxPerSec;
    const bool movePitch = (tool == toolMain || tool == toolPitch) && dragZone == Zone::body;
    // the main tool moves in time only for a clear sideways drag: a hand moving a note
    // up or down always wobbles a few pixels, and that must not shift its timing
    const bool moveTime  = (tool == toolMain && dragZone == Zone::body && std::abs (dx) > 8.0f && std::abs (dx) > 0.5f * std::abs (dy))
                           || tool == toolTiming || dragZone != Zone::body;

    if (dragZone == Zone::left)
        clampTime (n, juce::jmin (dragOrig.outStart + dt, dragOrig.outEnd - minNoteSeconds), dragOrig.outEnd);
    else if (dragZone == Zone::right)
        clampTime (n, dragOrig.outStart, juce::jmax (dragOrig.outEnd + dt, dragOrig.outStart + minNoteSeconds));
    else if (moveTime && tool != toolPitch)
        clampTime (n, dragOrig.outStart + dt, dragOrig.outEnd + dt);

    if (movePitch && std::abs (dy) > 3.0f)
    {
        const float desired = dragOrig.newCenter() - dy / rowH;
        if (snap == 2 || e.mods.isAltDown()) n.moveCenterTo (desired, false);   // Alt / Option: no snap
        else           n.moveCenterTo (snapPitch (desired + (dy < 0.0f ? 0.02f : -0.02f)), true);   // ties follow the drag
    }

    switch (tool)
    {
        case toolModulation: n.vibAmt   = juce::jlimit (0.0f, 2.0f, dragOrig.vibAmt - dy / 100.0f); break;
        case toolDrift:      n.driftAmt = juce::jlimit (0.0f, 1.0f, dragOrig.driftAmt - dy / 150.0f); break;
        case toolFormant:    n.formant  = juce::jlimit (-12.0f, 12.0f, dragOrig.formant - dy / 40.0f); break;
        case toolAmplitude:  n.gainDb   = juce::jlimit (-24.0f, 12.0f, dragOrig.gainDb - dy / 8.0f); break;
        default: break;
    }

    notes[(size_t) i] = n;
    commit();

    // the tone follows the note (to the cent when dragging with Alt)
    lastPreviewSemi = (int) std::round (n.newCenter());
    proc.previewNote (n, n.newCenter(), true);
}

void PianoRoll::mouseUp (const juce::MouseEvent& e)
{
    if (drawing)
    {
        drawing = false;
        proc.dropUndoIfUnchanged();
        if (auto* n = getSelectedNote())
            proc.previewRendered (*n);   // hear the drawn note once it has been rendered
    }
    if (dragging) proc.stopPreview();
    dragging = false;
    panning = false;
    proc.dropUndoIfUnchanged();   // a plain click is not an edit
    dragZone = Zone::none;
    updateCursor (e.position);
    if (onSelectionChanged) onSelectionChanged();
}

void PianoRoll::mouseMove (const juce::MouseEvent& e)
{
    updateCursor (e.position);
}

void PianoRoll::mouseExit (const juce::MouseEvent&)
{
    if (hoverId != -1) { hoverId = -1; repaint(); }
}

void PianoRoll::updateCursor (juce::Point<float> p)
{
    const auto hit = hitNote (p);
    const int id = hit.index >= 0 ? notes[(size_t) hit.index].id : -1;
    if (id != hoverId) { hoverId = id; repaint(); }

    if (p.y < topH() && p.x > keysW)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);   // ruler: move the DAW cursor
        return;
    }

    if (tool == toolDraw)
    {
        setMouseCursor (gridArea().contains (p) ? pencilCursor() : juce::MouseCursor::NormalCursor);
        return;
    }

    if (hit.zone == Zone::left || hit.zone == Zone::right)   setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (hit.zone == Zone::body && tool == toolSeparation) setMouseCursor (juce::MouseCursor::IBeamCursor);
    else if (hit.zone == Zone::body && tool == toolTiming)     setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (hit.zone == Zone::body && tool != toolMain)       setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else if (hit.zone == Zone::body)                           setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else                                                       setMouseCursor (juce::MouseCursor::NormalCursor);
}

void PianoRoll::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.position.y < topH()) return;   // the ruler handles its own double-click

    if (tool == toolDraw)   // double-click: back to the sung curve on that note
    {
        const int i = noteIndexAtTime (timeForX (e.position.x));
        if (i < 0 || ! notes[(size_t) i].hasDrawing()) return;
        proc.pushUndo();
        notes[(size_t) i].clearDrawing();
        commit();
        return;
    }

    const auto hit = hitNote (e.position);
    if (hit.index < 0 || tool == toolSeparation) return;
    auto& n = notes[(size_t) hit.index];
    proc.pushUndo();
    n.moveCenterTo (snapPitch (n.newCenter()), snap != 2);
    if (snap == 2) n.moveCenterTo (std::round (n.newCenter()), true);
    commit();
    proc.previewNote (n, n.newCenter());
}

int PianoRoll::noteIndexAtTime (double t) const
{
    for (size_t i = 0; i < notes.size(); ++i)
        if (t >= notes[i].outStart && t < notes[i].outEnd) return (int) i;
    return -1;
}

int PianoRoll::drawStroke (juce::Point<float> a, juce::Point<float> b, bool erase)
{
    // walk the mouse path every 2 px; each note gets the points that fall on it
    const float len = juce::jmax (std::abs (b.x - a.x), 1.0f);
    const int steps = juce::jmax (1, (int) std::ceil (len / 2.0f));
    int current = -1, lastId = -1;
    float uMin = 0.0f, uMax = 0.0f;
    std::vector<std::pair<float, float>> pts;

    auto flush = [&]
    {
        if (current < 0) return;
        auto& n = notes[(size_t) current];
        if (erase) n.eraseDrawing (uMin, uMax);
        else       n.drawSegment (uMin, uMax, pts);
        pts.clear();
    };

    for (int k = 0; k <= steps; ++k)
    {
        const auto p = a + (b - a) * ((float) k / (float) steps);
        const double t = timeForX (p.x);
        const int idx = noteIndexAtTime (t);
        if (idx != current) { flush(); current = idx; uMin = 1.0e9f; uMax = -1.0e9f; }
        if (idx < 0) continue;

        const auto& n = notes[(size_t) idx];
        const float u = (float) juce::jlimit (0.0, 1.0, (t - n.outStart) / n.outLength());
        uMin = juce::jmin (uMin, u);
        uMax = juce::jmax (uMax, u);
        pts.push_back ({ u, juce::jlimit (-24.0f, 24.0f, midiForY (p.y) - n.newCenter()) });
        lastId = n.id;
    }
    flush();
    return lastId;
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const double tAtMouse = timeForX (e.position.x);
        pxPerSec *= std::pow (1.15, (double) w.deltaY * 6.0);
        clampView();
        scrollSec = tAtMouse - (double) (e.position.x - gridArea().getX()) / pxPerSec;
    }
    else if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        const float d = std::abs (w.deltaX) > 0.0f ? w.deltaX : w.deltaY;
        scrollSec -= (double) d * 300.0 / pxPerSec;
    }
    else
    {
        topMidi += w.deltaY * 12.0f;
    }
    clampView();
    repaint();
}

bool PianoRoll::keyPressed (const juce::KeyPress& k)
{
    const int i = indexOfId (selectedId);
    if (i < 0) return false;

    const int code = k.getKeyCode();
    const bool shift = k.getModifiers().isShiftDown();
    auto n = notes[(size_t) i];

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const float step = code == juce::KeyPress::upKey ? 1.0f : -1.0f;
        proc.pushUndo();
        n.moveCenterTo (std::round (n.newCenter()) + step, true);
        if (snap == 1 && scale.valid)
            while (! scale.contains ((int) std::round (n.newCenter()))) n.moveCenterTo (std::round (n.newCenter()) + step, true);
        notes[(size_t) i] = n;
        commit();
        proc.previewNote (n, n.newCenter());
        return true;
    }

    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const double d = (code == juce::KeyPress::rightKey ? 1.0 : -1.0) * 0.01;
        proc.pushUndo();
        if (shift) clampTime (n, n.outStart, juce::jmax (n.outStart + minNoteSeconds, n.outEnd + d));
        else       clampTime (n, n.outStart + d, n.outEnd + d);
        notes[(size_t) i] = n;
        commit();
        if (onSelectionChanged) onSelectionChanged();
        return true;
    }
    return false;
}

//==============================================================================
void PianoRoll::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto grid = gridArea();
    const auto ruler = juce::Rectangle<float> (grid.getX(), laneH(), grid.getWidth(), rulerH);

    g.fillAll (bg);
    paintRows (g, grid);
    paintBeats (g, grid, ruler);
    paintChords (g, grid);

    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (grid.toNearestInt());

        for (const auto& n : notes)
            if (n.id != selectedId) paintNote (g, n, false);
        if (auto* sel = getSelectedNote())
            paintNote (g, *sel, true);

        if (notes.empty())
        {
            const auto st = proc.getStatus();
            juce::String msg = "PRESS ANALYZE, THEN PLAY THE SONG IN YOUR DAW";
            if (st == ScribaPitchProcessor::Status::armed)     msg = "PRESS PLAY IN YOUR DAW  (THE DAW IS NOT USING ARA: SEE \"NO ARA\" AT THE TOP)";
            if (st == ScribaPitchProcessor::Status::recording) msg = "LISTENING...  STOP THE DAW WHEN THE VOCAL ENDS";
            if (st == ScribaPitchProcessor::Status::analyzing) msg = "ANALYZING THE VOICE...";
            if (st == ScribaPitchProcessor::Status::loading)   msg = "LOADING...";
            if (st == ScribaPitchProcessor::Status::reading)   msg = "READING THE TRACK FROM THE DAW...";
            if (st == ScribaPitchProcessor::Status::idle && proc.isAraMode())
                msg = "WAITING FOR THE AUDIO OF THE TRACK";
            drawText (g, msg, grid, 14.0f, Weight::black, muted, 0.14f, juce::Justification::centred);
            const auto diag = proc.getAraDiagnostics();
            if (diag.isNotEmpty())
            {
                drawText (g, diag, grid.withTrimmedTop (grid.getHeight() * 0.5f + 18.0f).withHeight (18.0f), 11.0f,
                          Weight::black, accent, 0.1f, juce::Justification::centred);
                drawText (g, "LOG: " + ScribaPitchProcessor::getLogFile().getFullPathName(),
                          grid.withTrimmedTop (grid.getHeight() * 0.5f + 40.0f).withHeight (16.0f), 9.5f,
                          Weight::bold, dim, 0.04f, juce::Justification::centred);
            }
        }
        else if (proc.getStatus() != ScribaPitchProcessor::Status::missingTake)
            paintSummary (g, grid);

        if (! notes.empty() && proc.getStatus() == ScribaPitchProcessor::Status::missingTake)
        {
            drawText (g, "THE RECORDED TAKE WAS NOT FOUND ON THIS COMPUTER: PRESS ANALYZE AGAIN",
                      grid.withHeight (40.0f), 12.0f, Weight::black, accent, 0.12f, juce::Justification::centred);
        }

        if (playheadX >= 0.0f)
        {
            g.setColour (white.withAlpha (0.85f));
            g.fillRect (playheadX - 1.0f, grid.getY(), 2.0f, grid.getHeight());
        }
    }

    if (playheadX >= 0.0f)
    {
        juce::Path tri;
        tri.addTriangle (playheadX - 7.0f, topH() - 9.0f, playheadX + 7.0f, topH() - 9.0f, playheadX, topH());
        g.setColour (white);
        g.fillPath (tri);
    }

    if (flashText.isNotEmpty() && juce::Time::getMillisecondCounter() < flashUntil)
    {
        const auto box = juce::Rectangle<float> (grid.getCentreX() - 330.0f, grid.getY() + 14.0f, 660.0f, 32.0f);
        g.setColour (panel2.withAlpha (0.96f));
        g.fillRoundedRectangle (box, 8.0f);
        g.setColour (accent);
        g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);
        drawText (g, flashText, box, 11.0f, Weight::black, accent, 0.1f, juce::Justification::centred);
    }

    g.setColour (line);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);
}

/** After reading the track: the key, where it comes from, and how many notes are
    red (out of the key), orange (off by some cents) and light azure (in the key). */
void PianoRoll::paintSummary (juce::Graphics& g, juce::Rectangle<float> grid)
{
    int red = 0, orange = 0, ok = 0;
    for (const auto& n : notes)
    {
        const auto t = tuningFor (n);
        if (t == sp::Tuning::outOfKey) ++red; else if (t == sp::Tuning::offCents) ++orange; else ++ok;
    }

    juce::String keyText = "NO KEY YET  " + juce::String (juce::CharPointer_UTF8 ("\xc2\xb7")) + "  ADD SCRIBA TO A GUITAR / BASS / PIANO TRACK";
    const int key = proc.getEffectiveKey();
    const auto src = proc.getKeySource();
    if (scale.valid && key >= 0)
    {
        keyText = "KEY " + sp::keyLongName (key) + "  ";
        if (src == ScribaPitchProcessor::KeySource::scriba)
            keyText << "FROM SCRIBA (" << proc.getLinked().sources.joinIntoString (" + ") << ")";
        else if (src == ScribaPitchProcessor::KeySource::manual)
            keyText << "(MANUAL)";
        else
            keyText << "FROM THE VOCAL (SCRIBA NOT FOUND)";
    }

    const auto f = font (10.0f, Weight::black, 0.08f);
    struct Item { juce::String text; juce::Colour c; };
    std::vector<Item> items { { keyText, text } };
    if (scale.valid)
    {
        items.push_back ({ juce::String (red) + " OUT OF KEY", wrong });
        items.push_back ({ juce::String (orange) + " OFF BY CENTS", accent });
        items.push_back ({ juce::String (ok) + " IN KEY", inKey });
    }
    float w = 16.0f;
    for (size_t i = 0; i < items.size(); ++i)
        w += juce::GlyphArrangement::getStringWidth (f, items[i].text) + (i == 0 ? 8.0f : 30.0f);
    const auto box = juce::Rectangle<float> (grid.getX() + 10.0f, grid.getBottom() - 36.0f, juce::jmin (w, grid.getWidth() - 20.0f), 26.0f);
    g.setColour (panel.withAlpha (0.94f));
    g.fillRoundedRectangle (box, 13.0f);
    g.setColour (line2);
    g.drawRoundedRectangle (box.reduced (0.5f), 13.0f, 1.0f);
    float x = box.getX() + 12.0f;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
        {
            g.setColour (items[i].c);
            g.fillEllipse (x + 10.0f, box.getCentreY() - 4.0f, 8.0f, 8.0f);
            x += 22.0f;
        }
        const float tw = juce::GlyphArrangement::getStringWidth (f, items[i].text) + 2.0f;
        drawText (g, items[i].text, { x, box.getY(), tw, box.getHeight() }, 10.0f, Weight::black,
                  i == 0 ? text : items[i].c, 0.08f);
        x += tw + 8.0f;
    }
}

void PianoRoll::paintRows (juce::Graphics& g, juce::Rectangle<float> grid)
{
    const auto keys = juce::Rectangle<float> (0.0f, grid.getY(), keysW, grid.getHeight());
    g.setColour (header);
    g.fillRect (keys);
    g.fillRect (0.0f, 0.0f, keysW, topH());
    if (! chords.empty())
        drawText (g, "CHORDS", { 0.0f, 0.0f, keysW - 6.0f, laneH() }, 8.5f, Weight::black, tonic, 0.08f,
                  juce::Justification::centredRight);

    const int lo = (int) std::floor (midiForY (grid.getBottom()));
    const int hi = (int) std::ceil (midiForY (grid.getY()));

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (juce::Rectangle<float> (0.0f, grid.getY(), (float) getWidth(), grid.getHeight()).toNearestInt());

    for (int m = lo; m <= hi; ++m)
    {
        const float y = yForMidi ((float) m) - rowH * 0.5f;
        const bool inKey = scale.valid ? scale.contains (m) : ! isBlackKey (m);
        const bool isTonic = scale.valid && ((m - scale.tonicPc) % 12 + 12) % 12 == 0;

        g.setColour (isTonic ? panel.interpolatedWith (tonic, 0.10f) : (inKey ? rowKey : rowOut));
        g.fillRect (grid.getX(), y, grid.getWidth(), rowH);
        g.setColour (juce::Colour (0xff22272e));
        g.fillRect (grid.getX(), y + rowH - 1.0f, grid.getWidth(), 1.0f);

        g.setColour (gridMinor);
        g.fillRect (0.0f, y + rowH - 1.0f, keysW, 1.0f);
        drawText (g, sp::noteName (m), { 0.0f, y, keysW - 8.0f, rowH }, 11.0f, Weight::bold,
                  isTonic ? tonic : (inKey ? text2 : dim), 0.0f, juce::Justification::centredRight);
    }

    g.setColour (line);
    g.fillRect (keysW - 1.0f, 0.0f, 1.0f, (float) getHeight());
}

void PianoRoll::paintBeats (juce::Graphics& g, juce::Rectangle<float> grid, juce::Rectangle<float> ruler)
{
    g.setColour (header);
    g.fillRect (ruler);
    g.setColour (line);
    g.fillRect (ruler.getX(), ruler.getBottom() - 1.0f, ruler.getWidth(), 1.0f);

    const double beatQ = 4.0 / juce::jmax (1, sigDen);
    const double barQ = beatQ * juce::jmax (1, sigNum);
    const double secPerQ = 60.0 / bpm;
    const double beatPx = beatQ * secPerQ * pxPerSec;
    const bool drawBeats = beatPx > 10.0;
    const int barSkip = (barQ * secPerQ * pxPerSec) < 40.0 ? 4 : 1;

    const double q0 = startPpq + timeForX (grid.getX()) / secPerQ;
    const double q1 = startPpq + timeForX (grid.getRight()) / secPerQ;

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (juce::Rectangle<float> (grid.getX(), 0.0f, grid.getWidth(), (float) getHeight()).toNearestInt());

    for (double q = std::ceil (q0 / beatQ) * beatQ; q <= q1 + 1.0e-9; q += beatQ)
    {
        const double barPos = q / barQ;
        const bool isBar = std::abs (barPos - std::round (barPos)) < 1.0e-6;
        const float x = xForTime ((q - startPpq) * secPerQ);

        if (isBar)
        {
            const int bar = (int) std::round (barPos) + 1;
            g.setColour (line2);
            g.fillRect (x, grid.getY(), 1.0f, grid.getHeight());
            if ((bar - 1) % barSkip == 0)
            {
                g.fillRect (x, ruler.getY() + 4.0f, 1.0f, ruler.getHeight() - 4.0f);
                drawText (g, juce::String (bar), { x + 5.0f, ruler.getY(), 60.0f, ruler.getHeight() }, 11.0f,
                          Weight::bold, muted, 0.0f);
            }
        }
        else if (drawBeats)
        {
            g.setColour (gridMinor);
            g.fillRect (x, grid.getY(), 1.0f, grid.getHeight());
        }
    }
}

void PianoRoll::paintChords (juce::Graphics& g, juce::Rectangle<float> grid)
{
    if (chords.empty()) return;
    const auto lane = juce::Rectangle<float> (grid.getX(), 0.0f, grid.getWidth(), laneH());
    g.setColour (header);
    g.fillRect (lane);

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (lane.toNearestInt());
    const double secPerQ = 60.0 / bpm;
    const auto tonicChord = sp::keyShortName (songKey);

    for (const auto& c : chords)
    {
        const float x0 = xForTime ((c.ppq - startPpq) * secPerQ);
        const float x1 = xForTime ((c.ppq + c.lengthPpq - startPpq) * secPerQ);
        if (x1 < lane.getX() || x0 > lane.getRight()) continue;
        const auto r = juce::Rectangle<float> (x0, 2.0f, juce::jmax (4.0f, x1 - x0 - 3.0f), laneH() - 4.0f);
        const bool isTonic = c.name == tonicChord;
        g.setColour (isTonic ? panel.interpolatedWith (tonic, 0.22f) : rowKey);
        g.fillRoundedRectangle (r, 4.0f);
        const auto visible = r.getIntersection (lane);   // keep the name readable when the chord starts off-screen
        if (visible.getWidth() > 18.0f)
            drawText (g, c.name, visible.withTrimmedLeft (6.0f), 11.0f, Weight::black, isTonic ? tonic : text2, 0.04f);
    }
}

void PianoRoll::paintNote (juce::Graphics& g, const sp::Note& n, bool selected)
{
    const auto r = noteRect (n);
    const auto grid = gridArea();
    if (r.getRight() < grid.getX() - 80.0f || r.getX() > grid.getRight() + 10.0f)
        return;

    const float centre = n.newCenter();
    const auto tuning = tuningFor (n);
    const bool out = tuning == sp::Tuning::outOfKey;
    const auto col = scale.valid ? colourFor (tuning) : noKeyNote;   // no key yet: neutral until Scriba / the vocal gives it
    const float radius = juce::jmin (9.0f, r.getHeight() * 0.5f, r.getWidth() * 0.5f);

    if (out)
    {
        const int target = scale.nearest (centre);
        const auto ghost = r.withY (yForMidi ((float) target) - r.getHeight() * 0.5f);
        juce::Path gp, dashed;
        gp.addRoundedRectangle (ghost, radius);
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, gp, dashes, 2);
        g.setColour (wrong);
        g.fillPath (dashed);

        juce::Path conn, connDashed;
        conn.startNewSubPath (r.getCentreX(), r.getCentreY());
        conn.lineTo (r.getCentreX(), ghost.getCentreY());
        const float dots[] = { 2.0f, 3.0f };
        juce::PathStrokeType (2.0f).createDashedStroke (connDashed, conn, dots, 2);
        g.fillPath (connDashed);

        const auto tag = "-> " + sp::noteName (target);
        const auto tagR = juce::Rectangle<float> (ghost.getRight() + 4.0f, ghost.getY() + 1.0f, 44.0f, 16.0f);
        g.setColour (wrong);
        g.fillRoundedRectangle (tagR, 4.0f);
        drawText (g, tag, tagR, 10.0f, Weight::black, ink, 0.0f, juce::Justification::centred);
    }

    g.setColour (rowOut.interpolatedWith (col, 0.22f));
    g.fillRoundedRectangle (r, radius);

    // pitch curve of the corrected note
    if (analysis != nullptr && r.getWidth() > 4.0f)
    {
        juce::Graphics::ScopedSaveState s (g);
        const bool drawn = n.hasDrawing();
        g.reduceClipRegion ((drawn ? grid : r.expanded (0.0f, rowH * 0.6f)).toNearestInt());
        juce::Path curve, drawnCurve;
        bool started = false, drawnStarted = false;
        const double inPerOut = n.outLength() > 0.0 ? n.inLength() / n.outLength() : 1.0;
        const float x0 = juce::jmax (r.getX(), grid.getX()), x1 = juce::jmin (r.getRight(), grid.getRight());
        for (float x = x0; x <= x1; x += 2.0f)
        {
            const double tout = timeForX (x);
            const double tin = n.inStart + (tout - n.outStart) * inPerOut;
            const float m = n.newMidiAt (analysis->track, tin);
            if (m <= 0.0f) { started = drawnStarted = false; continue; }
            float y = r.getCentreY() + (centre - m) * rowH;
            if (! drawn) y = juce::jlimit (r.getY() - rowH * 0.5f, r.getBottom() + rowH * 0.5f, y);
            if (! started) { curve.startNewSubPath (x, y); started = true; }
            else curve.lineTo (x, y);

            const bool here = drawn && n.isDrawnAt ((float) ((tin - n.inStart) / juce::jmax (1.0e-6, n.inLength())));
            if (here) { if (! drawnStarted) { drawnCurve.startNewSubPath (x, y); drawnStarted = true; } else drawnCurve.lineTo (x, y); }
            else drawnStarted = false;
        }
        g.setColour (col);
        g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (drawn)   // hand-drawn parts in white, a little thicker
        {
            g.setColour (white);
            g.strokePath (drawnCurve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // stretch handles
    if ((selected || n.id == hoverId) && (tool == toolMain || tool == toolTiming))
    {
        const float edge = juce::jmin (9.0f, r.getWidth() * 0.25f);
        juce::Graphics::ScopedSaveState s (g);
        juce::Path clip;
        clip.addRoundedRectangle (r, radius);
        g.reduceClipRegion (clip);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.fillRect (r.withWidth (edge));
        g.fillRect (r.withLeft (r.getRight() - edge));
    }

    g.setColour (selected ? juce::Colours::white : col);
    g.drawRoundedRectangle (r.reduced (selected ? 1.0f : 0.5f), radius, selected ? 2.0f : 1.0f);

    const int cents = juce::roundToInt ((centre - std::round (centre)) * 100.0f);
    juce::String label = sp::noteName ((int) std::round (centre));
    if (cents != 0) label << (cents > 0 ? " +" : " ") << cents;
    if (r.getWidth() > 26.0f || selected)
        drawText (g, label, { r.getX(), r.getY() - 15.0f, 90.0f, 14.0f }, 10.0f, Weight::bold,
                  tuning == sp::Tuning::inTune ? text3 : col, 0.0f);
}
