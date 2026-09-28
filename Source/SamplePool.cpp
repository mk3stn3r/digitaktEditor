#include "SamplePool.h"

#include "SampleImport.h"

const SamplePool::Slot SamplePool::emptySlot {};

bool SamplePool::loadFromFile (int index, const juce::File& file, juce::String& errorOut)
{
    if (index < 0 || index >= numSlots) { errorOut = "slot out of range"; return false; }

    std::vector<float> samples;
    if (! loadAudioAsMono48k (file, samples, errorOut)) return false;

    setSlot (index, std::move (samples), file.getFileNameWithoutExtension(), file);
    return true;
}

void SamplePool::setSlot (int index, std::vector<float> samples,
                          const juce::String& name, const juce::File& source)
{
    if (index < 0 || index >= numSlots || samples.empty()) return;

    // Build the buffer outside the lock; the audio thread must never wait on
    // a file read or an allocation.
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (1, (int) samples.size());
    buffer->copyFrom (0, 0, samples.data(), (int) samples.size());

    Slot s;
    s.audio  = std::move (buffer);
    s.name   = name;
    s.source = source;

    const juce::SpinLock::ScopedLockType sl (lock);
    slots[(size_t) index] = std::move (s);
}

void SamplePool::clearSlot (int index)
{
    if (index < 0 || index >= numSlots) return;
    const juce::SpinLock::ScopedLockType sl (lock);
    slots[(size_t) index] = Slot {};
}

void SamplePool::clearAll()
{
    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& s : slots) s = Slot {};
}

const SamplePool::Slot& SamplePool::slot (int index) const
{
    if (index < 0 || index >= numSlots) return emptySlot;
    return slots[(size_t) index];
}

int SamplePool::numLoaded() const
{
    int n = 0;
    for (const auto& s : slots) if (! s.empty()) ++n;
    return n;
}

juce::String SamplePool::slotDescription (int index) const
{
    const auto& s = slot (index);
    if (s.empty()) return "-";
    return s.name + "  (" + juce::String (s.seconds(), 2) + " s)";
}

SamplePool::Audio SamplePool::audioForInstrumentSlot (int instrumentSlot) const noexcept
{
    const int index = slotIndex (instrumentSlot);
    if (index < 0 || index >= numSlots) return nullptr;

    // A try-lock keeps the audio thread free of priority inversion. Losing the
    // race means one missed trig at the instant a slot is being replaced,
    // which is inaudible; blocking would be a dropout.
    const juce::SpinLock::ScopedTryLockType sl (lock);
    if (! sl.isLocked()) return nullptr;
    return slots[(size_t) index].audio;
}
