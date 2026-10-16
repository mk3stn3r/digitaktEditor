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

    for (int t = 0; t < numTracks; ++t)
    {
        // Audio tracks play the sample pool; MIDI tracks feed the plugin rack.
        // Both need their step data, so neither is skipped here.
        const bool haveTrack = t < (int) pk.pattern.tracks.size();
        const bool audio = haveTrack && pk.isAudioTrack (t);
        const int trackSlot = (audio && t < (int) pk.kit.sounds.size())
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
                    // A sample p-lock overrides the track's kit sample. MIDI
                    // tracks have no sample, so the slot stays 0 for them.
                    d.slot     = audio
                        ? dtk::SampleLockLane::get (pk, t, i).value_or (trackSlot)
                        : 0;
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
    samplesUntilNextClock = 0.0;
    pendingStart.store (true);
    playing.store (true);
}

void SequencerEngine::stop()
{
    playing.store (false);
    pendingStop.store (true);
}

void SequencerEngine::handleExternalMidi (const juce::MidiMessage& m)
{
    if (! externalMidi.load()) return;
    externalCollector.addMessageToQueue (m);
}

void SequencerEngine::setExternalMidiInput (bool enabled, int baseChannel)
{
    externalBase.store (juce::jlimit (1, 16, baseChannel));
    externalMidi.store (enabled);
    if (! enabled)
        for (auto& n : externalRouted) n.store (0);
}

int SequencerEngine::externalMessagesRouted (int slot) const
{
    if (slot < 0 || slot >= numMidiTracks) return 0;
    return externalRouted[slot].load();
}

void SequencerEngine::setMidiSink (MidiSink* s)
{
    const juce::SpinLock::ScopedLockType sl (remoteLock);
    sink = s;
}

void SequencerEngine::setRemoteConfig (RemoteConfig c)
{
    c.firstTrackChannel = juce::jlimit (1, 16, c.firstTrackChannel);
    const juce::SpinLock::ScopedLockType sl (remoteLock);
    remote = c;
}

SequencerEngine::RemoteConfig SequencerEngine::remoteConfig() const
{
    const juce::SpinLock::ScopedLockType sl (remoteLock);
    return remote;
}

SequencerEngine::RemoteConfig SequencerEngine::currentRemote() const
{
    // The audio thread must not wait on the message thread; a missed update
    // costs one block of stale settings, which is inaudible.
    const juce::SpinLock::ScopedTryLockType sl (remoteLock);
    if (! sl.isLocked()) return RemoteConfig {};
    return remote;
}

void SequencerEngine::emitNoteOn (int track, const StepData& d,
                                  double samplesPerStep, int offset)
{
    const auto cfg = currentRemote();
    if (! cfg.enabled || ! cfg.sendNotes) return;

    const int channel = cfg.firstTrackChannel + track;
    if (channel > 16) return;              // track would fall off the end

    // Each Digitakt track is monophonic, so a retrig must release the last
    // note or the instrument is left holding it.
    if (activeMidiNote[track] >= 0) emitNoteOff (track, offset);

    const int note = juce::jlimit (0, 127, d.note);
    const auto vel = (juce::uint8) juce::jlimit (1, 127, d.velocity);
    midiOut.addEvent (juce::MidiMessage::noteOn (channel, note, vel), offset);

    activeMidiNote[track] = note;
    midiGateLeft[track] = d.length > 0 ? d.length * samplesPerStep : samplesPerStep;
}

void SequencerEngine::emitNoteOff (int track, int offset)
{
    if (activeMidiNote[track] < 0) return;

    const auto cfg = currentRemote();
    const int channel = cfg.firstTrackChannel + track;
    if (channel <= 16)
        midiOut.addEvent (juce::MidiMessage::noteOff (channel, activeMidiNote[track]),
                          offset);

    activeMidiNote[track] = -1;
    midiGateLeft[track] = -1.0;
}

void SequencerEngine::allNotesOff (int offset)
{
    for (int t = 0; t < numAudioTracks; ++t) emitNoteOff (t, offset);
    for (int t = 0; t < numMidiTracks; ++t) emitPluginNoteOff (t, offset);
}

void SequencerEngine::emitPluginNoteOn (int midiTrack, const StepData& d,
                                        double samplesPerStep, int offset)
{
    if (midiTrack < 0 || midiTrack >= numMidiTracks) return;

    // The Digitakt's MIDI tracks are sequenced one note per step here, so a
    // retrig releases the held note before starting the next.
    if (pluginNote[midiTrack] >= 0) emitPluginNoteOff (midiTrack, offset);

    const int note = juce::jlimit (0, 127, d.note);
    rack.addNoteOn (midiTrack, note, d.velocity, offset);

    pluginNote[midiTrack] = note;
    pluginGateLeft[midiTrack] = d.length > 0 ? d.length * samplesPerStep
                                             : samplesPerStep;
}

void SequencerEngine::emitPluginNoteOff (int midiTrack, int offset)
{
    if (midiTrack < 0 || midiTrack >= numMidiTracks) return;
    if (pluginNote[midiTrack] < 0) return;

    rack.addNoteOff (midiTrack, pluginNote[midiTrack], offset);
    pluginNote[midiTrack] = -1;
    pluginGateLeft[midiTrack] = -1.0;
}

void SequencerEngine::triggerAudition (int slot, int note, float velocity)
{
    auditionNote.store (note);
    auditionVelocity.store (velocity);
    auditionSlot.store (slot);             // picked up by the audio thread
}

void SequencerEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    if (sampleRate <= 0.0) sampleRate = 48000.0;
    samplesUntilNextStep = 0.0;
    samplesUntilNextClock = 0.0;
    for (auto& v : voices) v.reset();
    auditionVoice.reset();
    for (int t = 0; t < numAudioTracks; ++t)
    {
        activeMidiNote[t] = -1;
        midiGateLeft[t] = -1.0;
    }
    for (int t = 0; t < numMidiTracks; ++t)
    {
        pluginNote[t] = -1;
        pluginGateLeft[t] = -1.0;
    }
    rack.prepare (sampleRate, device != nullptr
                                  ? device->getCurrentBufferSizeSamples() : 512);
    externalCollector.reset (sampleRate);
}

void SequencerEngine::audioDeviceStopped()
{
    playing.store (false);
    pendingStop.store (true);
    rack.releaseResources();
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

void SequencerEngine::fireStep (int step, int sampleOffset)
{
    const auto& s = snapshots[liveSnapshot.load()];
    const double samplesPerStep = sampleRate * 60.0 / juce::jmax (30.0, s.tempo) / 4.0;

    for (int t = numAudioTracks; t < numTracks; ++t)
    {
        const auto& d = s.steps[t][step];
        if (d.trig) emitPluginNoteOn (t - numAudioTracks, d, samplesPerStep,
                                      sampleOffset);
    }

    for (int t = 0; t < numAudioTracks; ++t)
    {
        const auto& d = s.steps[t][step];
        if (! d.trig) continue;

        // The instrument plays its own sample for the track, so a trig is
        // worth sending even when the local pool has nothing in that slot.
        emitNoteOn (t, d, samplesPerStep, sampleOffset);

        if (d.slot <= 0 || ! localAudio.load()) continue;

        auto audio = pool.audioForSlot (d.slot);
        if (audio == nullptr) continue;      // slot is empty locally

        const double gate = d.length > 0 ? d.length * samplesPerStep : -1.0;
        startVoice (voices[t], audio, d.note,
                    juce::jlimit (0.0f, 1.0f, (float) d.velocity / 127.0f), gate);
    }
}

void SequencerEngine::advanceStep (int sampleOffset)
{
    const auto& s = snapshots[liveSnapshot.load()];
    int next = stepIndex.load() + 1;

    if (next >= s.length)
    {
        if (! looping.load())
        {
            playing.store (false);
            pendingStop.store (true);
            return;
        }
        next = 0;
    }
    stepIndex.store (next);
    fireStep (next, sampleOffset);
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
        if (auto audio = pool.audioForSlot (audition))
            startVoice (auditionVoice, audio, auditionNote.load(),
                        auditionVelocity.load(), -1.0);

    const auto& snap = snapshots[liveSnapshot.load()];
    const double samplesPerStep =
        sampleRate * 60.0 / juce::jmax (30.0, snap.tempo) / 4.0;
    // 24 pulses per quarter note is the MIDI clock rate, so six per sixteenth.
    const double samplesPerClock = samplesPerStep / 6.0;

    const auto cfg = currentRemote();
    midiOut.clear();
    rack.beginBlock();

    // Live MIDI from the instrument, dispatched to the hosted plugins by
    // channel. This runs whether or not the transport is playing, so the
    // Digitakt's MIDI tracks can drive the plugins on their own.
    if (externalMidi.load())
    {
        juce::MidiBuffer incoming;
        externalCollector.removeNextBlockOfMessages (incoming, numSamples);

        const int base = externalBase.load();
        for (const auto meta : incoming)
        {
            const auto m = meta.getMessage();
            const int channel = m.getChannel();
            if (channel <= 0) continue;                  // not a channel message

            const int slot = channel - base;
            if (slot < 0 || slot >= numMidiTracks) continue;

            rack.addMessage (slot, m, juce::jlimit (0, numSamples - 1,
                                                    meta.samplePosition));
            externalRouted[slot].fetch_add (1);
        }
    }

    if (pendingStart.exchange (false) && cfg.enabled && cfg.sendTransport)
    {
        midiOut.addEvent (juce::MidiMessage::midiStart(), 0);
        samplesUntilNextClock = 0.0;
    }
    if (pendingStop.exchange (false))
    {
        allNotesOff (0);
        if (cfg.enabled && cfg.sendTransport)
            midiOut.addEvent (juce::MidiMessage::midiStop(), 0);
    }

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
                fireStep (stepIndex.load(), offset);
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

        // Muting the sample tracks takes effect at once, including on voices
        // already sounding. Letting them ring would put a tail into whatever
        // the Digitakt is recording.
        if (localAudio.load())
        {
            for (auto& v : voices) renderVoice (v);
            renderVoice (auditionVoice);
        }

        // MIDI clock runs on its own subdivision of the step grid.
        if (playing.load() && cfg.enabled && cfg.sendClock)
        {
            double remaining = (double) block;
            double at = (double) offset;
            while (samplesUntilNextClock <= remaining)
            {
                at += samplesUntilNextClock;
                remaining -= samplesUntilNextClock;
                midiOut.addEvent (juce::MidiMessage::midiClock(),
                                  juce::jlimit (0, numSamples - 1, (int) at));
                samplesUntilNextClock = samplesPerClock;
            }
            samplesUntilNextClock -= remaining;
        }

        // Release remote notes whose gate has run out.
        for (int t = 0; t < numAudioTracks; ++t)
        {
            if (activeMidiNote[t] < 0 || midiGateLeft[t] < 0.0) continue;
            midiGateLeft[t] -= (double) block;
            if (midiGateLeft[t] <= 0.0)
                emitNoteOff (t, juce::jmin (numSamples - 1, offset + block - 1));
        }

        // Same for the notes held by the hosted instruments.
        for (int t = 0; t < numMidiTracks; ++t)
        {
            if (pluginNote[t] < 0 || pluginGateLeft[t] < 0.0) continue;
            pluginGateLeft[t] -= (double) block;
            if (pluginGateLeft[t] <= 0.0)
                emitPluginNoteOff (t, juce::jmin (numSamples - 1, offset + block - 1));
        }

        if (playing.load())
        {
            samplesUntilNextStep -= (double) block;
            if (samplesUntilNextStep <= 0.0)
            {
                advanceStep (juce::jmin (numSamples - 1, offset + block));
                samplesUntilNextStep += samplesPerStep;
            }
        }
        offset += block;
    }

    // Plugins run once for the whole callback, after every note event for the
    // block has been queued at its correct sample offset.
    rack.renderAndMix (outputChannelData, numOutputChannels, numSamples,
                       pluginAudio.load() ? masterGain : 0.0f);

    if (! midiOut.isEmpty())
    {
        const juce::SpinLock::ScopedTryLockType sl (remoteLock);
        if (sl.isLocked() && sink != nullptr)
            sink->sendBlock (midiOut, sampleRate);
    }
}
