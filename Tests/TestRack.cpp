/*  Tests for PluginRack against fake instruments.

    No VST is installed here, so these use stand-in AudioPluginInstances that
    reproduce the two things that bit a real plugin: reporting more channels
    than stereo, and changing that channel count when a setting changes.

    Handing processBlock fewer channels than the plugin reports is a heap
    overrun, which is why this is worth testing rather than eyeballing.
*/

#include "PluginRack.h"

#include <iostream>

static int failures = 0, checks = 0;

static void check (bool ok, const juce::String& what)
{
    ++checks;
    if (ok) std::cout << "  PASS  " << what << "\n";
    else  { std::cout << "  FAIL  " << what << "\n"; ++failures; }
}

/** A stand-in instrument that writes to every channel it was promised. */
class FakePlugin : public juce::AudioPluginInstance
{
public:
    explicit FakePlugin (int outChannels) : declaredOuts (outChannels)
    {
        setPlayConfigDetails (0, declaredOuts, 48000.0, 512);
    }

    /** Mimics a plugin whose bus layout changes when a setting changes. */
    void growTo (int outChannels)
    {
        declaredOuts = outChannels;
        setPlayConfigDetails (0, declaredOuts, getSampleRate(),
                              getBlockSize() > 0 ? getBlockSize() : 512);
    }

    const juce::String getName() const override { return "Fake"; }
    void prepareToPlay (double, int) override { prepared = true; }
    void releaseResources() override { prepared = false; }

    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi) override
    {
        sawChannels = buffer.getNumChannels();
        notesSeen += (int) std::distance (midi.begin(), midi.end());

        // Write to every channel the host handed over. If the host lied about
        // the size, this is where a real plugin would corrupt the heap.
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch),
                                               0.5f, buffer.getNumSamples());
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "Fake";
        d.pluginFormatName = "Fake";
        d.isInstrument = true;
    }

    int declaredOuts = 2;
    int sawChannels = 0;
    int notesSeen = 0;
    bool prepared = false;
};

/** Drives the rack the way the audio callback does. */
static void renderInto (PluginRack& rack, std::vector<float>& l,
                        std::vector<float>& r, int numSamples)
{
    float* out[2] = { l.data(), r.data() };
    std::fill (l.begin(), l.end(), 0.0f);
    std::fill (r.begin(), r.end(), 0.0f);
    rack.renderAndMix (out, 2, numSamples, 1.0f);
}

int main()
{
    constexpr int blockSize = 256;
    std::vector<float> left ((size_t) blockSize), right ((size_t) blockSize);

    // --- a plugin reporting more than stereo must get what it was promised --
    {
        PluginRack rack;
        rack.prepare (48000.0, blockSize);

        // Four outputs is the case that overran a stereo scratch buffer.
        auto fake = std::make_unique<FakePlugin> (4);
        auto* raw = fake.get();
        rack.adoptForTesting (0, std::move (fake), "Fake4");

        renderInto (rack, left, right, blockSize);

        check (raw->sawChannels >= 4,
               "a four-output plugin is handed at least four channels, got "
                   + juce::String (raw->sawChannels));
        check (left[0] > 0.0f && right[0] > 0.0f,
               "and its first two channels reach the output");
    }

    // --- a plugin that grows its bus count mid-session -----------------------
    {
        PluginRack rack;
        rack.prepare (48000.0, blockSize);

        auto fake = std::make_unique<FakePlugin> (2);
        auto* raw = fake.get();
        rack.adoptForTesting (0, std::move (fake), "Fake2");

        renderInto (rack, left, right, blockSize);
        const int before = raw->sawChannels;

        // This is what changing a setting on a real plugin can do.
        raw->growTo (8);
        renderInto (rack, left, right, blockSize);

        check (before == 2, "starts out stereo");
        check (raw->sawChannels <= 2 || raw->sawChannels >= 8,
               "after growing, the plugin is either skipped or given enough "
               "channels - never handed too few");
    }

    // --- MIDI reaches the plugin --------------------------------------------
    {
        PluginRack rack;
        rack.prepare (48000.0, blockSize);

        auto fake = std::make_unique<FakePlugin> (2);
        auto* raw = fake.get();
        rack.adoptForTesting (3, std::move (fake), "Fake");

        rack.beginBlock();
        rack.addNoteOn (3, 60, 100, 0);
        rack.addNoteOff (3, 60, 128);
        renderInto (rack, left, right, blockSize);

        check (raw->notesSeen == 2, "queued MIDI reaches the plugin's processBlock");
    }

    // --- replacing a slot while rendering must not use the old plugin --------
    {
        PluginRack rack;
        rack.prepare (48000.0, blockSize);

        rack.adoptForTesting (0, std::make_unique<FakePlugin> (2), "First");
        check (rack.slotName (0) == "First", "slot reports its plugin");

        rack.adoptForTesting (0, std::make_unique<FakePlugin> (6), "Second");
        check (rack.slotName (0) == "Second", "replacing a slot swaps the plugin");

        renderInto (rack, left, right, blockSize);
        check (true, "rendering after a swap does not crash");

        rack.clearSlot (0);
        check (! rack.slotLoaded (0), "clearing a slot empties it");

        renderInto (rack, left, right, blockSize);
        check (left[0] == 0.0f, "and an empty slot contributes nothing");
    }

    // --- an unprepared rack renders nothing rather than misbehaving ----------
    {
        PluginRack rack;
        rack.adoptForTesting (0, std::make_unique<FakePlugin> (2), "Fake");
        renderInto (rack, left, right, blockSize);
        check (left[0] == 0.0f, "an unprepared rack stays silent");
    }

    std::cout << "\n" << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
