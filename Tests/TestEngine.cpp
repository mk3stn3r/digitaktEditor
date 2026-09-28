/*  Tests for the offline sample sequencer.

    The audio callback can be driven directly with scratch buffers, so step
    timing, voice triggering, sample-lock overrides, transposition, velocity
    and gating are all testable without a sound card - and without ears.
*/

#include "SequencerEngine.h"
#include "SamplePool.h"
#include "DigitaktCore.h"

#include <cmath>
#include <iostream>
#include <vector>

/** Captures the engine's remote-control MIDI instead of sending it anywhere. */
struct CapturingSink : MidiSink
{
    struct Event { double seconds; juce::MidiMessage message; };
    std::vector<Event> events;
    double elapsed = 0.0;

    void sendBlock (const juce::MidiBuffer& buffer, double sampleRate) override
    {
        for (const auto meta : buffer)
            events.push_back ({ elapsed + meta.samplePosition / sampleRate,
                                meta.getMessage() });
    }

    /** Called by the test harness between blocks to keep absolute time. */
    void advance (int numSamples, double sampleRate)
        { elapsed += numSamples / sampleRate; }

    int count (bool (*pred) (const juce::MidiMessage&)) const
    {
        int n = 0;
        for (const auto& e : events) if (pred (e.message)) ++n;
        return n;
    }
    void clear() { events.clear(); elapsed = 0.0; }
};

static bool isClock (const juce::MidiMessage& m) { return m.isMidiClock(); }
static bool isStart (const juce::MidiMessage& m) { return m.isMidiStart(); }
static bool isStop  (const juce::MidiMessage& m) { return m.isMidiStop(); }
static bool isNoteOn (const juce::MidiMessage& m) { return m.isNoteOn(); }
static bool isNoteOff (const juce::MidiMessage& m) { return m.isNoteOff(); }

static int failures = 0, checks = 0;

static void check (bool ok, const juce::String& what)
{
    ++checks;
    if (ok) std::cout << "  PASS  " << what << "\n";
    else  { std::cout << "  FAIL  " << what << "\n"; ++failures; }
}

/** A flat DC sample, so the level in the output identifies which slot fired. */
static void loadFlat (SamplePool& pool, int slot, float value, int frames)
{
    pool.setSlot (slot, std::vector<float> ((size_t) frames, value),
                  "flat" + juce::String (slot));
}

/** Renders `numSamples` through the engine and returns the mono output. */
static std::vector<float> render (SequencerEngine& engine, int numSamples,
                                  int blockSize = 512, CapturingSink* sink = nullptr)
{
    std::vector<float> out;
    out.reserve ((size_t) numSamples);

    std::vector<float> left ((size_t) blockSize), right ((size_t) blockSize);
    float* channels[2] = { left.data(), right.data() };
    juce::AudioIODeviceCallbackContext ctx;

    int done = 0;
    while (done < numSamples)
    {
        const int n = juce::jmin (blockSize, numSamples - done);
        engine.audioDeviceIOCallbackWithContext (nullptr, 0, channels, 2, n, ctx);
        for (int i = 0; i < n; ++i) out.push_back (left[(size_t) i]);
        if (sink != nullptr) sink->advance (n, 48000.0);
        done += n;
    }
    return out;
}

static float peakBetween (const std::vector<float>& x, int from, int to)
{
    float p = 0.0f;
    for (int i = juce::jmax (0, from); i < juce::jmin ((int) x.size(), to); ++i)
        p = juce::jmax (p, std::abs (x[(size_t) i]));
    return p;
}

/** First index at or after `from` where the signal rises above `threshold`. */
static int firstOnset (const std::vector<float>& x, float threshold, int from = 0)
{
    for (int i = from; i < (int) x.size(); ++i)
        if (std::abs (x[(size_t) i]) > threshold) return i;
    return -1;
}

int main()
{
    // 120 BPM sixteenths at 48 kHz = 6000 samples per step.
    constexpr int stepSamples = 6000;

    // --- slot numbering ------------------------------------------------------
    {
        // A sample slot value is a direct index with 0 meaning "off", so
        // usable slots are 1 to 127. Getting this wrong would play the sample
        // next door to the one the pattern asks for.
        SamplePool pool;
        check (! SamplePool::isUsableSlot (0), "slot 0 is the 'off' entry");
        check (SamplePool::isUsableSlot (1) && SamplePool::isUsableSlot (127),
               "slots 1 to 127 are usable");
        check (! SamplePool::isUsableSlot (128), "there is no slot 128");

        loadFlat (pool, 1, 0.5f, 100);
        check (pool.audioForSlot (1) != nullptr, "slot 1 holds what was put there");
        check (pool.audioForSlot (0) == nullptr, "slot 0 never holds audio");
        check (pool.audioForSlot (2) == nullptr, "and nothing leaked into slot 2");
        check (pool.numLoaded() == 1, "one slot reports as loaded");

        pool.setSlot (0, std::vector<float> (100, 1.0f), "nope");
        check (pool.audioForSlot (0) == nullptr && pool.numLoaded() == 1,
               "writing to slot 0 is refused");

        check (pool.firstFreeSlot() == 2, "the first free slot skips what is used");
    }

    // --- silence when nothing is loaded -------------------------------------
    {
        SamplePool pool;
        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        dtk::PatternEditor ed (pk);
        ed.setTrig (0, 0, true);
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, stepSamples * 4);
        check (peakBetween (out, 0, (int) out.size()) == 0.0f,
               "an empty pool plays silence rather than crashing");
    }

    // --- a trig fires the track's kit sample --------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 5, 0.5f, 2000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        ed.setVelocity (0, 0, 127);
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, stepSamples * 2);
        check (std::abs (peakBetween (out, 0, 1000) - 0.5f) < 0.01f,
               "a trig plays the track's kit sample at full velocity");
        check (peakBetween (out, 3000, stepSamples) == 0.0f,
               "and stops when the sample runs out");
    }

    // --- step timing --------------------------------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 5, 0.5f, 1000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTempo (120.0);
        for (int s : { 0, 4, 8, 12 })
        {
            ed.setTrig (0, s, true);
            ed.setVelocity (0, s, 127);
        }
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, stepSamples * 16);

        bool timingOk = true;
        for (int beat = 0; beat < 4; ++beat)
        {
            const int expected = beat * 4 * stepSamples;
            const int onset = firstOnset (out, 0.1f, expected - 2);
            if (onset < 0 || std::abs (onset - expected) > 2) timingOk = false;
        }
        check (timingOk, "four-on-the-floor lands within 2 samples of each beat");

        // Using an odd block size must not shift the grid.
        SequencerEngine e2 (pool);
        e2.audioDeviceAboutToStart (nullptr);
        e2.setGain (1.0f);
        e2.updateFromPattern (pk);
        e2.start();
        const auto odd = render (e2, stepSamples * 8, 337);
        check (std::abs (firstOnset (odd, 0.1f, 4 * stepSamples - 2)
                         - 4 * stepSamples) <= 2,
               "timing is independent of the audio block size");
    }

    // --- velocity, transposition, gating ------------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 5, 1.0f, 48000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        ed.setVelocity (0, 0, 64);
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, 2000);
        check (std::abs (peakBetween (out, 0, 1000) - 64.0f / 127.0f) < 0.01f,
               "velocity scales the output level");
    }

    {
        // A one-second sample at note 72 is an octave up, so it should run out
        // in half the time.
        SamplePool pool;
        loadFlat (pool, 5, 0.5f, 48000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (64);
        ed.setTrig (0, 0, true);
        ed.setNote (0, 0, 72);
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, 48000);
        check (peakBetween (out, 23000, 23500) > 0.1f
               && peakBetween (out, 25000, 48000) == 0.0f,
               "note 72 plays an octave up, finishing in half the time");
    }

    {
        // LEN gates the voice: 2 steps of a much longer sample.
        SamplePool pool;
        loadFlat (pool, 5, 0.5f, 48000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (64);
        ed.setTrig (0, 0, true);
        ed.setStepLength (0, 0, 2);
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, stepSamples * 4);
        check (peakBetween (out, 0, stepSamples) > 0.1f
               && peakBetween (out, 2 * stepSamples + 200, 3 * stepSamples) == 0.0f,
               "a length lock gates the voice after that many steps");
    }

    // --- the sample sequencer: a p-lock overrides the kit sample -------------
    {
        SamplePool pool;
        loadFlat (pool, 5, 0.25f, 1000);    // the track's kit sample
        loadFlat (pool, 9, 0.75f, 1000);    // the locked sample

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTempo (120.0);
        for (int s : { 0, 4 }) { ed.setTrig (0, s, true); ed.setVelocity (0, s, 127); }
        ed.setSampleLock (0, 4, 9);          // step 4 fires slot 9 instead
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, stepSamples * 8);
        const float first = peakBetween (out, 0, 900);
        const float second = peakBetween (out, 4 * stepSamples, 4 * stepSamples + 900);

        check (std::abs (first - 0.25f) < 0.01f,
               "an unlocked step plays the track's kit sample");
        check (std::abs (second - 0.75f) < 0.01f,
               "a sample-locked step plays the locked slot instead");
    }

    // --- looping and one-shot -----------------------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 5, 0.5f, 500);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (4);                    // a very short pattern
        ed.setTempo (120.0);
        ed.setTrig (0, 0, true);
        ed.setVelocity (0, 0, 127);
        engine.updateFromPattern (pk);

        engine.setLooping (true);
        engine.start();
        auto out = render (engine, stepSamples * 12);
        check (firstOnset (out, 0.1f, 4 * stepSamples - 2) > 0
               && firstOnset (out, 0.1f, 8 * stepSamples - 2) > 0,
               "a looping pattern retriggers every cycle");
        check (engine.isPlaying(), "and keeps playing");

        engine.stop();
        engine.setLooping (false);
        engine.start();
        out = render (engine, stepSamples * 12);
        check (! engine.isPlaying(), "a non-looping pattern stops at the end");
        check (peakBetween (out, 5 * stepSamples, 12 * stepSamples) == 0.0f,
               "and makes no sound past the pattern length");
    }

    // --- MIDI tracks have no samples ----------------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 5, 0.5f, 1000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (9, 0, true);             // a MIDI track
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, stepSamples * 2);
        check (peakBetween (out, 0, (int) out.size()) == 0.0f,
               "a trig on a MIDI track produces no audio");
    }

    // --- a track pointed at slot 0 is off ------------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 1, 0.5f, 1000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 0;     // "off" on the instrument
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, stepSamples * 2);
        check (peakBetween (out, 0, (int) out.size()) == 0.0f,
               "a track whose sample slot is 0 makes no sound");
    }

    // --- auditioning a slot --------------------------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 7, 0.6f, 2000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        engine.triggerAudition (7);
        const auto out = render (engine, 4000);
        check (std::abs (peakBetween (out, 0, 1500) - 0.6f) < 0.01f,
               "auditioning plays a slot without the transport running");
        check (! engine.isPlaying(), "and does not start the sequencer");
    }

    // --- editing while playing -----------------------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 5, 0.5f, 1000);

        SequencerEngine engine (pool);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        engine.updateFromPattern (pk);
        engine.start();

        render (engine, 1000);

        // Publish new snapshots repeatedly, as the UI does while editing.
        for (int i = 0; i < 20; ++i)
        {
            ed.setTrig (0, (i % 15) + 1, true);
            engine.updateFromPattern (pk);
            render (engine, 700);
        }
        check (engine.isPlaying(), "editing while playing does not stop or crash it");
    }

    // --- remote control: clock, transport and notes --------------------------
    {
        SamplePool pool;
        SequencerEngine engine (pool);
        CapturingSink sink;
        engine.setMidiSink (&sink);
        engine.audioDeviceAboutToStart (nullptr);

        SequencerEngine::RemoteConfig cfg;
        cfg.enabled = true;
        cfg.firstTrackChannel = 1;
        engine.setRemoteConfig (cfg);

        auto pk = dtk::blankPatternKit (9);
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTempo (120.0);
        ed.setTrig (0, 0, true);
        ed.setNote (0, 0, 48);
        ed.setVelocity (0, 0, 100);
        ed.setTrig (2, 4, true);             // a different track
        engine.updateFromPattern (pk);

        engine.start();
        // Just short of a full bar: landing exactly on the loop point would
        // wrap and fire step 0 a second time, which is correct behaviour but
        // makes the counts below ambiguous.
        render (engine, stepSamples * 16 - 200, 512, &sink);

        check (sink.count (isStart) == 1, "starting sends exactly one MIDI start");

        // One bar of sixteenths is 4 quarter notes, so 96 clocks at 24 ppqn.
        const int clocks = sink.count (isClock);
        check (clocks == 96,
               "one bar produces exactly 96 MIDI clocks (24 ppqn), got "
                   + juce::String (clocks));

        // Clock spacing should be even: 120 BPM / 24 ppqn = 20.833 ms.
        double worstJitter = 0.0;
        double previous = -1.0;
        for (const auto& e : sink.events)
        {
            if (! e.message.isMidiClock()) continue;
            if (previous >= 0.0)
                worstJitter = juce::jmax (worstJitter,
                                          std::abs ((e.seconds - previous) - 0.0208333));
            previous = e.seconds;
        }
        check (worstJitter < 0.0005,
               "clock spacing is even to under half a millisecond");

        check (sink.count (isNoteOn) == 2, "each trig sends one note on");

        // The note must carry the step's pitch and velocity on the track's channel.
        bool noteOk = false, channelOk = false;
        for (const auto& e : sink.events)
        {
            if (! e.message.isNoteOn()) continue;
            if (e.message.getNoteNumber() == 48 && e.message.getVelocity() == 100
                && e.message.getChannel() == 1)
                noteOk = true;
            if (e.message.getNoteNumber() == 60 && e.message.getChannel() == 3)
                channelOk = true;
        }
        check (noteOk, "a note carries the step's pitch and velocity");
        check (channelOk, "track 3 sends on channel 3 with the track default note");

        check (sink.count (isNoteOff) >= 2, "every note is released");

        sink.clear();
        engine.stop();
        render (engine, 2048, 512, &sink);
        check (sink.count (isStop) == 1, "stopping sends one MIDI stop");
    }

    // --- remote off means silence on the wire --------------------------------
    {
        SamplePool pool;
        SequencerEngine engine (pool);
        CapturingSink sink;
        engine.setMidiSink (&sink);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setRemoteConfig (SequencerEngine::RemoteConfig {});   // disabled

        auto pk = dtk::blankPatternKit (9);
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        engine.updateFromPattern (pk);
        engine.start();
        render (engine, stepSamples * 4, 512, &sink);

        check (sink.events.empty(),
               "with remote disabled the editor sends no MIDI at all");
    }

    // --- the channel base shifts every track ---------------------------------
    {
        SamplePool pool;
        SequencerEngine engine (pool);
        CapturingSink sink;
        engine.setMidiSink (&sink);
        engine.audioDeviceAboutToStart (nullptr);

        SequencerEngine::RemoteConfig cfg;
        cfg.enabled = true;
        cfg.sendClock = false;
        cfg.firstTrackChannel = 9;
        engine.setRemoteConfig (cfg);

        auto pk = dtk::blankPatternKit (9);
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        ed.setTrig (7, 0, true);            // track 8 -> channel 16
        engine.updateFromPattern (pk);
        engine.start();
        render (engine, stepSamples * 2, 512, &sink);

        bool ch9 = false, ch16 = false;
        for (const auto& e : sink.events)
        {
            if (! e.message.isNoteOn()) continue;
            if (e.message.getChannel() == 9)  ch9 = true;
            if (e.message.getChannel() == 16) ch16 = true;
        }
        check (ch9 && ch16, "the channel base shifts every track together");
        check (sink.count (isClock) == 0, "clock can be turned off independently");
    }

    // --- a retrig releases the previous note ---------------------------------
    {
        SamplePool pool;
        SequencerEngine engine (pool);
        CapturingSink sink;
        engine.setMidiSink (&sink);
        engine.audioDeviceAboutToStart (nullptr);

        SequencerEngine::RemoteConfig cfg;
        cfg.enabled = true;
        cfg.sendClock = false;
        engine.setRemoteConfig (cfg);

        auto pk = dtk::blankPatternKit (9);
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        for (int s = 0; s < 8; ++s)
        {
            ed.setTrig (0, s, true);
            ed.setStepLength (0, s, 8);     // gates deliberately longer than a step
        }
        engine.updateFromPattern (pk);
        engine.start();
        render (engine, stepSamples * 10, 512, &sink);

        // Track monophony means offs must keep pace with ons, or the
        // instrument is left holding notes down.
        const int ons = sink.count (isNoteOn), offs = sink.count (isNoteOff);
        check (ons == 8 && offs >= 7,
               "overlapping trigs on one track still release: " + juce::String (ons)
                   + " on, " + juce::String (offs) + " off");
    }

    // --- stopping releases everything ----------------------------------------
    {
        SamplePool pool;
        SequencerEngine engine (pool);
        CapturingSink sink;
        engine.setMidiSink (&sink);
        engine.audioDeviceAboutToStart (nullptr);

        SequencerEngine::RemoteConfig cfg;
        cfg.enabled = true;
        cfg.sendClock = false;
        engine.setRemoteConfig (cfg);

        auto pk = dtk::blankPatternKit (9);
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        ed.setStepLength (0, 0, 60);        // a very long gate
        engine.updateFromPattern (pk);
        engine.start();
        render (engine, stepSamples, 512, &sink);

        const int beforeStop = sink.count (isNoteOff);
        engine.stop();
        render (engine, 1024, 512, &sink);

        check (sink.count (isNoteOff) > beforeStop,
               "stopping releases notes still held, leaving no stuck note");
    }

    // --- remote only, with local audio off -----------------------------------
    {
        SamplePool pool;
        loadFlat (pool, 5, 0.5f, 2000);

        SequencerEngine engine (pool);
        CapturingSink sink;
        engine.setMidiSink (&sink);
        engine.audioDeviceAboutToStart (nullptr);
        engine.setGain (1.0f);

        SequencerEngine::RemoteConfig cfg;
        cfg.enabled = true;
        cfg.sendClock = false;
        engine.setRemoteConfig (cfg);
        engine.setLocalAudioEnabled (false);

        auto pk = dtk::blankPatternKit (9);
        pk.kit.sounds[0].sampleSlot = 5;
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        engine.updateFromPattern (pk);
        engine.start();

        const auto out = render (engine, stepSamples, 512, &sink);
        check (peakBetween (out, 0, (int) out.size()) == 0.0f,
               "local audio off makes no sound locally");
        check (sink.count (isNoteOn) == 1, "but the trig still goes to the instrument");
    }

    // --- a trig with no local sample still drives the instrument -------------
    {
        SamplePool pool;                     // deliberately empty
        SequencerEngine engine (pool);
        CapturingSink sink;
        engine.setMidiSink (&sink);
        engine.audioDeviceAboutToStart (nullptr);

        SequencerEngine::RemoteConfig cfg;
        cfg.enabled = true;
        cfg.sendClock = false;
        engine.setRemoteConfig (cfg);

        auto pk = dtk::blankPatternKit (9);
        dtk::PatternEditor ed (pk);
        ed.setLength (16);
        ed.setTrig (0, 0, true);
        engine.updateFromPattern (pk);
        engine.start();
        render (engine, stepSamples, 512, &sink);

        check (sink.count (isNoteOn) == 1,
               "an empty local pool does not stop the instrument being played");
    }

    std::cout << "\n" << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
