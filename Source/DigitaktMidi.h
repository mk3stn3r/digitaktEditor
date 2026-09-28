/*
    DigitaktMidi.h - device I/O and MIDI file import.

    This is the JUCE half of the app. Using juce::MidiOutput directly replaces
    shelling out to amidi and works the same on Linux, macOS and Windows.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "DigitaktCore.h"

//==============================================================================
/** Sends and receives Elektron dumps over a MIDI port. */
class DigitaktMidi : private juce::MidiInputCallback,
                     private juce::AsyncUpdater
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        /** Called on the message thread when a complete dump has arrived. */
        virtual void dumpReceived (const dtk::ElkDump&) = 0;
        virtual void receiveProgress (int bytesSoFar) = 0;
        virtual void midiError (const juce::String&) = 0;
        /** Every SysEx message, dump or not, so the API client can see replies. */
        virtual void rawSysExReceived (const dtk::Bytes&) {}
    };

    DigitaktMidi() = default;
    ~DigitaktMidi() override { closeInput(); closeOutput(); }

    void setListener (Listener* l) { listener = l; }

    //== ports =================================================================
    static juce::Array<juce::MidiDeviceInfo> outputDevices()
        { return juce::MidiOutput::getAvailableDevices(); }
    static juce::Array<juce::MidiDeviceInfo> inputDevices()
        { return juce::MidiInput::getAvailableDevices(); }

    /** Guesses which port is the Digitakt, so the common case needs no setup. */
    static int indexOfDigitakt (const juce::Array<juce::MidiDeviceInfo>& devices)
    {
        for (int i = 0; i < devices.size(); ++i)
            if (devices[i].name.containsIgnoreCase ("digitakt")
             || devices[i].name.containsIgnoreCase ("elektron"))
                return i;
        return -1;
    }

    bool openOutput (const juce::String& identifier)
    {
        output = juce::MidiOutput::openDevice (identifier);
        return output != nullptr;
    }

    bool openInput (const juce::String& identifier)
    {
        input = juce::MidiInput::openDevice (identifier, this);
        if (input != nullptr) { input->start(); return true; }
        return false;
    }

    void closeOutput() { output.reset(); }
    void closeInput()  { if (input != nullptr) input->stop(); input.reset(); }

    bool outputOpen() const noexcept { return output != nullptr; }
    /** For the API client, which sends its own messages on the same port. */
    juce::MidiOutput* rawOutput() const noexcept { return output.get(); }
    bool inputOpen()  const noexcept { return input  != nullptr; }

    //== sending ==============================================================
    /** Sends one dump. The message must already be a framed F0..F7 blob;
        juce::MidiMessage::createSysExMessage adds the framing itself, so we
        hand it only the body. */
    bool send (const dtk::ElkDump& dump, juce::String& errorOut)
    {
        if (output == nullptr) { errorOut = "no MIDI output is open"; return false; }

        const auto wire = dump.toBytes();
        if (wire.size() < 2) { errorOut = "empty dump"; return false; }

        const auto msg = juce::MidiMessage::createSysExMessage (
            wire.data() + 1, (int) wire.size() - 2);
        output->sendMessageNow (msg);
        return true;
    }

    //== receiving ============================================================
    void beginReceive()
    {
        const juce::ScopedLock sl (bufferLock);
        incoming.clear();
        receiving = true;
    }

    void endReceive()
    {
        const juce::ScopedLock sl (bufferLock);
        receiving = false;
    }

    bool isReceiving() const noexcept { return receiving; }

    /** Everything captured since beginReceive, as raw bytes. */
    dtk::Bytes capturedBytes() const
    {
        const juce::ScopedLock sl (bufferLock);
        return incoming;
    }

private:
    void handleIncomingMidiMessage (juce::MidiInput*,
                                    const juce::MidiMessage& m) override
    {
        if (! m.isSysEx()) return;
        appendSysEx (m.getSysExData(), m.getSysExDataSize());
    }

    // Note: no handlePartialSysexMessage override. JUCE reassembles fragmented
    // SysEx into complete messages before calling handleIncomingMidiMessage, so
    // acting on partials as well would double-count the same bytes.

    void appendSysEx (const juce::uint8* body, int size)
    {
        // API replies must reach the client whether or not a dump capture is
        // running, so hand them over before the `receiving` gate.
        {
            dtk::Bytes whole;
            whole.push_back (0xF0);
            whole.insert (whole.end(), body, body + size);
            whole.push_back (0xF7);
            const juce::ScopedLock sl (bufferLock);
            apiInbox.push_back (std::move (whole));
        }
        triggerAsyncUpdate();

        {
            const juce::ScopedLock sl (bufferLock);
            if (! receiving) return;
            incoming.push_back (0xF0);
            incoming.insert (incoming.end(), body, body + size);
            incoming.push_back (0xF7);
        }
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        if (listener == nullptr) return;

        std::vector<dtk::Bytes> inbox;
        {
            const juce::ScopedLock sl (bufferLock);
            inbox.swap (apiInbox);
        }
        for (const auto& m : inbox) listener->rawSysExReceived (m);

        if (! receiving) return;

        dtk::Bytes snapshot;
        {
            const juce::ScopedLock sl (bufferLock);
            snapshot = incoming;
        }
        listener->receiveProgress ((int) snapshot.size());

        try
        {
            const auto dumps = dtk::parseSysExFile (snapshot);
            for (size_t i = reported; i < dumps.size(); ++i)
                listener->dumpReceived (dumps[i]);
            reported = dumps.size();
        }
        catch (const std::exception&)
        {
            // Still mid-transfer, or not a dump. Wait for more bytes; a real
            // failure surfaces when the user stops the capture.
        }
    }

    std::unique_ptr<juce::MidiOutput> output;
    std::unique_ptr<juce::MidiInput>  input;
    Listener* listener = nullptr;

    juce::CriticalSection bufferLock;
    dtk::Bytes incoming;
    std::vector<dtk::Bytes> apiInbox;
    std::atomic<bool> receiving { false };
    size_t reported = 0;
};

//==============================================================================
/** Quantises a Standard MIDI File onto a pattern's step grid. */
struct MidiImportResult
{
    int placed = 0;
    int outOfRange = 0;
    int collisions = 0;
    double bpm = 120.0;
    int patternLength = 16;
    juce::Array<int> unmappedChannels;
    juce::Array<int> touchedTracks;

    juce::String summary() const;
};

struct MidiImportOptions
{
    std::map<int, int> channelMap;   // 0-based MIDI channel -> 0-based track
    int  stepsPerBeat  = 4;          // 4 = sixteenths
    int  barOffset     = 0;
    bool setTempo      = true;
    bool writeNotes    = true;
    bool writeVelocity = true;
};

MidiImportResult importMidiFile (const juce::File& file,
                                 dtk::PatternEditor& editor,
                                 const MidiImportOptions& options,
                                 juce::String& errorOut);
