/*
    PluginRack.h - one hosted instrument plugin per Digitakt MIDI track.

    The Digitakt's eight MIDI tracks send notes to external gear and make no
    sound of their own. Offline they were simply silent. A rack slot gives each
    of them a VST3 or LV2 instrument, so M1-M8 become playable parts of the
    same pattern.

    Threading: plugin instances are created and destroyed on the message
    thread. The audio thread takes a try-lock; if a load is in progress it
    skips plugin rendering for that block rather than waiting, so loading a
    plugin costs a few milliseconds of plugin silence instead of a dropout.
*/

#pragma once

#include <array>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

class PluginRack
{
public:
    static constexpr int numSlots = 8;     // one per Digitakt MIDI track

    PluginRack() = default;
    ~PluginRack() { releaseResources(); }

    //== message thread =======================================================
    void prepare (double sampleRate, int blockSize);
    void releaseResources();

    /** Instantiates a plugin into a slot. Returns false and fills `errorOut`. */
    bool loadPlugin (int slot, const juce::PluginDescription&,
                     juce::AudioPluginFormatManager&, juce::String& errorOut);

    /** Installs an already-created instance. Used by tests, and by any caller
        that has instantiated a plugin itself. */
    void adoptForTesting (int slot, std::unique_ptr<juce::AudioPluginInstance>,
                          const juce::String& name);

    void clearSlot (int slot);
    void clearAll();

    /** Message thread only - for opening an editor window. */
    juce::AudioPluginInstance* instance (int slot);

    juce::String slotName (int slot) const;
    bool slotLoaded (int slot) const;
    int  numLoaded() const;

    /** Slot gain in 0..1, applied when mixing the plugin into the output. */
    void setSlotGain (int slot, float gain);
    float slotGain (int slot) const;

    //== audio thread =========================================================
    /** Clears the per-slot MIDI buffers at the top of a callback. */
    void beginBlock();

    void addNoteOn (int slot, int note, int velocity, int sampleOffset);
    void addNoteOff (int slot, int note, int sampleOffset);
    void addAllNotesOff (int slot, int sampleOffset);

    /** Forwards any channel message to a slot, rewritten to channel 1.

        This is what lets the Digitakt's own MIDI tracks drive a hosted
        instrument: notes, CCs, pitch bend and aftertouch all pass through. */
    void addMessage (int slot, const juce::MidiMessage&, int sampleOffset);

    /** Runs every loaded plugin for `numSamples` and mixes it into `out`. */
    void renderAndMix (float* const* out, int numChannels, int numSamples,
                       float masterGain);

    /** Test hook: the MIDI queued for a slot in the current block. */
    const juce::MidiBuffer& pendingMidi (int slot) const;

    /** True when a plugin has reported a change (a preset switch, usually)
        that needs the host to prepare it again. Clears the flag. */
    bool takeNeedsReprepare();

    /** Re-runs prepareToPlay on any slot whose layout has moved. */
    void reprepareChangedSlots();

private:
    struct Entry
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> scratch;
        juce::String name;
        std::atomic<float> gain { 1.0f };
        /** Channels processBlock must be handed: max(total in, total out).
            A synth is usually 2, but multi-out and sidechain buses are not
            rare, and handing processBlock too few channels is a buffer
            overrun, not a quiet mix-down. */
        int channels { 2 };
        std::unique_ptr<juce::AudioProcessorListener> watcher;
    };

    std::array<Entry, numSlots> slots;
    mutable juce::CriticalSection lock;
    double rate { 48000.0 };
    int    block { 512 };
    bool   prepared { false };

    static bool valid (int slot) noexcept { return slot >= 0 && slot < numSlots; }

    void attachWatcher (int slot);
    std::atomic<bool> needsReprepare { false };

    friend struct PluginWatcher;

    JUCE_DECLARE_NON_COPYABLE (PluginRack)
};
