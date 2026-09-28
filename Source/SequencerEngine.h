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
#include "SamplePool.h"

class SequencerEngine : public juce::AudioIODeviceCallback
{
public:
    static constexpr int numAudioTracks = 8;
    static constexpr int maxSteps = 64;
    static constexpr int numSnapshots = 4;

    explicit SequencerEngine (SamplePool& p) : pool (p) {}
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

    /** Fires one track's sample immediately, for auditioning a slot. */
    void triggerAudition (int instrumentSlot, int note = 60, float velocity = 1.0f);

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
        int  slot { 0 };          // instrument slot number, 0 = none
    };

    struct Snapshot
    {
        double tempo { 120.0 };
        int    length { 16 };
        StepData steps[numAudioTracks][maxSteps];
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

    void advanceStep();
    void fireStep (int step);
    void startVoice (Voice&, const SamplePool::Audio&, int note, float velocity,
                     double gateSamples);

    SamplePool& pool;

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

    std::atomic<int>   auditionSlot { -1 };
    std::atomic<int>   auditionNote { 60 };
    std::atomic<float> auditionVelocity { 1.0f };
};
