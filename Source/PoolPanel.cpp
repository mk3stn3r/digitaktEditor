#include "PoolPanel.h"

namespace
{
    const juce::Colour bgColour   { 0xff1b1d21 };
    const juce::Colour rowAlt     { 0xff212429 };
    const juce::Colour textCol    { 0xffd8dee6 };
    const juce::Colour textDim    { 0xff7d858f };
    const juce::Colour usedColour { 0xff7fc7a8 };
    const juce::Colour dropColour { 0xff8fbf7f };
}

PoolPanel::PoolPanel (SamplePool& p, SequencerEngine& e) : pool (p), engine (e)
{
    addAndMakeVisible (list);
    list.setColour (juce::ListBox::backgroundColourId, bgColour);
    list.setRowHeight (22);
    list.setMultipleSelectionEnabled (true);

    auto add = [this] (juce::TextButton& b, std::function<void()> fn)
    {
        addAndMakeVisible (b);
        b.onClick = std::move (fn);
    };
    add (loadButton,  [this] { loadIntoSelected(); });
    add (clearButton, [this] { clearSelected(); });
    add (auditionButton, [this]
    {
        const int row = list.getSelectedRow();
        if (row >= 0) engine.triggerAudition (SamplePool::instrumentSlot (row));
    });

    addAndMakeVisible (header);
    header.setColour (juce::Label::textColourId, textCol);
    header.setFont (juce::FontOptions (13.0f, juce::Font::bold));

    addAndMakeVisible (hint);
    hint.setColour (juce::Label::textColourId, textDim);
    hint.setFont (juce::FontOptions (11.5f));
    hint.setText ("Drop audio files onto a slot to load it. Double-click to audition. "
                  "Slots highlighted in green are used by the loaded pattern.",
                  juce::dontSendNotification);
    refresh();
}

void PoolPanel::setSlotsInUse (const std::vector<int>& used)
{
    slotsInUse = used;
    list.repaint();
}

void PoolPanel::refresh()
{
    header.setText ("Local sample pool - " + juce::String (pool.numLoaded())
                    + " of 128 slots loaded", juce::dontSendNotification);
    list.updateContent();
    list.repaint();
}

void PoolPanel::paint (juce::Graphics& g) { g.fillAll (bgColour); }

void PoolPanel::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto toolbar = area.removeFromTop (28);
    loadButton    .setBounds (toolbar.removeFromLeft (130).reduced (2));
    clearButton   .setBounds (toolbar.removeFromLeft (90).reduced (2));
    auditionButton.setBounds (toolbar.removeFromLeft (90).reduced (2));

    area.removeFromTop (4);
    header.setBounds (area.removeFromTop (20));
    hint.setBounds (area.removeFromBottom (18));
    area.removeFromBottom (4);
    list.setBounds (area);
}

//==============================================================================
int PoolPanel::getNumRows() { return SamplePool::numSlots; }

void PoolPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= SamplePool::numSlots) return;

    const bool inUse = std::find (slotsInUse.begin(), slotsInUse.end(),
                                  SamplePool::instrumentSlot (row)) != slotsInUse.end();

    g.fillAll (selected ? rowAlt.brighter (0.25f)
                        : (row == dropRow ? dropColour.withAlpha (0.2f)
                                          : (row % 2 ? rowAlt : bgColour)));

    const auto& s = pool.slot (row);

    g.setColour (inUse ? usedColour : textDim);
    g.setFont (juce::FontOptions (11.0f, inUse ? juce::Font::bold : juce::Font::plain));
    g.drawText (juce::String (SamplePool::instrumentSlot (row)),
                6, 0, 34, h, juce::Justification::centredRight);

    g.setColour (s.empty() ? textDim.withAlpha (0.5f) : textCol);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText (s.empty() ? "(empty)" : s.name, 48, 0, w - 130, h,
                juce::Justification::centredLeft);

    if (! s.empty())
    {
        g.setColour (textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (s.seconds(), 2) + " s", w - 76, 0, 68, h,
                    juce::Justification::centredRight);
    }
}

void PoolPanel::listBoxItemClicked (int, const juce::MouseEvent&) {}

void PoolPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (row < 0 || pool.slot (row).empty()) return;
    engine.triggerAudition (SamplePool::instrumentSlot (row));
}

//==============================================================================
bool PoolPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        const auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac"
            || ext == ".ogg" || ext == ".mp3")
            return true;
    }
    return false;
}

void PoolPanel::fileDragEnter (const juce::StringArray& f, int x, int y)
{
    fileDragMove (f, x, y);
}

void PoolPanel::fileDragMove (const juce::StringArray&, int x, int y)
{
    const auto local = list.getLocalPoint (this, juce::Point<int> (x, y));
    dropRow = list.getRowContainingPosition (local.x, local.y);
    list.repaint();
}

void PoolPanel::fileDragExit (const juce::StringArray&)
{
    dropRow = -1;
    list.repaint();
}

void PoolPanel::filesDropped (const juce::StringArray& files, int x, int y)
{
    const auto local = list.getLocalPoint (this, juce::Point<int> (x, y));
    int row = list.getRowContainingPosition (local.x, local.y);
    dropRow = -1;

    // Dropping past the end fills from the first free slot rather than failing.
    if (row < 0)
    {
        row = 0;
        while (row < SamplePool::numSlots && ! pool.slot (row).empty()) ++row;
        if (row >= SamplePool::numSlots) row = 0;
    }
    loadFiles (files, row);
}

void PoolPanel::loadFiles (const juce::StringArray& paths, int firstSlot)
{
    int slot = firstSlot;
    int loaded = 0;
    juce::StringArray problems;

    for (const auto& p : paths)
    {
        while (slot < SamplePool::numSlots && ! pool.slot (slot).empty()
               && paths.size() > 1)
            ++slot;                        // a multi-file drop fills free slots

        if (slot >= SamplePool::numSlots)
        {
            problems.add ("ran out of free slots");
            break;
        }

        juce::String error;
        if (pool.loadFromFile (slot, juce::File (p), error)) { ++loaded; ++slot; }
        else problems.add (error);
    }

    refresh();
    if (listener != nullptr)
    {
        listener->poolChanged();
        listener->statusMessage (
            juce::String (loaded) + " sample(s) loaded into the pool"
                + (problems.isEmpty() ? "" : " - " + problems.joinIntoString ("; ")),
            ! problems.isEmpty());
    }
}

void PoolPanel::loadIntoSelected()
{
    const int row = juce::jmax (0, list.getSelectedRow());

    chooser = std::make_unique<juce::FileChooser> (
        "Load audio into slot " + juce::String (SamplePool::instrumentSlot (row)),
        juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::canSelectMultipleItems,
        [this, row] (const juce::FileChooser& fc)
        {
            juce::StringArray paths;
            for (const auto& f : fc.getResults()) paths.add (f.getFullPathName());
            if (! paths.isEmpty()) loadFiles (paths, row);
        });
}

void PoolPanel::clearSelected()
{
    for (int i = 0; i < list.getNumSelectedRows(); ++i)
        pool.clearSlot (list.getSelectedRow (i));

    refresh();
    if (listener != nullptr) listener->poolChanged();
}
