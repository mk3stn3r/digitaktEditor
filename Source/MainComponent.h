/*
    MainComponent.h - the application window.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "DigitaktCore.h"
#include "DigitaktMidi.h"
#include "PatternGrid.h"
#include <juce_audio_utils/juce_audio_utils.h>

#include "DumpRequester.h"
#include "PoolPanel.h"
#include "SamplePool.h"
#include "SequencerEngine.h"
#include "SamplesPanel.h"

class MainComponent : public juce::Component,
                      private PatternGrid::Listener,
                      private DigitaktMidi::Listener,
                      private SamplesPanel::Listener,
                      private DumpRequester::Listener,
                      private PoolPanel::Listener,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // File actions, also reachable from the menu bar.
    void newPattern();
    void openFile();
    void saveFile (bool saveAs);
    void importMidi();
    void sendToDevice();
    void toggleReceive();
    void downloadPattern();
    void showProbe();
    void togglePlay();
    void showAudioSettings();

    /** Loads a .syx given on the command line. Returns false on failure. */
    bool loadFile (const juce::File&);

    bool hasUnsavedChanges() const noexcept { return dirty; }

private:
    void selectionChanged (int track, int step) override;
    void patternEdited() override;

    void assignSampleToSlot (const dtk::api::DirEntry&, int slot) override;
    void statusMessage (const juce::String&, bool isError) override;

    void dumpReceived (const dtk::ElkDump&) override;
    void receiveProgress (int bytes) override;
    void midiError (const juce::String&) override;
    void rawSysExReceived (const dtk::Bytes&) override;

    void dumpsArriving (int count, const juce::String& latest) override;
    void dumpsComplete (std::vector<dtk::ElkDump>) override;
    void dumpsFailed (const juce::String& why) override;

    void poolChanged() override;
    void refreshEngine();

    void timerCallback() override;

    /** Runs `proceed` immediately when clean, or after the user confirms. */
    void withDiscardConfirmation (std::function<void()> proceed);
    void loadDumps (std::vector<dtk::ElkDump>, const juce::String& sourceName);
    void refreshHeaderFields();
    void refreshInspector();
    void refreshMidiPorts();
    void setStatus (const juce::String&, bool isError = false);
    void markDirty();
    dtk::ElkDump* activeDump();

    //== state ================================================================
    std::vector<dtk::ElkDump> dumps;
    int activeDumpIndex = -1;
    std::unique_ptr<dtk::PatternEditor> editor;
    juce::File currentFile;
    bool dirty = false;

    DigitaktMidi midi;
    DumpRequester requester;

    // Offline playback: the app is a working sample sequencer with no
    // instrument attached.
    SamplePool pool;
    SequencerEngine engine { pool };
    juce::AudioDeviceManager audioDevices;
    bool audioReady = false;
    juce::String audioStartupError;

    static constexpr int portScanTicks = 60;   // ~2 s at a 33 ms tick
    int portScanCountdown = 1;

    //== widgets ==============================================================
    juce::TextButton newButton     { "New" };
    juce::TextButton openButton    { "Open" };
    juce::TextButton saveButton    { "Save" };
    juce::TextButton importButton  { "Import MIDI" };
    juce::TextButton downloadButton { "Download" };
    juce::TextButton receiveButton { "Receive" };
    juce::TextButton sendButton    { "Send" };
    juce::TextButton probeButton   { "Probe" };
    juce::TextButton playButton    { "Play" };
    juce::TextButton audioButton   { "Audio..." };
    juce::ToggleButton loopButton  { "Loop" };

    juce::Label      nameLabel     { {}, "Name" };
    juce::TextEditor nameEditor;
    juce::Label      tempoLabel    { {}, "Tempo" };
    juce::Slider     tempoSlider   { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Label      lengthLabel   { {}, "Length" };
    juce::Slider     lengthSlider  { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Label      slotLabel     { {}, "Slot" };
    juce::Slider     slotSlider    { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };

    juce::Label      portLabel     { {}, "Digitakt port" };
    juce::ComboBox   outputPortBox, inputPortBox;

    PatternGrid grid;
    juce::Viewport gridViewport;
    juce::Component gridPage;
    SamplesPanel samplesPanel;
    PoolPanel poolPanel { pool, engine };
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    juce::Label inspectorLabel;
    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Array<juce::MidiDeviceInfo> outputs, inputs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
