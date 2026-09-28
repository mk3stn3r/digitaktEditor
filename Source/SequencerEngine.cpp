#include "SequencerEngine.h"

#include <cmath>

void SequencerEngine::updateFromPattern (const dtk::PatternKit& pk)
{
    // Write into a buffer the audio thread is not reading, then publish it.
    // With four buffers and a single writer, a reader would have to be four
    // updates behind to see a torn snapshot, which cannot happen at UI rates.
    writeSnapshot = (liveSnapshot.load() + 1) % numSnapshots;
    auto& s = snapshots[writeSnapshot];

    s.tempo  = pk.pattern.tempo();
    s.length = juce::jlimit (1, maxSteps, pk.pattern.length());

    const int steps = juce::jmin ((int) pk.pattern.tracks.size() > 0
                                      ? (int) pk.pattern.tracks[0].steps.size()
                                      : 0,
                                  maxSteps);

    for (int t = 0; t < numAudioTracks; ++t)
    {
        const bool haveTrack = t < (int) pk.pattern.tracks.size() && pk.isAudioTrack (t);
        const int trackSlot = (haveTrack && t < (int) pk.kit.sounds.size())
                                  ? pk.kit.sounds[(size_t) t].sampleSlot : 0;

        for (int i = 0; i < maxSteps; ++i)
        {
            StepData d;
            if (haveTrack && i < steps)
            {
                const auto& tr = pk.pattern.tracks[(size_t) t];
                d.trig = tr.trig (i);
                if (d.trig)
                {
                    d.note     = tr.lock ("note", i).value_or (tr.defaultNote());
                    d.velocity = tr.lock ("velocity", i).value_or (tr.defaultVelocity());
                    d.length   = tr.lock ("length", i).value_or (0);
                    // A sample p-lock overrides the track's kit sample.
                    d.slot     = dtk::SampleLockLane::get (pk, t, i).value_or (trackSlot);
                }
            }
            s.steps[t][i] = d;
        }
    }

    liveSnapshot.store (writeSnapshot);
}

void SequencerEngine::start()
{
    stepIndex.store (0);
    samplesUntilNextStep = 0.0;
    playing.store (true);
}

void SequencerEngine::stop()
{
    playing.store (false);
}

void SequencerEngine::triggerAudition (int instrumentSlot, int note, float velocity)
{
    auditionNote.store (note);
    auditionVelocity.store (velocity);
    auditionSlot.store (instrumentSlot);   // picked up by the audio thread
}

void SequencerEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    if (sampleRate <= 0.0) sampleRate = 48000.0;
    samplesUntilNextStep = 0.0;
    for (auto& v : voices) v.reset();
    auditionVoice.reset();
}

void SequencerEngine::audioDeviceStopped()
{
    playing.store (false);
    for (auto& v : voices) v.reset();
    auditionVoice.reset();
}

void SequencerEngine::startVoice (Voice& v, const SamplePool::Audio& audio,
                                  int note, float velocity, double gateSamples)
{
    v.audio = audio;
    v.position = 0.0;
    // 60 is the Digitakt's original-pitch note, as in the track defaults.
    v.increment = std::pow (2.0, (double) (note - 60) / 12.0)
                    * (SamplePool::rate / sampleRate);
    v.gain = velocity;
    v.gateSamplesLeft = gateSamples;
    v.active = true;
}

void SequencerEngine::fireStep (int step)
{
    const auto& s = snapshots[liveSnapshot.load()];
    const double samplesPerStep = sampleRate * 60.0 / juce::jmax (30.0, s.tempo) / 4.0;

    for (int t = 0; t < numAudioTracks; ++t)
    {
        const auto& d = s.steps[t][step];
        if (! d.trig || d.slot <= 0) continue;

        auto audio = pool.audioForInstrumentSlot (d.slot);
        if (audio == nullptr) continue;      // slot is empty locally

        const double gate = d.length > 0 ? d.length * samplesPerStep : -1.0;
        startVoice (voices[t], audio, d.note,
                    juce::jlimit (0.0f, 1.0f, (float) d.velocity / 127.0f), gate);
    }
}

void SequencerEngine::advanceStep()
{
    const auto& s = snapshots[liveSnapshot.load()];
    int next = stepIndex.load() + 1;

    if (next >= s.length)
    {
        if (! looping.load()) { playing.store (false); return; }
        next = 0;
    }
    stepIndex.store (next);
    fireStep (next);
}

void SequencerEngine::audioDeviceIOCallbackWithContext (
    const float* const*, int, float* const* outputChannelData,
    int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    // Pick up an audition request without blocking the message thread.
    const int audition = auditionSlot.exchange (-1);
    if (audition > 0)
        if (auto audio = pool.audioForInstrumentSlot (audition))
            startVoice (auditionVoice, audio, auditionNote.load(),
                        auditionVelocity.load(), -1.0);

    const auto& snap = snapshots[liveSnapshot.load()];
    const double samplesPerStep =
        sampleRate * 60.0 / juce::jmax (30.0, snap.tempo) / 4.0;

    const float masterGain = gain.load();
    int offset = 0;

    while (offset < numSamples)
    {
        int block = numSamples - offset;

        if (playing.load())
        {
            if (samplesUntilNextStep <= 0.0)
            {
                // The very first step of a run fires here rather than in
                // start(), so it lands on a buffer boundary with a valid rate.
                fireStep (stepIndex.load());
                samplesUntilNextStep = samplesPerStep;
            }
            block = juce::jmin (block, (int) std::ceil (samplesUntilNextStep));
            block = juce::jmax (1, block);
        }

        auto renderVoice = [&] (Voice& v)
        {
            if (! v.active || v.audio == nullptr) return;

            const auto* src = v.audio->getReadPointer (0);
            const int len = v.audio->getNumSamples();

            for (int i = 0; i < block; ++i)
            {
                if (v.position >= (double) (len - 1) || v.gateSamplesLeft == 0.0)
                {
                    v.reset();
                    return;
                }

                const int i0 = (int) v.position;
                const float frac = (float) (v.position - (double) i0);
                const float sample = src[i0] + frac * (src[i0 + 1] - src[i0]);
                const float out = sample * v.gain * masterGain;

                for (int ch = 0; ch < numOutputChannels; ++ch)
                    if (outputChannelData[ch] != nullptr)
                        outputChannelData[ch][offset + i] += out;

                v.position += v.increment;
                if (v.gateSamplesLeft > 0.0)
                {
                    v.gateSamplesLeft -= 1.0;
                    if (v.gateSamplesLeft <= 0.0) { v.reset(); return; }
                }
            }
        };

        for (auto& v : voices) renderVoice (v);
        renderVoice (auditionVoice);

        if (playing.load())
        {
            samplesUntilNextStep -= (double) block;
            if (samplesUntilNextStep <= 0.0)
            {
                advanceStep();
                samplesUntilNextStep += samplesPerStep;
            }
        }
        offset += block;
    }
}
