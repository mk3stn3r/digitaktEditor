/*
    DigitaktMidi.h - device I/O and MIDI file import.

    This is the JUCE half of the app. Using juce::MidiOutput directly replaces
    shelling out to amidi and works the same on Linux, macOS and Windows.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "DigitaktCore.h"
#include "RawMidiOut.h"

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

        /** Live channel messages, delivered on the MIDI thread rather than
            deferred, so their timestamps stay usable for scheduling. */
        virtual void liveMidiReceived (const juce::MidiMessage&) {}
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
    /** Sends one dump, preferring the raw MIDI port.

        A pattern dump is ~31 KB in one SysEx message. juce::MidiOutput goes
        through the ALSA *sequencer*, whose sendMessageNow returns void - the
        ALSA error code is discarded, so a failed send cannot be distinguished
        from a successful one. Raw MIDI is the transport `amidi` uses, blocks
        until the bytes are gone, and reports what happened.

        `usedPortOut` names the transport actually used, for the status line. */
    bool send (const dtk::ElkDump& dump, juce::String& errorOut,
               juce::String* usedPortOut = nullptr)
    {
        const auto wire = dump.toBytes();
        if (wire.size() < 2) { errorOut = "empty dump"; return false; }

        const juce::ScopedLock sl (rawLock);

        if (rawOut.isOpen())
        {
            if (rawOut.write (wire.data(), wire.size(), errorOut))
            {
                if (usedPortOut != nullptr)
                    *usedPortOut = "raw MIDI " + juce::String (rawOut.openedId());
                return true;
            }
            // Fall through to the sequencer rather than giving up outright.
            errorOut = "raw MIDI failed (" + errorOut + "), retrying on the "
                       "sequencer port";
        }

        if (output == nullptr)
        {
            if (errorOut.isEmpty()) errorOut = "no MIDI output is open";
            return false;
        }

        const auto msg = juce::MidiMessage::createSysExMessage (
            wire.data() + 1, (int) wire.size() - 2);
        output->sendMessageNow (msg);

        if (usedPortOut != nullptr)
            *usedPortOut = "the sequencer port (delivery unconfirmed)";

        // Deliberately not a claim of success: this path cannot know.
        return true;
    }

    //== raw MIDI (bulk transfers) ============================================
    bool openRawPort (const std::string& id, juce::String& errorOut)
        { const juce::ScopedLock sl (rawLock); return rawOut.open (id, errorOut); }
    void closeRawPort() { const juce::ScopedLock sl (rawLock); rawOut.close(); }
    bool rawPortOpen() const { const juce::ScopedLock sl (rawLock); return rawOut.isOpen(); }
    juce::String rawPortId() const
        { const juce::ScopedLock sl (rawLock); return rawOut.openedId(); }

    /** Opens the rawmidi port that looks like a Digitakt, if there is one. */
    bool openRawPortAutomatically (juce::String& errorOut)
    {
        RawMidiOut::Port p;
        if (! RawMidiOut::findDigitakt (p))
        {
            errorOut = "no raw MIDI port looks like a Digitakt";
            return false;
        }
        const juce::ScopedLock sl (rawLock);
        return rawOut.open (p.id, errorOut);
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
        if (! m.isSysEx())
        {
            // Passed straight through on this thread: deferring to the message
            // thread would cost the timestamp that makes live playing tight.
            if (listener != nullptr && m.getChannel() > 0)
                listener->liveMidiReceived (m);
            return;
        }
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
    RawMidiOut rawOut;
    // rawOut is opened on a background thread and used from the
    // message thread, so every touch of it is guarded.
    mutable juce::CriticalSection rawLock;
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
