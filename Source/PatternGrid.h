/*
    PatternGrid.h - the step grid: sixteen tracks down, steps across,
    laid out the way the machine itself is.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "DigitaktCore.h"

class PatternGrid : public juce::Component
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void selectionChanged (int track, int step) = 0;
        virtual void patternEdited() = 0;
    };

    PatternGrid() { setWantsKeyboardFocus (true); }

    void setEditor (dtk::PatternEditor* e)
    {
        editor = e;
        selectedTrack = selectedStep = 0;
        repaint();
    }

    void setListener (Listener* l) { listener = l; }

    /** Highlights the step currently sounding, or -1 when stopped. */
    void setPlayhead (int step)
    {
        if (step == playhead) return;
        playhead = step;
        repaint();
    }

    int getSelectedTrack() const noexcept { return selectedTrack; }
    int getSelectedStep()  const noexcept { return selectedStep; }

    void setSelection (int track, int step)
    {
        if (editor == nullptr) return;
        selectedTrack = juce::jlimit (0, editor->numTracks() - 1, track);
        selectedStep  = juce::jlimit (0, editor->stepCount() - 1, step);
        if (listener != nullptr) listener->selectionChanged (selectedTrack, selectedStep);
        repaint();
    }

    void paint (juce::Graphics&) override;
    void resized() override {}
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Height needed to show every track without scrolling. */
    int idealHeight() const
    {
        const int tracks = editor != nullptr ? editor->numTracks() : 16;
        return headerHeight + tracks * rowHeight + 8;
    }

private:
    struct Cell { int track = -1, step = -1; };
    Cell cellAt (juce::Point<int>) const;
    juce::Rectangle<int> cellBounds (int track, int step) const;
    int stepsPerRow() const;

    static constexpr int labelWidth   = 56;
    static constexpr int headerHeight = 22;
    static constexpr int rowHeight    = 26;
    static constexpr int cellWidth    = 30;

    dtk::PatternEditor* editor = nullptr;
    Listener* listener = nullptr;
    int selectedTrack = 0, selectedStep = 0;
    int playhead = -1;
    bool dragValue = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternGrid)
};
