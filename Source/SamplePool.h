/*
    SamplePool.h - the local mirror of the Digitakt's 128 sample slots.

    Filled from local audio files or from samples downloaded off the +Drive.
    This is what makes the app usable with no instrument attached: the pattern
    references slot numbers, and this supplies the audio those numbers mean.

    Slot numbering follows the instrument exactly: a sound's sampleSlot and a
    sample p-lock are a *direct* index into this pool, and slot 0 means "off".
    Usable slots are therefore 1 to 127, and the number shown to the user is
    the number stored in the file - no conversion anywhere.

    (elk-herd settles this: its sample slot labels are the index itself, its
    pool permanently pins index 0 to the "off" entry, and isZeroSampleIndex
    tests i % 128 == 0. Sound pool slots are the ones that are 1-based; sample
    slots are not.)
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

    /** Slot 0 is the instrument's "off" entry and holds no audio. */
    static constexpr int firstUsableSlot = 1;
    static constexpr int lastUsableSlot = numSlots - 1;   // 127

    static bool isUsableSlot (int slot) noexcept
        { return slot >= firstUsableSlot && slot <= lastUsableSlot; }

    //== message thread ======================================================
    bool loadFromFile (int slot, const juce::File&, juce::String& errorOut);
    void setSlot (int slot, std::vector<float> samples, const juce::String& name,
                  const juce::File& source = {});
    void clearSlot (int slot);
    void clearAll();

    const Slot& slot (int slot) const;
    int numLoaded() const;
    juce::String slotDescription (int slot) const;
    /** The lowest usable slot with nothing in it, or -1 when the pool is full. */
    int firstFreeSlot() const;

    //== audio thread ========================================================
    /** Returns the audio for a slot number, or nullptr for slot 0 / empty.
        Safe to call from the audio thread; never allocates or blocks. */
    Audio audioForSlot (int slot) const noexcept;

private:
    std::array<Slot, numSlots> slots;
    mutable juce::SpinLock lock;
    static const Slot emptySlot;
};
