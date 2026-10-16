/*
    SequencerEngine.h - plays a Digitakt pattern through the computer's audio
    output, so the app is a working sample sequencer with nothing plugged in.

    It follows the instrument's behaviour rather than inventing its own:
      * eight audio tracks, each monophonic - a new trig steals that track's
        voice, exactly as the Digitakt does
      * a step's sample p-lock overrides which pool slot fires
      * NOTE transposes the sample, with 60 as original pitch
      * VEL scales gain, LEN gates the voice in steps
      * the step grid is sixteenths, so a step is 60 / bpm / 4 seconds

    Threading: the audio callback never touches the PatternKit. The message
    thread renders a small plain-data snapshot and publishes it into a ring of
    buffers with an atomic index, so the audio thread always reads a complete,
    stable copy without locking.
*/

#pragma once

#include <atomic>

#include <juce_audio_devices/juce_audio_devices.h>

#include "DigitaktCore.h"
#include "PluginRack.h"
#include "SamplePool.h"

/** Where the engine's generated MIDI goes.

    An interface rather than a juce::MidiOutput directly, so remote control can
    be captured and asserted on in tests with no device attached. */
struct MidiSink
{
    virtual ~MidiSink() = default;
    /** Called from the audio thread; timestamps are sample offsets. */
    virtual void sendBlock (const juce::MidiBuffer&, double sampleRate) = 0;
};

class SequencerEngine : public juce::AudioIODeviceCallback
{
public:
    static constexpr int numAudioTracks = 8;
    static constexpr int numMidiTracks = 8;      // M1-M8, hosted as plugins
    static constexpr int numTracks = numAudioTracks + numMidiTracks;
    static constexpr int maxSteps = 64;
    static constexpr int numSnapshots = 4;

    SequencerEngine (SamplePool& p, PluginRack& r) : pool (p), rack (r) {}
    ~SequencerEngine() override = default;

    /** Rebuilds the snapshot the audio thread plays from. Message thread only. */
    void updateFromPattern (const dtk::PatternKit&);

    void start();
    void stop();
    bool isPlaying() const noexcept { return playing.load(); }

    /** The step currently sounding, or -1 when stopped. For the playhead. */
    int currentStep() const noexcept { return playing.load() ? stepIndex.load() : -1; }

    void setLooping (bool b) noexcept { looping.store (b); }
    bool isLooping() const noexcept { return looping.load(); }

    void setGain (float g) noexcept { gain.store (juce::jlimit (0.0f, 2.0f, g)); }
    float getGain() const noexcept { return gain.load(); }

    /** Fires a pool slot immediately, for auditioning. */
    void triggerAudition (int slot, int note = 60, float velocity = 1.0f);

    //== remote control =======================================================
    /** Drives a connected Digitakt from this transport.

        The machine needs MIDI clock to run from an external source, so clock
        and transport travel together by default. Notes go out on each track's
        own channel, which is what makes an edit in the grid audible on the
        instrument on the next pass - no upload, no slot overwritten. */
    struct RemoteConfig
    {
        bool enabled { false };
        bool sendClock { true };        // 24 ppqn
        bool sendTransport { true };    // start / stop
        bool sendNotes { true };        // trigs on the per-track channels
        int  firstTrackChannel { 1 };   // track 1 -> this channel (1-16)
    };

    void setMidiSink (MidiSink*);
    void setRemoteConfig (RemoteConfig);
    RemoteConfig remoteConfig() const;

    /** Turn the sample voices off to hear only the instrument. */
    void setLocalAudioEnabled (bool b) noexcept { localAudio.store (b); }
    bool localAudioEnabled() const noexcept { return localAudio.load(); }

    /** Plugin output is controlled separately from the sample voices, so the
        MIDI-track instruments can be sent to the Digitakt on their own for
        resampling while the sample tracks stay silent. */
    void setPluginAudioEnabled (bool b) noexcept { pluginAudio.store (b); }
    bool pluginAudioEnabled() const noexcept { return pluginAudio.load(); }

    //== playing the instruments from the Digitakt ============================
    /** Feeds live MIDI from the instrument to the hosted plugins.

        Call from the MIDI input thread. Messages are queued with their
        timestamps and consumed by the audio thread at the right sample
        offsets, so a part played on the Digitakt's MIDI tracks arrives in
        time rather than quantised to the block. */
    void handleExternalMidi (const juce::MidiMessage&);

    /** `baseChannel` is the channel the Digitakt's M1 sends on; M2 is the
        next one up, and so on through M8. */
    void setExternalMidiInput (bool enabled, int baseChannel);
    bool externalMidiEnabled() const noexcept { return externalMidi.load(); }
    int  externalMidiBaseChannel() const noexcept { return externalBase.load(); }

    /** Test hook: how many external messages have been routed to a slot. */
    int externalMessagesRouted (int slot) const;

    void audioDeviceIOCallbackWithContext (const float* const*, int,
                                           float* const* outputChannelData,
                                           int numOutputChannels, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    struct StepData
    {
        bool trig { false };
        int  note { 60 };
        int  velocity { 100 };
        int  length { 0 };        // in steps; 0 = let the sample run out
        int  slot { 0 };          // pool slot number, 0 = off
    };

    struct Snapshot
    {
        double tempo { 120.0 };
        int    length { 16 };
        StepData steps[numTracks][maxSteps];
    };

    struct Voice
    {
        SamplePool::Audio audio;
        double position { 0.0 };
        double increment { 1.0 };
        float  gain { 1.0f };
        double gateSamplesLeft { -1.0 };   // negative = no gate
        bool   active { false };

        void reset() { audio.reset(); active = false; }
    };

    void advanceStep (int sampleOffset);
    void emitPluginNoteOn (int midiTrack, const StepData&, double samplesPerStep,
                           int offset);
    void emitPluginNoteOff (int midiTrack, int offset);
    void fireStep (int step, int sampleOffset);
    void emitNoteOn (int track, const StepData&, double samplesPerStep, int offset);
    void emitNoteOff (int track, int offset);
    void allNotesOff (int offset);
    RemoteConfig currentRemote() const;
    void startVoice (Voice&, const SamplePool::Audio&, int note, float velocity,
                     double gateSamples);

    SamplePool& pool;
    PluginRack& rack;

    Snapshot snapshots[numSnapshots];
    std::atomic<int> liveSnapshot { 0 };
    int writeSnapshot = 0;

    std::atomic<bool> playing { false };
    std::atomic<bool> looping { true };
    std::atomic<int>  stepIndex { 0 };
    std::atomic<float> gain { 0.8f };

    double sampleRate { 48000.0 };
    double samplesUntilNextStep { 0.0 };
    Voice voices[numAudioTracks];
    Voice auditionVoice;

    // remote control
    MidiSink* sink = nullptr;
    RemoteConfig remote;
    mutable juce::SpinLock remoteLock;
    juce::MidiBuffer midiOut;
    double samplesUntilNextClock { 0.0 };
    int    activeMidiNote[numAudioTracks] { -1, -1, -1, -1, -1, -1, -1, -1 };
    double midiGateLeft[numAudioTracks] { -1, -1, -1, -1, -1, -1, -1, -1 };

    // Notes currently held by the hosted instruments on the MIDI tracks.
    int    pluginNote[numMidiTracks] { -1, -1, -1, -1, -1, -1, -1, -1 };
    double pluginGateLeft[numMidiTracks] { -1, -1, -1, -1, -1, -1, -1, -1 };
    std::atomic<bool> localAudio { true };
    std::atomic<bool> pluginAudio { true };

    // live MIDI in from the instrument
    juce::MidiMessageCollector externalCollector;
    std::atomic<bool> externalMidi { false };
    std::atomic<int>  externalBase { 9 };
    std::atomic<int>  externalRouted[numMidiTracks] {};
    std::atomic<bool> pendingStart { false };
    std::atomic<bool> pendingStop { false };

    std::atomic<int>   auditionSlot { -1 };
    std::atomic<int>   auditionNote { 60 };
    std::atomic<float> auditionVelocity { 1.0f };
};
