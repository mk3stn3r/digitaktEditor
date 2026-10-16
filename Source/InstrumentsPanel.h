/*
    InstrumentsPanel.h - assign a VST3 or LV2 instrument to each MIDI track.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginRack.h"

class InstrumentsPanel : public juce::Component,
                         private juce::ListBoxModel,
                         private juce::ChangeListener
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void instrumentsChanged() = 0;
        virtual void externalMidiChanged (bool enabled, int baseChannel) = 0;
        virtual void pluginAudioChanged (bool enabled) = 0;
        virtual void sampleAudioChanged (bool enabled) = 0;
        virtual void statusMessage (const juce::String&, bool isError) = 0;
    };

    explicit InstrumentsPanel (PluginRack&);
    ~InstrumentsPanel() override;

    void setListener (Listener* l) { listener = l; }

    /** Marks which MIDI tracks the loaded pattern actually uses. */
    void setTracksInUse (const std::vector<int>& midiTrackIndices);

    /** Shown in the routing help, so the resampling chain can be checked. */
    void setAudioOutputName (const juce::String&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool sel) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void scanForPlugins();
    void assignToSelected();
    void clearSelected();
    void openEditor (int slot);
    void closeEditor (int slot);
    juce::File settingsFile() const;
    void loadKnownPlugins();
    void saveKnownPlugins();

    PluginRack& rack;
    Listener* listener = nullptr;

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    std::vector<int> tracksInUse;

    juce::ListBox list { "instruments", this };
    juce::TextButton scanButton   { "Scan for plugins..." };
    juce::TextButton assignButton { "Assign to track..." };
    juce::TextButton clearButton  { "Clear track" };
    juce::TextButton editorButton { "Open editor" };
    juce::ToggleButton genericEditorToggle { "Generic parameter editor" };
    juce::Label header;
    juce::Label hint;

    juce::ToggleButton playFromDigitakt { "Play these from the Digitakt's MIDI tracks" };
    juce::Label      baseChannelLabel { {}, "M1 channel" };
    juce::Slider     baseChannelSlider { juce::Slider::IncDecButtons,
                                         juce::Slider::TextBoxLeft };
    juce::ToggleButton pluginAudioToggle { "Instrument audio to the output" };
    juce::ToggleButton sampleAudioToggle { "Sample tracks to the output" };
    juce::Label      routingLabel;
    juce::Label      routingHelp;

    // One window per slot, never more: createEditorIfNeeded returns the same
    // editor object each time, so a second owning window would delete it twice.
    std::array<std::unique_ptr<juce::DocumentWindow>, PluginRack::numSlots> editors;
    std::unique_ptr<juce::DialogWindow> scanWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentsPanel)
};
