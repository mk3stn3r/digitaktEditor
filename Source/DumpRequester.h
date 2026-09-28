/*
    DumpRequester.h - asks the Digitakt to send dumps, rather than waiting for
    the user to trigger them from the front panel.

    The dump protocol has request messages alongside the responses: same
    envelope, empty payload. Sending 0x60 with a slot number makes the machine
    reply with that pattern; 0x6F asks for the whole project.

    A whole-project reply is a series of pattern dumps, then zero or more sound
    dumps, then the project settings - and nothing marks the end except the
    project settings arriving last. So this finishes on that message, and falls
    back to an idle timeout if it never comes.
*/

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "DigitaktCore.h"

class DumpRequester : private juce::Timer
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void dumpsArriving (int count, const juce::String& latest) = 0;
        virtual void dumpsComplete (std::vector<dtk::ElkDump>) = 0;
        virtual void dumpsFailed (const juce::String& why) = 0;
    };

    enum class What { pattern, projectSettings, wholeProject };

    void setOutput (juce::MidiOutput* o) { output = o; }
    void setListener (Listener* l) { listener = l; }

    bool isBusy() const noexcept { return busy; }
    What requesting() const noexcept { return what; }

    /** `slot` is 0-based; the Digitakt has 128 pattern slots. */
    void requestPattern (int slot);
    void requestProjectSettings();
    void requestWholeProject();

    void handleSysEx (const dtk::Bytes& sysex);
    void cancel();

    /** How long to wait for the first reply before giving up. */
    static constexpr int firstReplyTimeoutMs = 6000;
    /** How long a quiet gap ends a multi-message whole-project transfer. */
    static constexpr int idleTimeoutMs = 4000;

private:
    void start (What, int slot, int messageType);
    void timerCallback() override;
    void finish();
    void fail (const juce::String& why);

    juce::MidiOutput* output = nullptr;
    Listener* listener = nullptr;

    bool busy = false;
    bool sawAnything = false;
    What what { What::pattern };
    int requestedSlot = 0;
    std::vector<dtk::ElkDump> collected;
};
