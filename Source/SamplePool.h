/*
    SamplePool.h - the local mirror of the Digitakt's 128 sample slots.

    Filled from local audio files or from samples downloaded off the +Drive.
    This is what makes the app usable with no instrument attached: the pattern
    references slot numbers, and this supplies the audio those numbers mean.

    Slot numbering follows the instrument: a sound's sampleSlot and a sample
    p-lock are 1-based, with 0 meaning "no sample". `slotIndex` converts.
*/

#pragma once

#include <array>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

class SamplePool
{
public:
    static constexpr int numSlots = 128;
    static constexpr double rate = 48000.0;

    using Audio = std::shared_ptr<const juce::AudioBuffer<float>>;

    struct Slot
    {
        Audio audio;
        juce::String name;
        juce::File source;

        bool empty() const noexcept { return audio == nullptr; }
        double seconds() const
            { return audio ? audio->getNumSamples() / rate : 0.0; }
    };

    /** Converts an instrument slot number (1-based, 0 = none) to an index. */
    static int slotIndex (int instrumentSlot) noexcept
        { return instrumentSlot - 1; }
    static int instrumentSlot (int index) noexcept { return index + 1; }

    //== message thread ======================================================
    bool loadFromFile (int index, const juce::File&, juce::String& errorOut);
    void setSlot (int index, std::vector<float> samples, const juce::String& name,
                  const juce::File& source = {});
    void clearSlot (int index);
    void clearAll();

    const Slot& slot (int index) const;
    int numLoaded() const;
    juce::String slotDescription (int index) const;

    //== audio thread ========================================================
    /** Returns the audio for an instrument slot number, or nullptr.
        Safe to call from the audio thread; never allocates or blocks. */
    Audio audioForInstrumentSlot (int instrumentSlot) const noexcept;

private:
    std::array<Slot, numSlots> slots;
    mutable juce::SpinLock lock;
    static const Slot emptySlot;
};
