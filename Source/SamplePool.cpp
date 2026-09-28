#include "SamplePool.h"

#include "SampleImport.h"

const SamplePool::Slot SamplePool::emptySlot {};

bool SamplePool::loadFromFile (int slotNumber, const juce::File& file,
                               juce::String& errorOut)
{
    if (! isUsableSlot (slotNumber))
    {
        errorOut = "slot " + juce::String (slotNumber) + " is not a sample slot "
                   "(0 means off; usable slots are 1 to 127)";
        return false;
    }

    std::vector<float> samples;
    if (! loadAudioAsMono48k (file, samples, errorOut)) return false;

    setSlot (slotNumber, std::move (samples), file.getFileNameWithoutExtension(), file);
    return true;
}

void SamplePool::setSlot (int slotNumber, std::vector<float> samples,
                          const juce::String& name, const juce::File& source)
{
    if (! isUsableSlot (slotNumber) || samples.empty()) return;

    // Build the buffer outside the lock; the audio thread must never wait on
    // a file read or an allocation.
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (1, (int) samples.size());
    buffer->copyFrom (0, 0, samples.data(), (int) samples.size());

    Slot s;
    s.audio  = std::move (buffer);
    s.name   = name;
    s.source = source;

    const juce::SpinLock::ScopedLockType sl (lock);
    slots[(size_t) slotNumber] = std::move (s);
}

void SamplePool::clearSlot (int slotNumber)
{
    if (! isUsableSlot (slotNumber)) return;
    const juce::SpinLock::ScopedLockType sl (lock);
    slots[(size_t) slotNumber] = Slot {};
}

void SamplePool::clearAll()
{
    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& s : slots) s = Slot {};
}

const SamplePool::Slot& SamplePool::slot (int slotNumber) const
{
    if (slotNumber < 0 || slotNumber >= numSlots) return emptySlot;
    return slots[(size_t) slotNumber];
}

int SamplePool::firstFreeSlot() const
{
    for (int i = firstUsableSlot; i <= lastUsableSlot; ++i)
        if (slots[(size_t) i].empty()) return i;
    return -1;
}

int SamplePool::numLoaded() const
{
    int n = 0;
    for (const auto& s : slots) if (! s.empty()) ++n;
    return n;
}

juce::String SamplePool::slotDescription (int slotNumber) const
{
    if (slotNumber == 0) return "off";
    const auto& s = slot (slotNumber);
    if (s.empty()) return "-";
    return s.name + "  (" + juce::String (s.seconds(), 2) + " s)";
}

SamplePool::Audio SamplePool::audioForSlot (int slotNumber) const noexcept
{
    // Slot 0 is the instrument's "off" entry: never any audio.
    if (! isUsableSlot (slotNumber)) return nullptr;

    // A try-lock keeps the audio thread free of priority inversion. Losing the
    // race means one missed trig at the instant a slot is being replaced,
    // which is inaudible; blocking would be a dropout.
    const juce::SpinLock::ScopedTryLockType sl (lock);
    if (! sl.isLocked()) return nullptr;
    return slots[(size_t) slotNumber].audio;
}
