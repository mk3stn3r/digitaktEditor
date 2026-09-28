#include "RemotePanel.h"

namespace
{
    const juce::Colour bgColour  { 0xff1b1d21 };
    const juce::Colour textCol   { 0xffd8dee6 };
    const juce::Colour textDim   { 0xff7d858f };
    const juce::Colour okColour  { 0xff8fbf7f };
    const juce::Colour warnColour{ 0xffe0a33c };
}

RemotePanel::RemotePanel()
{
    auto addToggle = [this] (juce::ToggleButton& b, bool on)
    {
        addAndMakeVisible (b);
        b.setToggleState (on, juce::dontSendNotification);
        b.setColour (juce::ToggleButton::textColourId, textCol);
        b.onClick = [this] { pushConfig(); };
    };

    addToggle (driveToggle, false);
    addToggle (clockToggle, true);
    addToggle (transportToggle, true);
    addToggle (notesToggle, true);

    addAndMakeVisible (localAudioToggle);
    localAudioToggle.setToggleState (true, juce::dontSendNotification);
    localAudioToggle.setColour (juce::ToggleButton::textColourId, textCol);
    localAudioToggle.onClick = [this]
    {
        if (listener != nullptr)
            listener->localAudioChanged (localAudioToggle.getToggleState());
    };

    for (auto* l : { &channelLabel, &pcLabel, &ccLabel, &connectionLabel })
    {
        addAndMakeVisible (*l);
        l->setColour (juce::Label::textColourId, textDim);
        l->setFont (juce::FontOptions (12.0f));
    }
    connectionLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));

    addAndMakeVisible (channelSlider);
    channelSlider.setRange (1, 16, 1);
    channelSlider.setValue (1, juce::dontSendNotification);
    channelSlider.onValueChange = [this] { pushConfig(); };

    addAndMakeVisible (followToggle);
    followToggle.setColour (juce::ToggleButton::textColourId, textCol);

    addAndMakeVisible (pcSlider);
    pcSlider.setRange (1, 128, 1);
    pcSlider.setValue (1, juce::dontSendNotification);

    addAndMakeVisible (pcButton);
    pcButton.onClick = [this]
    {
        if (listener != nullptr) listener->sendProgramChange ((int) pcSlider.getValue());
    };

    addAndMakeVisible (autoSendToggle);
    autoSendToggle.setColour (juce::ToggleButton::textColourId, warnColour);
    autoSendToggle.onClick = [this]
    {
        if (! autoSendToggle.getToggleState())
        {
            if (listener != nullptr) listener->autoSendChanged (false);
            return;
        }

        // Arming this overwrites a slot on the instrument without asking each
        // time, so ask once, clearly, before it is armed.
        autoSendToggle.setToggleState (false, juce::dontSendNotification);
        juce::AlertWindow::showOkCancelBox (
            juce::MessageBoxIconType::WarningIcon, "Auto-send overwrites a slot",
            "With this on, the editor uploads the pattern to the slot shown in "
            "the toolbar two seconds after you stop editing.\n\n"
            "That overwrites whatever is in that slot on the Digitakt, every "
            "time, with no further prompting. Point the Slot field at a scratch "
            "pattern before arming this.\n\n"
            "Note that the instrument stores what it receives; a pattern that is "
            "already playing may not pick up the change until you reselect it. "
            "For live editing, Drive is the better tool.",
            "Arm it", "Cancel", nullptr,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                const bool on = result != 0;
                autoSendToggle.setToggleState (on, juce::dontSendNotification);
                if (listener != nullptr) listener->autoSendChanged (on);
            }));
    };

    addAndMakeVisible (ccChannel);
    ccChannel.setTextValueSuffix (" ch");
    ccChannel.setRange (1, 16, 1);
    ccChannel.setValue (1, juce::dontSendNotification);
    addAndMakeVisible (ccNumber);
    ccNumber.setTextValueSuffix (" no");
    ccNumber.setRange (0, 127, 1);
    ccNumber.setValue (74, juce::dontSendNotification);
    addAndMakeVisible (ccValue);
    ccValue.setTextValueSuffix (" val");
    ccValue.setRange (0, 127, 1);
    ccValue.setValue (64, juce::dontSendNotification);

    addAndMakeVisible (ccButton);
    ccButton.onClick = [this]
    {
        if (listener != nullptr)
            listener->sendControlChange ((int) ccChannel.getValue(),
                                         (int) ccNumber.getValue(),
                                         (int) ccValue.getValue());
    };

    addAndMakeVisible (help);
    help.setColour (juce::Label::textColourId, textDim);
    help.setFont (juce::FontOptions (11.5f));
    help.setJustificationType (juce::Justification::topLeft);
    help.setText (
        "On the Digitakt: SETTINGS > MIDI CONFIG > SYNC, turn on CLOCK RECEIVE and "
        "TRANSPORT RECEIVE (and PROG CH RECEIVE for pattern changes). In "
        "MIDI CONFIG > CHANNELS set TRACK 1-8 to consecutive channels starting at "
        "the one above.\n\n"
        "The Digitakt will not run from an external transport without clock, so "
        "leaving clock off means start and stop do nothing.\n\n"
        "Program changes are queued by the instrument and take effect when the "
        "current pattern ends, so send them a bar early.\n\n"
        "CC numbers are not the same across Elektron models and this build ships "
        "no parameter map, so the CC sender is deliberately raw: check your "
        "model's manual for the number you want.",
        juce::dontSendNotification);
    setConnected (false, {});
}

void RemotePanel::pushConfig()
{
    SequencerEngine::RemoteConfig c;
    c.enabled           = driveToggle.getToggleState();
    c.sendClock         = clockToggle.getToggleState();
    c.sendTransport     = transportToggle.getToggleState();
    c.sendNotes         = notesToggle.getToggleState();
    c.firstTrackChannel = (int) channelSlider.getValue();

    clockToggle    .setEnabled (c.enabled);
    transportToggle.setEnabled (c.enabled);
    notesToggle    .setEnabled (c.enabled);
    channelSlider  .setEnabled (c.enabled);

    if (listener != nullptr) listener->remoteConfigChanged (c);
}

void RemotePanel::setConnected (bool midiOutOpen, const juce::String& portName)
{
    connectionLabel.setText (
        midiOutOpen ? "Connected to " + portName
                    : "No MIDI output selected - choose a port in the toolbar",
        juce::dontSendNotification);
    connectionLabel.setColour (juce::Label::textColourId,
                               midiOutOpen ? okColour : warnColour);

    driveToggle   .setEnabled (midiOutOpen);
    pcButton      .setEnabled (midiOutOpen);
    ccButton      .setEnabled (midiOutOpen);
    autoSendToggle.setEnabled (midiOutOpen);
    followToggle  .setEnabled (midiOutOpen);
    if (! midiOutOpen)
    {
        driveToggle.setToggleState (false, juce::dontSendNotification);
        pushConfig();
    }
}

void RemotePanel::paint (juce::Graphics& g)
{
    g.fillAll (bgColour);

    g.setColour (juce::Colour (0xff2f333a));
    const int y = 44 + 5 * 24 + 30 + 12;
    g.drawHorizontalLine (y, 8.0f, (float) getWidth() - 8.0f);
}

void RemotePanel::resized()
{
    auto area = getLocalBounds().reduced (12);

    connectionLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (8);

    driveToggle     .setBounds (area.removeFromTop (24));
    clockToggle     .setBounds (area.removeFromTop (24).withTrimmedLeft (22));
    transportToggle .setBounds (area.removeFromTop (24).withTrimmedLeft (22));
    notesToggle     .setBounds (area.removeFromTop (24).withTrimmedLeft (22));

    auto chRow = area.removeFromTop (28).withTrimmedLeft (22);
    channelLabel .setBounds (chRow.removeFromLeft (110));
    channelSlider.setBounds (chRow.removeFromLeft (130).reduced (2));

    localAudioToggle.setBounds (area.removeFromTop (24));
    area.removeFromTop (16);

    followToggle.setBounds (area.removeFromTop (24));
    auto pcRow = area.removeFromTop (28);
    pcLabel .setBounds (pcRow.removeFromLeft (60));
    pcSlider.setBounds (pcRow.removeFromLeft (150).reduced (2));
    pcRow.removeFromLeft (8);
    pcButton.setBounds (pcRow.removeFromLeft (180).reduced (2));
    area.removeFromTop (10);

    autoSendToggle.setBounds (area.removeFromTop (24));
    area.removeFromTop (16);

    // Wide enough that the inc/dec buttons are not squeezed into an ellipsis.
    auto ccRow = area.removeFromTop (28);
    ccLabel  .setBounds (ccRow.removeFromLeft (30));
    ccChannel.setBounds (ccRow.removeFromLeft (150).reduced (2));
    ccNumber .setBounds (ccRow.removeFromLeft (150).reduced (2));
    ccValue  .setBounds (ccRow.removeFromLeft (150).reduced (2));
    ccRow.removeFromLeft (10);
    ccButton .setBounds (ccRow.removeFromLeft (110).reduced (2));

    area.removeFromTop (14);
    help.setBounds (area);
}
