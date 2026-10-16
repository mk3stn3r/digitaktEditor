#include "PluginRack.h"

#include "Log.h"

/** Watches a hosted plugin for the changes a preset switch can bring.

    A preset can alter the bus layout, the latency or the tail length, and the
    host is expected to notice. Logging it as well means a crash inside a
    plugin has context immediately before it in the log. */
struct PluginWatcher : juce::AudioProcessorListener
{
    PluginWatcher (PluginRack& r, int s, juce::AudioPluginInstance& p)
        : rack (r), slot (s), plugin (p) {}

    void audioProcessorChanged (juce::AudioProcessor*,
                                const ChangeDetails& details) override
    {
        DTK_LOG ("plugin", "M" + juce::String (slot + 1) + " reported a change:"
                 + juce::String (details.latencyChanged ? " latency" : "")
                 + juce::String (details.parameterInfoChanged ? " parameters" : "")
                 + juce::String (details.programChanged ? " program" : "")
                 + juce::String (details.nonParameterStateChanged ? " state" : "")
                 + "  in=" + juce::String (plugin.getTotalNumInputChannels())
                 + " out=" + juce::String (plugin.getTotalNumOutputChannels())
                 + " rate=" + juce::String (plugin.getSampleRate(), 0)
                 + " block=" + juce::String (plugin.getBlockSize()));

        rack.needsReprepare.store (true);
    }

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    PluginRack& rack;
    int slot;
    juce::AudioPluginInstance& plugin;
};

namespace
{
    /** How many channels this plugin's processBlock needs handed to it. */
    int channelsRequired (juce::AudioPluginInstance& p)
    {
        return juce::jmax (2,
                           p.getTotalNumInputChannels(),
                           p.getTotalNumOutputChannels());
    }
}

void PluginRack::prepare (double sampleRate, int blockSize)
{
    const juce::ScopedLock sl (lock);
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    block = juce::jmax (16, blockSize);
    prepared = true;

    for (auto& s : slots)
    {
        if (s.plugin != nullptr)
        {
            s.plugin->setPlayConfigDetails (s.plugin->getTotalNumInputChannels(),
                                            juce::jmax (2, s.plugin->getTotalNumOutputChannels()),
                                            rate, block);
            s.plugin->prepareToPlay (rate, block);
            s.channels = channelsRequired (*s.plugin);
        }
        else
        {
            s.channels = 2;
        }
        s.scratch.setSize (s.channels, block, false, true, true);
    }
}

void PluginRack::releaseResources()
{
    const juce::ScopedLock sl (lock);
    for (auto& s : slots)
        if (s.plugin != nullptr)
            s.plugin->releaseResources();
    prepared = false;
}

bool PluginRack::loadPlugin (int slot, const juce::PluginDescription& desc,
                             juce::AudioPluginFormatManager& formats,
                             juce::String& errorOut)
{
    if (! valid (slot)) { errorOut = "slot out of range"; return false; }

    // Instantiate outside the lock: this can take seconds and must not block
    // the audio thread.
    auto created = formats.createPluginInstance (desc, rate, block, errorOut);
    if (created == nullptr)
    {
        if (errorOut.isEmpty()) errorOut = "the plugin could not be instantiated";
        return false;
    }

    // Ask for a plain stereo instrument layout, but take whatever the plugin
    // settles on: some refuse zero inputs, some expose extra buses.
    created->setPlayConfigDetails (0, 2, rate, block);
    created->prepareToPlay (rate, block);

    const int channels = channelsRequired (*created);

    // Size the scratch buffer outside the lock, before the audio thread can
    // ever see this plugin.
    juce::AudioBuffer<float> scratch (channels, block);
    scratch.clear();

    DTK_LOG ("plugin", "M" + juce::String (slot + 1) + " loading " + desc.name
             + " (" + desc.pluginFormatName + ")  in="
             + juce::String (created->getTotalNumInputChannels())
             + " out=" + juce::String (created->getTotalNumOutputChannels())
             + " channels=" + juce::String (channels)
             + " rate=" + juce::String (rate, 0)
             + " block=" + juce::String (block)
             + " latency=" + juce::String (created->getLatencySamples()));

    std::unique_ptr<juce::AudioPluginInstance> previous;
    std::unique_ptr<juce::AudioProcessorListener> previousWatcher;
    {
        const juce::ScopedLock sl (lock);
        auto& s = slots[(size_t) slot];

        if (s.plugin != nullptr && s.watcher != nullptr)
            s.plugin->removeListener (s.watcher.get());
        previousWatcher = std::move (s.watcher);
        previous = std::move (s.plugin);

        s.plugin   = std::move (created);
        s.name     = desc.name;
        s.channels = channels;
        s.scratch  = std::move (scratch);
        s.midi.clear();
    }

    attachWatcher (slot);

    // The old plugin is torn down outside the lock, and only after the audio
    // thread can no longer reach it.
    if (previous != nullptr) previous->releaseResources();
    return true;
}

void PluginRack::adoptForTesting (int slot,
                                  std::unique_ptr<juce::AudioPluginInstance> p,
                                  const juce::String& name)
{
    if (! valid (slot) || p == nullptr) return;

    p->prepareToPlay (rate, block);
    const int channels = channelsRequired (*p);

    juce::AudioBuffer<float> scratch (channels, block);
    scratch.clear();

    std::unique_ptr<juce::AudioPluginInstance> previous;
    {
        const juce::ScopedLock sl (lock);
        previous = std::move (slots[(size_t) slot].plugin);
        slots[(size_t) slot].plugin   = std::move (p);
        slots[(size_t) slot].name     = name;
        slots[(size_t) slot].channels = channels;
        slots[(size_t) slot].scratch  = std::move (scratch);
        slots[(size_t) slot].midi.clear();
    }
    if (previous != nullptr) previous->releaseResources();
}

void PluginRack::attachWatcher (int slot)
{
    auto& s = slots[(size_t) slot];
    if (s.plugin == nullptr) return;

    s.watcher = std::make_unique<PluginWatcher> (*this, slot, *s.plugin);
    s.plugin->addListener (s.watcher.get());
}

bool PluginRack::takeNeedsReprepare()
{
    return needsReprepare.exchange (false);
}

void PluginRack::reprepareChangedSlots()
{
    const juce::ScopedLock sl (lock);
    if (! prepared) return;

    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto& s = slots[i];
        if (s.plugin == nullptr) continue;

        const int needed = channelsRequired (*s.plugin);
        const bool rateMoved = std::abs (s.plugin->getSampleRate() - rate) > 1.0;
        const bool blockMoved = s.plugin->getBlockSize() != block;

        if (needed == s.channels && ! rateMoved && ! blockMoved) continue;

        // A preset change can move the channel count, and that needs nothing
        // from the plugin - just a bigger buffer on our side. Tearing its
        // resources down and rebuilding them while its editor is open, which
        // is exactly when a preset changes, is how you crash a plugin that
        // did nothing wrong.
        if (! rateMoved && ! blockMoved)
        {
            DTK_LOG ("plugin", "M" + juce::String ((int) i + 1)
                     + " channel count moved " + juce::String (s.channels)
                     + " -> " + juce::String (needed)
                     + "; resizing the buffer, not re-preparing");

            s.channels = needed;
            s.scratch.setSize (needed, block, false, true, true);
            continue;
        }

        // Rate or block size actually changed, which only happens when the
        // audio device changes - not while the user is clicking around in a
        // plugin's interface.
        DTK_LOG ("plugin", "M" + juce::String ((int) i + 1)
                 + " re-preparing: rate " + juce::String (s.plugin->getSampleRate(), 0)
                 + " -> " + juce::String (rate, 0)
                 + ", block " + juce::String (s.plugin->getBlockSize())
                 + " -> " + juce::String (block));

        s.plugin->releaseResources();
        s.plugin->setPlayConfigDetails (s.plugin->getTotalNumInputChannels(),
                                        juce::jmax (2, s.plugin->getTotalNumOutputChannels()),
                                        rate, block);
        s.plugin->prepareToPlay (rate, block);
        s.channels = channelsRequired (*s.plugin);
        s.scratch.setSize (s.channels, block, false, true, true);
    }
}

void PluginRack::clearSlot (int slot)
{
    if (! valid (slot)) return;

    std::unique_ptr<juce::AudioPluginInstance> doomed;
    std::unique_ptr<juce::AudioProcessorListener> doomedWatcher;
    {
        const juce::ScopedLock sl (lock);
        auto& s = slots[(size_t) slot];
        if (s.plugin != nullptr && s.watcher != nullptr)
            s.plugin->removeListener (s.watcher.get());
        doomedWatcher = std::move (s.watcher);
        doomed = std::move (s.plugin);
        s.name.clear();
        s.midi.clear();
    }
    DTK_LOG ("plugin", "M" + juce::String (slot + 1) + " cleared");
    // Destroy outside the lock; some plugins are slow to tear down.
    if (doomed != nullptr) doomed->releaseResources();
}

void PluginRack::clearAll()
{
    for (int i = 0; i < numSlots; ++i) clearSlot (i);
}

juce::AudioPluginInstance* PluginRack::instance (int slot)
{
    if (! valid (slot)) return nullptr;
    const juce::ScopedLock sl (lock);
    return slots[(size_t) slot].plugin.get();
}

juce::String PluginRack::slotName (int slot) const
{
    if (! valid (slot)) return {};
    const juce::ScopedLock sl (lock);
    return slots[(size_t) slot].name;
}

bool PluginRack::slotLoaded (int slot) const
{
    if (! valid (slot)) return false;
    const juce::ScopedLock sl (lock);
    return slots[(size_t) slot].plugin != nullptr;
}

int PluginRack::numLoaded() const
{
    const juce::ScopedLock sl (lock);
    int n = 0;
    for (const auto& s : slots) if (s.plugin != nullptr) ++n;
    return n;
}

void PluginRack::setSlotGain (int slot, float g)
{
    if (valid (slot)) slots[(size_t) slot].gain.store (juce::jlimit (0.0f, 2.0f, g));
}

float PluginRack::slotGain (int slot) const
{
    return valid (slot) ? slots[(size_t) slot].gain.load() : 0.0f;
}

//==============================================================================
void PluginRack::beginBlock()
{
    for (auto& s : slots) s.midi.clear();
}

void PluginRack::addNoteOn (int slot, int note, int velocity, int offset)
{
    if (! valid (slot)) return;
    slots[(size_t) slot].midi.addEvent (
        juce::MidiMessage::noteOn (1, juce::jlimit (0, 127, note),
                                   (juce::uint8) juce::jlimit (1, 127, velocity)),
        juce::jmax (0, offset));
}

void PluginRack::addNoteOff (int slot, int note, int offset)
{
    if (! valid (slot)) return;
    slots[(size_t) slot].midi.addEvent (
        juce::MidiMessage::noteOff (1, juce::jlimit (0, 127, note)),
        juce::jmax (0, offset));
}

void PluginRack::addAllNotesOff (int slot, int offset)
{
    if (! valid (slot)) return;
    slots[(size_t) slot].midi.addEvent (juce::MidiMessage::allNotesOff (1),
                                        juce::jmax (0, offset));
}

void PluginRack::addMessage (int slot, const juce::MidiMessage& message, int offset)
{
    if (! valid (slot) || ! message.getChannel()) return;

    // Hosted instruments all listen on channel 1; the source channel is what
    // selected the slot, so it carries no further meaning here.
    auto copy = message;
    copy.setChannel (1);
    slots[(size_t) slot].midi.addEvent (copy, juce::jmax (0, offset));
}

const juce::MidiBuffer& PluginRack::pendingMidi (int slot) const
{
    static const juce::MidiBuffer empty;
    if (! valid (slot)) return empty;
    return slots[(size_t) slot].midi;
}

void PluginRack::renderAndMix (float* const* out, int numChannels,
                               int numSamples, float masterGain)
{
    // A try-lock: if the message thread is swapping a plugin, this block gets
    // no plugin audio rather than a blocked audio thread.
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked() || ! prepared) return;

    const juce::ScopedNoDenormals noDenormals;

    for (auto& s : slots)
    {
        if (s.plugin == nullptr) continue;

        if (s.scratch.getNumSamples() < numSamples)
            continue;   // prepare() has not caught up with a bigger block yet

        // A plugin can change its bus layout when its settings change, so the
        // requirement is rechecked every block. Skipping is a dropout on one
        // slot; processing anyway would be a heap overrun.
        const int needed = channelsRequired (*s.plugin);
        if (needed > s.scratch.getNumChannels())
        {
            needsReprepare.store (true);   // the message thread will resize
            continue;
        }

        s.scratch.clear();
        juce::AudioBuffer<float> view (s.scratch.getArrayOfWritePointers(),
                                       needed, numSamples);
        s.plugin->processBlock (view, s.midi);

        const float g = s.gain.load() * masterGain;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (out[ch] == nullptr) continue;
            // Mono plugins feed both outputs; stereo maps straight across.
            const int src = juce::jmin (ch, view.getNumChannels() - 1);
            if (src < 0) continue;
            juce::FloatVectorOperations::addWithMultiply (out[ch],
                                                          view.getReadPointer (src),
                                                          g, numSamples);
        }
    }
}
