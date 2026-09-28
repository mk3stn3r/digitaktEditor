#include "MainComponent.h"

namespace
{
    const juce::Colour panelBg { 0xff16181c };
    const juce::Colour textCol { 0xffd8dee6 };
    const juce::Colour errCol  { 0xffe07a5f };
    const juce::Colour okCol   { 0xff8fbf7f };
}

MainComponent::MainComponent()
{
    auto addButton = [this] (juce::TextButton& b, std::function<void()> fn)
    {
        addAndMakeVisible (b);
        b.onClick = std::move (fn);
    };

    addButton (newButton,     [this] { newPattern(); });
    addButton (openButton,    [this] { openFile(); });
    addButton (saveButton,    [this] { saveFile (false); });
    addButton (importButton,  [this] { importMidi(); });
    addButton (downloadButton, [this] { downloadPattern(); });
    addButton (receiveButton, [this] { toggleReceive(); });
    addButton (sendButton,    [this] { sendToDevice(); });
    addButton (probeButton,   [this] { showProbe(); });
    addButton (playButton,    [this] { togglePlay(); });
    addButton (audioButton,   [this] { showAudioSettings(); });

    addAndMakeVisible (loopButton);
    loopButton.setToggleState (true, juce::dontSendNotification);
    loopButton.setColour (juce::ToggleButton::textColourId, textCol.withAlpha (0.8f));
    loopButton.onClick = [this] { engine.setLooping (loopButton.getToggleState()); };

    for (auto* l : { &nameLabel, &tempoLabel, &lengthLabel, &slotLabel, &portLabel })
    {
        addAndMakeVisible (*l);
        l->setColour (juce::Label::textColourId, textCol.withAlpha (0.7f));
        l->setFont (juce::FontOptions (12.0f));
        l->setJustificationType (juce::Justification::centredRight);
    }

    addAndMakeVisible (nameEditor);
    nameEditor.setTextToShowWhenEmpty ("(unnamed)", textCol.withAlpha (0.4f));
    nameEditor.onTextChange = [this]
    {
        if (editor == nullptr) return;
        editor->setName (nameEditor.getText().toStdString());
        markDirty();
    };

    addAndMakeVisible (tempoSlider);
    tempoSlider.setRange (30.0, 300.0, 0.5);
    tempoSlider.onValueChange = [this]
    {
        if (editor == nullptr) return;
        editor->setTempo (tempoSlider.getValue());
        markDirty();
    };

    addAndMakeVisible (lengthSlider);
    lengthSlider.setRange (1.0, 64.0, 1.0);
    lengthSlider.onValueChange = [this]
    {
        if (editor == nullptr) return;
        editor->setLength ((int) lengthSlider.getValue());
        markDirty();
        grid.repaint();
    };

    addAndMakeVisible (slotSlider);
    slotSlider.setRange (0.0, 127.0, 1.0);
    slotSlider.onValueChange = [this]
    {
        if (auto* d = activeDump()) { d->index = (int) slotSlider.getValue(); markDirty(); }
    };

    addAndMakeVisible (outputPortBox);
    addAndMakeVisible (inputPortBox);
    outputPortBox.onChange = [this]
    {
        const int i = outputPortBox.getSelectedItemIndex();
        if (i >= 0 && i < outputs.size())
        {
            midi.closeOutput();
            if (midi.openOutput (outputs[i].identifier))
            {
                samplesPanel.setMidiOutput (midi.rawOutput());
                requester.setOutput (midi.rawOutput());
            }
            else
                setStatus ("could not open " + outputs[i].name + " for output", true);
        }
    };
    inputPortBox.onChange = [this]
    {
        const int i = inputPortBox.getSelectedItemIndex();
        if (i >= 0 && i < inputs.size())
        {
            midi.closeInput();
            if (! midi.openInput (inputs[i].identifier))
                setStatus ("could not open " + inputs[i].name + " for input", true);
        }
    };

    gridViewport.setViewedComponent (&grid, false);
    gridViewport.setScrollBarsShown (true, true);
    gridPage.addAndMakeVisible (gridViewport);
    grid.setListener (this);

    samplesPanel.setListener (this);

    addAndMakeVisible (tabs);
    tabs.setOutline (0);
    tabs.setColour (juce::TabbedComponent::backgroundColourId, panelBg);
    tabs.addTab ("Pattern", panelBg, &gridPage, false);
    tabs.addTab ("+Drive samples", panelBg, &samplesPanel, false);
    tabs.addTab ("Sample pool", panelBg, &poolPanel, false);
    poolPanel.setListener (this);

    // Open the default audio output. Failing is not fatal - everything except
    // local playback still works - so it is reported, not thrown.
    audioStartupError = audioDevices.initialiseWithDefaultDevices (0, 2);
    if (audioStartupError.isEmpty())
    {
        audioDevices.addAudioCallback (&engine);
        audioReady = true;
    }
    // Play stays enabled even without a device: clicking it explains the
    // problem, which survives later status messages in a way a startup line
    // does not.

    addAndMakeVisible (inspectorLabel);
    inspectorLabel.setColour (juce::Label::textColourId, textCol);
    inspectorLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                               13.0f, juce::Font::plain));

    addAndMakeVisible (statusLabel);
    statusLabel.setFont (juce::FontOptions (12.0f));

    midi.setListener (this);
    requester.setListener (this);
    refreshMidiPorts();
    // Fast enough to animate the playhead; port scanning is decimated.
    startTimer (33);

    newPattern();
    setSize (1180, 680);
}

MainComponent::~MainComponent()
{
    // Remove the callback before anything the audio thread touches goes away.
    if (audioReady) audioDevices.removeAudioCallback (&engine);
    engine.stop();
    midi.setListener (nullptr);
    stopTimer();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (panelBg);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto toolbar = area.removeFromTop (30);
    for (auto* b : { &newButton, &openButton, &saveButton, &importButton })
        b->setBounds (toolbar.removeFromLeft (b == &importButton ? 100 : 66).reduced (2));
    toolbar.removeFromLeft (12);
    downloadButton.setBounds (toolbar.removeFromLeft (86).reduced (2));
    receiveButton.setBounds (toolbar.removeFromLeft (86).reduced (2));
    sendButton   .setBounds (toolbar.removeFromLeft (66).reduced (2));
    probeButton  .setBounds (toolbar.removeFromLeft (66).reduced (2));

    toolbar.removeFromLeft (12);
    playButton .setBounds (toolbar.removeFromLeft (66).reduced (2));
    loopButton .setBounds (toolbar.removeFromLeft (62).reduced (2));
    audioButton.setBounds (toolbar.removeFromLeft (74).reduced (2));

    toolbar.removeFromLeft (12);
    portLabel.setBounds (toolbar.removeFromLeft (86));
    outputPortBox.setBounds (toolbar.removeFromLeft (180).reduced (2));
    inputPortBox .setBounds (toolbar.removeFromLeft (180).reduced (2));

    area.removeFromTop (6);
    auto header = area.removeFromTop (28);
    nameLabel  .setBounds (header.removeFromLeft (44));
    nameEditor .setBounds (header.removeFromLeft (170).reduced (2));
    header.removeFromLeft (10);
    tempoLabel .setBounds (header.removeFromLeft (48));
    tempoSlider.setBounds (header.removeFromLeft (150).reduced (2));
    header.removeFromLeft (10);
    lengthLabel .setBounds (header.removeFromLeft (52));
    lengthSlider.setBounds (header.removeFromLeft (130).reduced (2));
    header.removeFromLeft (10);
    slotLabel  .setBounds (header.removeFromLeft (34));
    slotSlider .setBounds (header.removeFromLeft (130).reduced (2));

    area.removeFromTop (6);
    statusLabel.setBounds (area.removeFromBottom (22));
    inspectorLabel.setBounds (area.removeFromBottom (24));
    area.removeFromBottom (4);

    tabs.setBounds (area);
    gridViewport.setBounds (gridPage.getLocalBounds());

    const int steps = editor != nullptr ? editor->stepCount() : 64;
    grid.setSize (56 + steps * 30 + 8, grid.idealHeight());
}

//==============================================================================
dtk::ElkDump* MainComponent::activeDump()
{
    if (activeDumpIndex < 0 || activeDumpIndex >= (int) dumps.size()) return nullptr;
    return &dumps[(size_t) activeDumpIndex];
}

void MainComponent::markDirty()
{
    dirty = true;
    saveButton.setButtonText ("Save *");
}

void MainComponent::setStatus (const juce::String& text, bool isError)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId,
                           isError ? errCol : textCol.withAlpha (0.75f));
}

void MainComponent::loadDumps (std::vector<dtk::ElkDump> newDumps,
                               const juce::String& sourceName)
{
    int index = -1;
    for (size_t i = 0; i < newDumps.size(); ++i)
        if (newDumps[i].patternKit.has_value()) { index = (int) i; break; }

    if (index < 0)
    {
        juce::String detail;
        for (const auto& d : newDumps)
            if (! d.parseError.empty())
                detail += "\n" + juce::String (d.typeName()) + ": " + d.parseError;

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon, "No pattern found",
            sourceName + " contains no pattern dump this app can read." + detail);
        return;
    }

    dumps = std::move (newDumps);
    activeDumpIndex = index;
    editor = std::make_unique<dtk::PatternEditor> (*dumps[(size_t) index].patternKit);
    grid.setEditor (editor.get());
    dirty = false;
    saveButton.setButtonText ("Save");

    refreshHeaderFields();
    refreshInspector();
    refreshEngine();
    resized();

    const auto v = dumps[(size_t) index].patternKit->pattern.version.value;
    setStatus (sourceName + " loaded - storage version " + juce::String (v)
               + ", " + juce::String ((int) dumps.size()) + " message(s)");
}

void MainComponent::refreshHeaderFields()
{
    if (editor == nullptr) return;
    nameEditor.setText (editor->name(), juce::dontSendNotification);
    tempoSlider.setValue (editor->tempo(), juce::dontSendNotification);
    lengthSlider.setRange (1.0, (double) editor->stepCount(), 1.0);
    lengthSlider.setValue (editor->length(), juce::dontSendNotification);
    if (auto* d = activeDump())
        slotSlider.setValue (d->index, juce::dontSendNotification);
}

void MainComponent::refreshInspector()
{
    if (editor == nullptr) { inspectorLabel.setText ({}, juce::dontSendNotification); return; }

    const int t = grid.getSelectedTrack(), s = grid.getSelectedStep();
    const auto step = editor->get (t, s);
    const auto& track = editor->patternKit().pattern.tracks[(size_t) t];

    auto opt = [] (const std::optional<int>& v, const juce::String& fallback)
    {
        return v.has_value() ? juce::String (*v) : fallback;
    };

    juce::String text;
    text << dtk::trackLabel (t) << "  step " << juce::String (s + 1).paddedLeft (' ', 2)
         << "   trig " << (step.trig ? "ON " : "off")
         << "   note " << (step.note.has_value()
                             ? juce::String (*step.note) + " " + dtk::noteName (*step.note)
                             : "- (track default " + juce::String (track.defaultNote()) + ")")
         << "   vel " << opt (step.velocity,
                              "- (default " + juce::String (track.defaultVelocity()) + ")")
         << "   len " << opt (step.length, "-")
         << "   sample " << opt (step.sampleLock, "-")
         << "   sound pool " << opt (step.sampleSlot, "-");

    inspectorLabel.setText (text, juce::dontSendNotification);
}

void MainComponent::refreshMidiPorts()
{
    const auto newOutputs = DigitaktMidi::outputDevices();
    const auto newInputs  = DigitaktMidi::inputDevices();

    auto sameNames = [] (const juce::Array<juce::MidiDeviceInfo>& a,
                         const juce::Array<juce::MidiDeviceInfo>& b)
    {
        if (a.size() != b.size()) return false;
        for (int i = 0; i < a.size(); ++i)
            if (a[i].identifier != b[i].identifier) return false;
        return true;
    };

    if (sameNames (newOutputs, outputs) && sameNames (newInputs, inputs)) return;

    outputs = newOutputs;
    inputs  = newInputs;

    outputPortBox.clear (juce::dontSendNotification);
    inputPortBox .clear (juce::dontSendNotification);

    for (int i = 0; i < outputs.size(); ++i)
        outputPortBox.addItem ("out: " + outputs[i].name, i + 1);
    for (int i = 0; i < inputs.size(); ++i)
        inputPortBox.addItem ("in: " + inputs[i].name, i + 1);

    if (outputs.isEmpty()) outputPortBox.setTextWhenNoChoicesAvailable ("no MIDI outputs");
    if (inputs.isEmpty())  inputPortBox .setTextWhenNoChoicesAvailable ("no MIDI inputs");

    // Pick the Digitakt automatically so the common case needs no setup.
    const int o = DigitaktMidi::indexOfDigitakt (outputs);
    const int i = DigitaktMidi::indexOfDigitakt (inputs);
    if (o >= 0) outputPortBox.setSelectedItemIndex (o, juce::sendNotification);
    if (i >= 0) inputPortBox .setSelectedItemIndex (i, juce::sendNotification);
}

void MainComponent::timerCallback()
{
    if (engine.isPlaying())
    {
        grid.setPlayhead (engine.currentStep());
    }
    else if (playButton.getButtonText() == "Stop")
    {
        // The engine stopped on its own at the end of a non-looping pass.
        playButton.setButtonText ("Play");
        grid.setPlayhead (-1);
    }

    if (--portScanCountdown <= 0)
    {
        portScanCountdown = portScanTicks;
        refreshMidiPorts();
    }
}

//==============================================================================
void MainComponent::selectionChanged (int, int) { refreshInspector(); }

void MainComponent::patternEdited()
{
    markDirty();
    refreshHeaderFields();
    refreshEngine();
}

void MainComponent::refreshEngine()
{
    if (editor == nullptr) return;
    engine.updateFromPattern (editor->patternKit());

    // Show which pool slots this pattern actually reaches for.
    std::vector<int> used;
    const auto& pk = editor->patternKit();
    for (int t = 0; t < 8 && t < (int) pk.kit.sounds.size(); ++t)
    {
        const int s = pk.kit.sounds[(size_t) t].sampleSlot;
        if (s > 0) used.push_back (s);
        for (int i = 0; i < editor->stepCount(); ++i)
            if (const auto lock = dtk::SampleLockLane::get (pk, t, i))
                used.push_back (*lock);
    }
    std::sort (used.begin(), used.end());
    used.erase (std::unique (used.begin(), used.end()), used.end());
    poolPanel.setSlotsInUse (used);
}

void MainComponent::poolChanged()
{
    refreshEngine();
    grid.repaint();
}

void MainComponent::togglePlay()
{
    if (! audioReady)
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon, "No audio output",
            "Local playback needs an audio output, and none could be opened at "
            "startup.\n\n"
            + (audioStartupError.isEmpty() ? juce::String ("No device was found.")
                                           : audioStartupError)
            + "\n\nEverything else - editing, MIDI import, and talking to the "
              "Digitakt - works without it. Open Audio... to choose a device.");
        return;
    }
    if (engine.isPlaying())
    {
        engine.stop();
        playButton.setButtonText ("Play");
        grid.setPlayhead (-1);
        return;
    }

    if (editor == nullptr) return;
    refreshEngine();

    if (pool.numLoaded() == 0)
        setStatus ("Playing, but the sample pool is empty - load audio in the "
                   "Sample pool tab to hear anything.");
    else
        setStatus ("Playing at " + juce::String (editor->tempo(), 1) + " BPM.");

    engine.start();
    playButton.setButtonText ("Stop");
}

void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        audioDevices, 0, 0, 1, 2, false, false, true, false);
    selector->setSize (500, 320);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = "Audio output";
    options.dialogBackgroundColour = panelBg;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();

    if (! audioReady && audioDevices.getCurrentAudioDevice() != nullptr)
    {
        audioDevices.addAudioCallback (&engine);
        audioReady = true;
        audioStartupError.clear();
        setStatus ("Audio output ready - press Play.");
    }
}

//==============================================================================
void MainComponent::newPattern()
{
    withDiscardConfirmation ([this]
    {
        try
        {
            auto pk = dtk::blankPatternKit (dtk::latestDT1PatternVersion);
            std::vector<dtk::ElkDump> d { dtk::makePatternKitDump (pk, 0) };
            currentFile = juce::File();
            loadDumps (std::move (d), "New pattern");
        }
        catch (const std::exception& e)
        {
            setStatus (juce::String ("could not create a blank pattern: ") + e.what(), true);
        }
    });
}

void MainComponent::openFile()
{
    withDiscardConfirmation ([this]
    {
    chooser = std::make_unique<juce::FileChooser> ("Open a Digitakt SysEx dump",
                                                   juce::File(), "*.syx");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file != juce::File()) loadFile (file);
        });
    });
}

bool MainComponent::loadFile (const juce::File& file)
{
    juce::MemoryBlock mb;
    if (! file.existsAsFile() || ! file.loadFileAsData (mb))
    {
        setStatus ("could not read " + file.getFullPathName(), true);
        return false;
    }
    const auto* p = static_cast<const uint8_t*> (mb.getData());

    try
    {
        auto parsed = dtk::parseSysExFile (dtk::Bytes (p, p + mb.getSize()));
        currentFile = file;
        loadDumps (std::move (parsed), file.getFileName());
        return true;
    }
    catch (const std::exception& e)
    {
        setStatus (file.getFileName() + ": " + e.what(), true);
        return false;
    }
}

void MainComponent::saveFile (bool saveAs)
{
    if (editor == nullptr) return;

    auto write = [this] (const juce::File& file)
    {
        if (auto* d = activeDump()) d->refreshContent();
        const auto bytes = dtk::buildSysExFile (dumps);
        if (file.replaceWithData (bytes.data(), bytes.size()))
        {
            currentFile = file;
            dirty = false;
            saveButton.setButtonText ("Save");
            setStatus ("wrote " + file.getFileName() + " ("
                       + juce::String ((int) bytes.size()) + " bytes)");
        }
        else
        {
            setStatus ("could not write " + file.getFullPathName(), true);
        }
    };

    if (! saveAs && currentFile != juce::File()) { write (currentFile); return; }

    chooser = std::make_unique<juce::FileChooser> ("Save pattern as",
                                                   juce::File(), "*.syx");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::warnAboutOverwriting,
        [write] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file != juce::File()) write (file);
        });
}

void MainComponent::importMidi()
{
    if (editor == nullptr) return;

    chooser = std::make_unique<juce::FileChooser> ("Import a MIDI file",
                                                   juce::File(), "*.mid;*.midi");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File() || editor == nullptr) return;

            MidiImportOptions options;   // default: channel n -> track n
            juce::String error;
            const auto result = importMidiFile (file, *editor, options, error);

            if (error.isNotEmpty()) { setStatus (file.getFileName() + ": " + error, true); return; }

            markDirty();
            refreshHeaderFields();
            refreshInspector();
            grid.repaint();
            setStatus (file.getFileName() + " - " + result.summary().replace ("\n", "  "));

            if (result.outOfRange > 0 || result.collisions > 0
                || ! result.unmappedChannels.isEmpty())
                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon, "MIDI import", result.summary());
        });
}

void MainComponent::sendToDevice()
{
    if (editor == nullptr) return;

    if (! midi.outputOpen())
    {
        setStatus ("choose a MIDI output port first", true);
        return;
    }

    juce::AlertWindow::showOkCancelBox (
        juce::MessageBoxIconType::QuestionIcon, "Send to Digitakt",
        "On the Digitakt:\n\n"
        "  SETTINGS > SYSEX DUMP > SYSEX RECEIVE > PATTERN\n"
        "  choose the target slot, then check the screen reads WAITING DATA.\n\n"
        "Back up the target pattern first - this overwrites it.",
        "Send", "Cancel", nullptr,
        juce::ModalCallbackFunction::create ([this] (int result)
        {
            if (result == 0) return;

            if (auto* d = activeDump())
            {
                d->refreshContent();
                juce::String error;
                if (midi.send (*d, error))
                    setStatus ("sent to slot " + juce::String (d->index)
                               + " - the Digitakt should have shown WRITING DATA");
                else
                    setStatus ("send failed: " + error, true);
            }
        }));
}

void MainComponent::toggleReceive()
{
    if (midi.isReceiving())
    {
        midi.endReceive();
        receiveButton.setButtonText ("Receive");

        const auto data = midi.capturedBytes();
        if (data.empty()) { setStatus ("nothing received", true); return; }

        try
        {
            auto parsed = dtk::parseSysExFile (data);
            currentFile = juce::File();
            loadDumps (std::move (parsed), "Received dump");
        }
        catch (const std::exception& e)
        {
            setStatus (juce::String ("received ") + juce::String ((int) data.size())
                       + " bytes but could not parse them: " + e.what(), true);
        }
        return;
    }

    if (! midi.inputOpen()) { setStatus ("choose a MIDI input port first", true); return; }

    withDiscardConfirmation ([this]
    {
        midi.beginReceive();
        receiveButton.setButtonText ("Stop");
        setStatus ("listening - on the Digitakt: SETTINGS > SYSEX DUMP > SYSEX SEND "
                   "> PATTERN > YES, then press Stop");
    });
}

void MainComponent::showProbe()
{
    if (editor == nullptr) return;

    chooser = std::make_unique<juce::FileChooser> (
        "Choose the 'before' dump to compare against", juce::File(), "*.syx");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File() || editor == nullptr) return;

            juce::MemoryBlock mb;
            if (! file.loadFileAsData (mb)) { setStatus ("could not read file", true); return; }
            const auto* p = static_cast<const uint8_t*> (mb.getData());

            try
            {
                auto before = dtk::parseSysExFile (dtk::Bytes (p, p + mb.getSize()));
                const dtk::PatternKit* bpk = nullptr;
                for (const auto& d : before)
                    if (d.patternKit.has_value()) { bpk = &*d.patternKit; break; }
                if (bpk == nullptr) { setStatus ("no pattern in that file", true); return; }

                const auto regions = dtk::patternKitRegions (editor->patternKit());
                const auto diffs = dtk::diffBytes (bpk->toBytes(),
                                                   editor->patternKit().toBytes(),
                                                   regions);
                juce::String text;
                text << juce::String ((int) diffs.size()) << " differing byte(s)\n\n";
                int shown = 0;
                for (const auto& d : diffs)
                {
                    if (++shown > 60) { text << "...\n"; break; }
                    text << juce::String ((int) d.offset).paddedLeft (' ', 7) << "  "
                         << juce::String::toHexString (d.before).paddedLeft ('0', 2)
                         << " -> "
                         << juce::String::toHexString (d.after).paddedLeft ('0', 2)
                         << "   " << d.label << "\n";
                }
                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon,
                    "Probe: " + file.getFileName() + " vs current", text);
            }
            catch (const std::exception& e)
            {
                setStatus (juce::String ("probe failed: ") + e.what(), true);
            }
        });
}

void MainComponent::withDiscardConfirmation (std::function<void()> proceed)
{
    if (! dirty) { proceed(); return; }

    juce::AlertWindow::showOkCancelBox (
        juce::MessageBoxIconType::WarningIcon, "Discard changes?",
        "This pattern has unsaved edits. Replacing it will lose them.",
        "Discard", "Cancel", nullptr,
        juce::ModalCallbackFunction::create ([proceed] (int result)
        {
            if (result != 0) proceed();
        }));
}

//==============================================================================
void MainComponent::assignSampleToSlot (const dtk::api::DirEntry& entry, int slot)
{
    // A pattern dump does not carry the project's sample pool - that lives in
    // the projectSettings dump - so say so rather than pretend the assignment
    // took effect.
    dtk::ProjectSettings* settings = nullptr;
    for (auto& d : dumps)
        if (d.projectSettings.has_value()) { settings = &*d.projectSettings; break; }

    if (settings == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon, "No project settings loaded",
            "Sample pool slots live in the project settings dump, not in a "
            "pattern dump.\n\nTo assign samples to slots, receive a whole "
            "project from the Digitakt (SYSEX DUMP > SYSEX SEND > WHOLE PROJECT) "
            "so the project settings come along too.\n\n"
            "You can still sequence sample slot " + juce::String (slot + 1)
            + " from the Pattern tab with the s and S keys - the slot number is "
              "all a pattern needs.");
        return;
    }

    if (slot < 0 || slot >= (int) settings->samples.size()) return;

    settings->samples[(size_t) slot] = dtk::Sample::fromDriveFile (entry.hash, entry.size);
    for (auto& d : dumps) if (d.projectSettings.has_value()) d.refreshContent();
    markDirty();
    setStatus ("slot " + juce::String (slot + 1) + " now points at "
               + juce::String (entry.name));
}

void MainComponent::statusMessage (const juce::String& text, bool isError)
{
    setStatus (text, isError);
}

//==============================================================================
void MainComponent::dumpReceived (const dtk::ElkDump& d)
{
    setStatus ("received " + juce::String (d.typeName())
               + " for slot " + juce::String (d.index)
               + (d.parseError.empty() ? " - press Stop to load"
                                       : juce::String (" - parse error: ") + d.parseError));
}

void MainComponent::receiveProgress (int bytes)
{
    if (midi.isReceiving())
        setStatus ("receiving... " + juce::String (bytes) + " bytes");
}

void MainComponent::midiError (const juce::String& e) { setStatus (e, true); }

void MainComponent::rawSysExReceived (const dtk::Bytes& msg)
{
    // Each of these filters out anything that is not its own kind of message.
    samplesPanel.handleIncomingSysEx (msg);
    requester.handleSysEx (msg);
}

//==============================================================================
void MainComponent::downloadPattern()
{
    if (requester.isBusy()) { requester.cancel(); return; }
    if (! midi.outputOpen() || ! midi.inputOpen())
    {
        setStatus ("Choose both a MIDI output and a MIDI input port first - "
                   "a download needs to send a request and hear the reply.", true);
        return;
    }

    auto* aw = new juce::AlertWindow (
        "Download from Digitakt",
        "This asks the machine to send data, so there is nothing to press on "
        "the front panel.\n\n"
        "A single pattern is quick. A whole project also brings the sample "
        "pool, which is what you need to assign +Drive samples to slots - but "
        "it sends all 128 patterns and takes a while.",
        juce::MessageBoxIconType::QuestionIcon);

    aw->addTextEditor ("slot", "1", "Pattern (1-128):");
    aw->addButton ("Pattern", 1);
    aw->addButton ("Whole project", 2);
    aw->addButton ("Cancel", 0);

    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, aw] (int result)
        {
            const int slot = aw->getTextEditorContents ("slot").getIntValue();
            std::unique_ptr<juce::AlertWindow> owner (aw);

            if (result == 1)
            {
                if (slot < 1 || slot > 128)
                {
                    setStatus ("Pattern must be between 1 and 128.", true);
                    return;
                }
                withDiscardConfirmation ([this, slot]
                {
                    downloadButton.setButtonText ("Stop");
                    setStatus ("Requesting pattern " + juce::String (slot) + "...");
                    requester.requestPattern (slot - 1);
                });
            }
            else if (result == 2)
            {
                withDiscardConfirmation ([this]
                {
                    downloadButton.setButtonText ("Stop");
                    setStatus ("Requesting the whole project - this sends 128 "
                               "patterns and can take a minute...");
                    requester.requestWholeProject();
                });
            }
        }), false);
}

void MainComponent::dumpsArriving (int count, const juce::String& latest)
{
    setStatus ("Receiving... " + juce::String (count) + " message(s), latest "
               + latest);
}

void MainComponent::dumpsComplete (std::vector<dtk::ElkDump> received)
{
    downloadButton.setButtonText ("Download");
    if (received.empty()) { setStatus ("Nothing was received.", true); return; }

    int patterns = 0;
    bool haveSettings = false;
    for (const auto& d : received)
    {
        if (d.patternKit.has_value()) ++patterns;
        if (d.projectSettings.has_value()) haveSettings = true;
    }

    currentFile = juce::File();
    loadDumps (std::move (received), "Downloaded");

    juce::String note = juce::String (patterns) + " pattern(s) downloaded";
    if (haveSettings)
        note << ", including the sample pool - you can now assign +Drive "
                "samples to slots";
    setStatus (note);
}

void MainComponent::dumpsFailed (const juce::String& why)
{
    downloadButton.setButtonText ("Download");
    setStatus (why.upToFirstOccurrenceOf ("\n", false, false), true);
    if (why.length() > 60)
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon, "Download failed", why);
}
