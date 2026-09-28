#include "PatternGrid.h"

namespace
{
    // A restrained palette: the grid should read at a glance, so colour is
    // used to distinguish trig states and beat divisions, nothing else.
    const juce::Colour bgColour        { 0xff1b1d21 };
    const juce::Colour rowAlt          { 0xff212429 };
    const juce::Colour gridLine        { 0xff2f333a };
    const juce::Colour beatLine        { 0xff444a54 };
    const juce::Colour textDim         { 0xff7d858f };
    const juce::Colour textBright      { 0xffd8dee6 };
    const juce::Colour trigPlain       { 0xff4c8ed9 };
    const juce::Colour trigWithNote    { 0xffe0a33c };
    const juce::Colour selectionColour { 0xffe8eef5 };
    const juce::Colour midiTrackTint   { 0xff262a30 };
    const juce::Colour sampleLockColour{ 0xff7fc7a8 };
    const juce::Colour playheadColour  { 0xffe8eef5 };
}

int PatternGrid::stepsPerRow() const
{
    return editor != nullptr ? editor->stepCount() : 64;
}

juce::Rectangle<int> PatternGrid::cellBounds (int track, int step) const
{
    return { labelWidth + step * cellWidth, headerHeight + track * rowHeight,
             cellWidth, rowHeight };
}

PatternGrid::Cell PatternGrid::cellAt (juce::Point<int> p) const
{
    if (editor == nullptr) return {};
    if (p.x < labelWidth || p.y < headerHeight) return {};

    const int step  = (p.x - labelWidth) / cellWidth;
    const int track = (p.y - headerHeight) / rowHeight;

    if (step < 0 || step >= editor->stepCount())   return {};
    if (track < 0 || track >= editor->numTracks()) return {};
    return { track, step };
}

void PatternGrid::paint (juce::Graphics& g)
{
    g.fillAll (bgColour);

    if (editor == nullptr)
    {
        g.setColour (textDim);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("No pattern loaded. Use File > New, Open, or Receive from Digitakt.",
                    getLocalBounds(), juce::Justification::centred);
        return;
    }

    const int nSteps  = editor->stepCount();
    const int nTracks = editor->numTracks();
    const int patLen  = editor->length();

    // step ruler
    g.setFont (juce::FontOptions (11.0f));
    for (int s = 0; s < nSteps; ++s)
    {
        const auto r = juce::Rectangle<int> (labelWidth + s * cellWidth, 0,
                                             cellWidth, headerHeight);
        g.setColour (s % 4 == 0 ? textBright : textDim);
        g.drawText (juce::String (s + 1), r, juce::Justification::centred);
    }

    for (int t = 0; t < nTracks; ++t)
    {
        const bool audio = editor->isAudioTrack (t);
        const auto rowArea = juce::Rectangle<int> (0, headerHeight + t * rowHeight,
                                                   getWidth(), rowHeight);
        g.setColour (audio ? (t % 2 ? rowAlt : bgColour) : midiTrackTint);
        g.fillRect (rowArea);

        // label, with the sample slot for audio tracks
        g.setColour (t == selectedTrack ? textBright : textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (dtk::trackLabel (t),
                    juce::Rectangle<int> (4, rowArea.getY(), 28, rowHeight),
                    juce::Justification::centredLeft);

        if (audio)
        {
            g.setColour (textDim.withAlpha (0.8f));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (juce::String (editor->sampleSlot (t)),
                        juce::Rectangle<int> (32, rowArea.getY(), 20, rowHeight),
                        juce::Justification::centredLeft);
        }

        for (int s = 0; s < nSteps; ++s)
        {
            auto cell = cellBounds (t, s).reduced (2);
            const auto step = editor->get (t, s);
            const bool beyond = s >= patLen;

            if (step.trig)
            {
                auto c = step.note.has_value() ? trigWithNote : trigPlain;
                if (beyond) c = c.withAlpha (0.28f);

                // Velocity shows as fill height, so dynamics are visible
                // without opening the inspector.
                const float amount = step.velocity.has_value()
                                   ? juce::jlimit (0.25f, 1.0f, *step.velocity / 127.0f)
                                   : 1.0f;
                auto filled = cell.toFloat();
                filled = filled.withTrimmedTop (filled.getHeight() * (1.0f - amount));
                g.setColour (c);
                g.fillRoundedRectangle (filled, 2.0f);
                g.setColour (c.withAlpha (beyond ? 0.3f : 0.55f));
                g.drawRoundedRectangle (cell.toFloat(), 2.0f, 1.0f);
            }
            else
            {
                g.setColour (beyond ? gridLine.withAlpha (0.4f) : gridLine);
                g.drawRoundedRectangle (cell.toFloat(), 2.0f, 1.0f);
            }

            // A sample lock overrides which +Drive sample this step fires, so
            // it earns the slot number printed right in the cell.
            if (step.sampleLock.has_value())
            {
                const auto strip = cell.removeFromBottom (10);
                g.setColour (sampleLockColour.withAlpha (beyond ? 0.4f : 1.0f));
                g.fillRoundedRectangle (strip.toFloat(), 1.5f);
                g.setColour (bgColour);
                g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
                g.drawText (juce::String (*step.sampleLock), strip,
                            juce::Justification::centred);
            }
        }
    }

    // beat divisions on top of everything
    for (int s = 0; s <= nSteps; s += 4)
    {
        const int x = labelWidth + s * cellWidth;
        g.setColour (s % 16 == 0 ? beatLine : gridLine);
        g.drawVerticalLine (x, (float) headerHeight,
                            (float) (headerHeight + nTracks * rowHeight));
    }

    // playhead
    if (playhead >= 0 && playhead < nSteps)
    {
        const auto col = juce::Rectangle<int> (labelWidth + playhead * cellWidth,
                                               headerHeight, cellWidth,
                                               nTracks * rowHeight);
        g.setColour (playheadColour.withAlpha (0.16f));
        g.fillRect (col);
        g.setColour (playheadColour.withAlpha (0.7f));
        g.drawVerticalLine (col.getX(), (float) col.getY(), (float) col.getBottom());
    }

    // selection
    const auto sel = cellBounds (selectedTrack, selectedStep);
    g.setColour (selectionColour);
    g.drawRoundedRectangle (sel.reduced (1).toFloat(), 3.0f, 1.6f);
}

void PatternGrid::mouseDown (const juce::MouseEvent& e)
{
    if (editor == nullptr) return;
    grabKeyboardFocus();

    const auto cell = cellAt (e.getPosition());
    if (cell.track < 0) return;

    setSelection (cell.track, cell.step);

    if (! e.mods.isRightButtonDown())
    {
        const bool now = ! editor->get (cell.track, cell.step).trig;
        editor->setTrig (cell.track, cell.step, now);
        dragValue = now;
        if (listener != nullptr) listener->patternEdited();
        repaint();
    }
}

void PatternGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (editor == nullptr || e.mods.isRightButtonDown()) return;

    const auto cell = cellAt (e.getPosition());
    if (cell.track < 0) return;

    if (editor->get (cell.track, cell.step).trig != dragValue)
    {
        editor->setTrig (cell.track, cell.step, dragValue);
        setSelection (cell.track, cell.step);
        if (listener != nullptr) listener->patternEdited();
        repaint();
    }
}

bool PatternGrid::keyPressed (const juce::KeyPress& key)
{
    if (editor == nullptr) return false;

    const auto code = key.getKeyCode();
    auto bump = [this] (const char* field, int delta, int lo, int hi)
    {
        const auto step = editor->get (selectedTrack, selectedStep);
        std::optional<int> cur;
        if      (juce::String (field) == "note")     cur = step.note;
        else if (juce::String (field) == "velocity") cur = step.velocity;
        else                                         cur = step.length;

        const auto& track = editor->patternKit().pattern.tracks[(size_t) selectedTrack];
        if (! cur.has_value())
            cur = juce::String (field) == "note"     ? track.defaultNote()
                : juce::String (field) == "velocity" ? track.defaultVelocity()
                                                     : 14;

        const int v = juce::jlimit (lo, hi, *cur + delta);
        if      (juce::String (field) == "note")     editor->setNote (selectedTrack, selectedStep, v);
        else if (juce::String (field) == "velocity") editor->setVelocity (selectedTrack, selectedStep, v);
        else                                         editor->setStepLength (selectedTrack, selectedStep, v);

        if (juce::String (field) == "note" && ! step.trig)
            editor->setTrig (selectedTrack, selectedStep, true);

        if (listener != nullptr) { listener->patternEdited();
                                   listener->selectionChanged (selectedTrack, selectedStep); }
        repaint();
    };

    if (code == juce::KeyPress::leftKey)  { setSelection (selectedTrack, selectedStep - 1); return true; }
    if (code == juce::KeyPress::rightKey) { setSelection (selectedTrack, selectedStep + 1); return true; }
    if (code == juce::KeyPress::upKey)    { setSelection (selectedTrack - 1, selectedStep); return true; }
    if (code == juce::KeyPress::downKey)  { setSelection (selectedTrack + 1, selectedStep); return true; }

    if (code == juce::KeyPress::spaceKey)
    {
        const bool now = ! editor->get (selectedTrack, selectedStep).trig;
        editor->setTrig (selectedTrack, selectedStep, now);
        if (listener != nullptr) { listener->patternEdited();
                                   listener->selectionChanged (selectedTrack, selectedStep); }
        repaint();
        return true;
    }

    const auto ch = key.getTextCharacter();
    switch (ch)
    {
        case 'n': bump ("note",     -1, 0, 127); return true;
        case 'N': bump ("note",      1, 0, 127); return true;
        case 'v': bump ("velocity", -1, 0, 127); return true;
        case 'V': bump ("velocity",  1, 0, 127); return true;
        case 'l': bump ("length",   -1, 1, 127); return true;
        case 'L': bump ("length",    1, 1, 127); return true;
        case 'c':
            editor->setNote (selectedTrack, selectedStep, std::nullopt);
            editor->setVelocity (selectedTrack, selectedStep, std::nullopt);
            editor->setStepLength (selectedTrack, selectedStep, std::nullopt);
            editor->setSampleLock (selectedTrack, selectedStep, std::nullopt);
            if (listener != nullptr) { listener->patternEdited();
                                       listener->selectionChanged (selectedTrack, selectedStep); }
            repaint();
            return true;
        case 's': case 'S':
        {
            // Sample lock: cycle the slot, wrapping past the pool size.
            const auto cur = editor->sampleLock (selectedTrack, selectedStep);
            const int delta = (ch == 'S') ? 1 : -1;
            int next = cur.has_value() ? *cur + delta : (delta > 0 ? 1 : 127);
            if (next < 1 || next > 127)
            {
                editor->setSampleLock (selectedTrack, selectedStep, std::nullopt);
            }
            else if (! editor->setSampleLock (selectedTrack, selectedStep, next))
            {
                if (listener != nullptr)
                    listener->patternEdited();
                return true;    // all 80 lanes claimed; nothing changed
            }
            if (! editor->get (selectedTrack, selectedStep).trig)
                editor->setTrig (selectedTrack, selectedStep, true);
            if (listener != nullptr) { listener->patternEdited();
                                       listener->selectionChanged (selectedTrack, selectedStep); }
            repaint();
            return true;
        }
        case 'd':
            editor->setSampleLock (selectedTrack, selectedStep, std::nullopt);
            if (listener != nullptr) { listener->patternEdited();
                                       listener->selectionChanged (selectedTrack, selectedStep); }
            repaint();
            return true;
        case 'C':
            editor->clearTrack (selectedTrack);
            if (listener != nullptr) { listener->patternEdited();
                                       listener->selectionChanged (selectedTrack, selectedStep); }
            repaint();
            return true;
        default: break;
    }
    return false;
}
