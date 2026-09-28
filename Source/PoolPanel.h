/*
    PoolPanel.h - the local sample pool: 128 slots you can fill from disk so
    the sequencer plays with no Digitakt attached.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SamplePool.h"
#include "SequencerEngine.h"

class PoolPanel : public juce::Component,
                  public juce::FileDragAndDropTarget,
                  private juce::ListBoxModel
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void poolChanged() = 0;
        virtual void statusMessage (const juce::String&, bool isError) = 0;
    };

    PoolPanel (SamplePool&, SequencerEngine&);

    void setListener (Listener* l) { listener = l; }

    /** Highlights the slots the loaded pattern actually uses. */
    void setSlotsInUse (const std::vector<int>& instrumentSlots);

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;

    void refresh();

private:
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void loadIntoSelected();
    void loadFiles (const juce::StringArray& paths, int firstSlot);
    void clearSelected();

    SamplePool& pool;
    SequencerEngine& engine;
    Listener* listener = nullptr;

    std::vector<int> slotsInUse;
    int dropRow = -1;

    juce::ListBox list { "pool", this };
    juce::TextButton loadButton { "Load into slot..." };
    juce::TextButton clearButton { "Clear slot" };
    juce::TextButton auditionButton { "Audition" };
    juce::Label header;
    juce::Label hint;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PoolPanel)
};
