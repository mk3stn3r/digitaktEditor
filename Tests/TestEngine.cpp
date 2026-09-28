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

static int failures = 0, checks = 0;

static void check (bool ok, const juce::String& what)
{
    ++checks;
    if (ok) std::cout << "  PASS  " << what << "\n";
    else  { std::cout << "  FAIL  " << what << "\n"; ++failures; }
}

/** A flat DC sample, so the level in the output identifies which slot fired. */
static void loadFlat (SamplePool& pool, int instrumentSlot, float value, int frames)
{
    pool.setSlot (SamplePool::slotIndex (instrumentSlot),
                  std::vector<float> ((size_t) frames, value),
                  "flat" + juce::String (instrumentSlot));
}

/** Renders `numSamples` through the engine and returns the mono output. */
static std::vector<float> render (SequencerEngine& engine, int numSamples,
                                  int blockSize = 512)
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

    std::cout << "\n" << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
