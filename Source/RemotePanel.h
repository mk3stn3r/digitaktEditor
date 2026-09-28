/*
    RemotePanel.h - driving a connected Digitakt from the editor.

    Three separate things, deliberately kept apart because they carry very
    different risk:

      Drive      clock, transport and per-track notes. The editor becomes the
                 master clock and plays the instrument's own samples live. An
                 edit in the grid is audible on the next pass. Nothing on the
                 machine is written to.

      Follow     a program change when the pattern slot changes, so the
                 instrument switches pattern with the editor. The Digitakt
                 queues the change until the current pattern ends.

      Auto-send  uploads the edited pattern to a slot after you stop typing.
                 This overwrites that slot, so it is off by default and asks
                 once before it is armed.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SequencerEngine.h"

class RemotePanel : public juce::Component
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void remoteConfigChanged (SequencerEngine::RemoteConfig) = 0;
        virtual void autoSendChanged (bool enabled) = 0;
        virtual void sendProgramChange (int patternSlot) = 0;
        virtual void sendControlChange (int channel, int cc, int value) = 0;
        virtual void localAudioChanged (bool enabled) = 0;
    };

    RemotePanel();

    void setListener (Listener* l) { listener = l; }
    void setConnected (bool midiOutOpen, const juce::String& portName);
    bool autoSendEnabled() const { return autoSendToggle.getToggleState(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void pushConfig();

    Listener* listener = nullptr;

    juce::Label  connectionLabel;

    juce::ToggleButton driveToggle     { "Drive the Digitakt from this transport" };
    juce::ToggleButton clockToggle     { "Send MIDI clock (24 ppqn)" };
    juce::ToggleButton transportToggle { "Send start / stop" };
    juce::ToggleButton notesToggle     { "Send trigs as notes on the track channels" };
    juce::ToggleButton localAudioToggle{ "Also play the local sample pool" };

    juce::Label    channelLabel { {}, "Track 1 channel" };
    juce::Slider   channelSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };

    juce::ToggleButton followToggle { "Send a program change when the slot changes" };
    juce::Label    pcLabel { {}, "Pattern" };
    juce::Slider   pcSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::TextButton pcButton { "Send program change" };

    juce::ToggleButton autoSendToggle { "Auto-send the edited pattern to its slot" };

    juce::Label    ccLabel { {}, "CC" };
    juce::Slider   ccChannel { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Slider   ccNumber  { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Slider   ccValue   { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::TextButton ccButton { "Send CC" };

    juce::Label help;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemotePanel)
};
