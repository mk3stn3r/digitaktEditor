/*
    RawMidiOut.h - bulk SysEx output over ALSA rawmidi.

    The ALSA *sequencer* (what juce::MidiOutput uses on Linux) is built for
    small, timestamped, real-time messages. A pattern dump is ~31 KB in a
    single SysEx, and JUCE's sequencer path discards the ALSA return code, so
    a failed send is reported to the user as a success - which looks exactly
    like the instrument sitting at WAITING DATA forever.

    This is the transport `amidi` uses: open the hardware port directly and
    write the bytes, blocking until they are gone. Real-time traffic (clock,
    notes) stays on the sequencer, where timestamps matter.
*/

#pragma once

#include <string>
#include <vector>

#include <juce_core/juce_core.h>

class RawMidiOut
{
public:
    struct Port
    {
        std::string id;       // "hw:2,0,0"
        std::string name;     // "Digitakt MIDI 1"
        std::string card;     // "Elektron Digitakt"

        std::string describe() const { return name + "  (" + id + ")"; }
    };

    /** Every rawmidi output port on the system. Empty on non-Linux builds. */
    static std::vector<Port> list();

    /** The port most likely to be the Digitakt, or nullopt. */
    static bool findDigitakt (Port& out);

    RawMidiOut() = default;
    ~RawMidiOut() { close(); }

    bool open (const std::string& id, juce::String& errorOut);
    void close();
    bool isOpen() const noexcept { return handle != nullptr; }
    const std::string& openedId() const noexcept { return currentId; }

    /** Writes every byte, blocking until the driver has taken them.
        Returns false and fills `errorOut` on any error or short write. */
    bool write (const uint8_t* data, size_t size, juce::String& errorOut);

    /** True when this build can talk to rawmidi at all. */
    static bool available();

private:
    void* handle = nullptr;      // snd_rawmidi_t*, kept opaque in the header
    std::string currentId;
};
