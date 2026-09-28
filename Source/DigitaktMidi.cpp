#include "DigitaktMidi.h"

#include <cmath>

juce::String MidiImportResult::summary() const
{
    juce::StringArray lines;
    lines.add ("Placed " + juce::String (placed) + " trig(s) on "
               + juce::String (touchedTracks.size()) + " track(s) at "
               + juce::String (bpm, 1) + " BPM.");

    if (outOfRange > 0)
        lines.add (juce::String (outOfRange) + " note(s) fell outside the pattern "
                   "and were dropped - raise the bar offset to reach later material.");

    if (collisions > 0)
        lines.add (juce::String (collisions) + " note(s) landed on an "
                   "already-used step and were dropped. The sequencer holds one "
                   "trig per step per track, so chords and flams collapse.");

    if (! unmappedChannels.isEmpty())
    {
        juce::StringArray ch;
        for (auto c : unmappedChannels) ch.add (juce::String (c + 1));
        lines.add ("MIDI channel(s) " + ch.joinIntoString (", ")
                   + " were not mapped to a track.");
    }
    return lines.joinIntoString ("\n");
}

MidiImportResult importMidiFile (const juce::File& file,
                                 dtk::PatternEditor& editor,
                                 const MidiImportOptions& options,
                                 juce::String& errorOut)
{
    MidiImportResult result;

    juce::FileInputStream stream (file);
    if (! stream.openedOk())
    {
        errorOut = "could not open " + file.getFileName();
        return result;
    }

    juce::MidiFile midi;
    if (! midi.readFrom (stream))
    {
        errorOut = "not a readable Standard MIDI File";
        return result;
    }

    const short timeFormat = midi.getTimeFormat();
    if (timeFormat <= 0)
    {
        errorOut = "SMPTE time division is not supported; re-export the file "
                   "with metrical (PPQ) timing";
        return result;
    }
    const double ticksPerQuarter = (double) timeFormat;

    // Flatten every track into one sequence of matched note pairs.
    juce::MidiMessageSequence all;
    double tempoUs = 500000.0;

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        auto seq = *midi.getTrack (t);
        seq.updateMatchedPairs();
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& msg = seq.getEventPointer (i)->message;
            if (msg.isTempoMetaEvent())
                tempoUs = msg.getTempoMetaEventTickLength (timeFormat)
                            * ticksPerQuarter * 1.0e6;
            all.addEvent (msg);
        }
    }
    all.updateMatchedPairs();
    all.sort();

    result.bpm = tempoUs > 0.0 ? 60.0e6 / tempoUs : 120.0;

    const double ticksPerStep = ticksPerQuarter / (double) options.stepsPerBeat;
    const double startTick = options.barOffset * options.stepsPerBeat * 4 * ticksPerStep;
    const int stepCount = editor.stepCount();

    // Which tracks are we about to write to? Clear those, and only those.
    std::map<int, int> map = options.channelMap;
    if (map.empty())
        for (int c = 0; c < 16; ++c) map[c] = c;

    std::set<int> targets;
    for (const auto& kv : map)
        if (kv.second >= 0 && kv.second < editor.numTracks())
            targets.insert (kv.second);
    for (int t : targets) editor.clearTrack (t);

    std::set<int> unmapped;
    std::set<int> touched;
    double lastTick = 0.0;

    for (int i = 0; i < all.getNumEvents(); ++i)
    {
        auto* ev = all.getEventPointer (i);
        const auto& msg = ev->message;
        if (! msg.isNoteOn()) continue;

        const int channel = msg.getChannel() - 1;      // JUCE channels are 1-16
        const auto it = map.find (channel);
        if (it == map.end() || it->second < 0 || it->second >= editor.numTracks())
        {
            unmapped.insert (channel);
            continue;
        }
        const int track = it->second;

        const double tick = msg.getTimeStamp();
        const int step = (int) std::lround ((tick - startTick) / ticksPerStep);

        double durationTicks = ticksPerStep;
        if (ev->noteOffObject != nullptr)
            durationTicks = ev->noteOffObject->message.getTimeStamp() - tick;
        lastTick = juce::jmax (lastTick, tick + durationTicks);

        if (step < 0 || step >= stepCount) { ++result.outOfRange; continue; }

        if (editor.get (track, step).trig) { ++result.collisions; continue; }

        editor.setTrig (track, step, true);
        if (options.writeNotes)
            editor.setNote (track, step, msg.getNoteNumber());
        if (options.writeVelocity)
            editor.setVelocity (track, step, (int) msg.getVelocity());

        const int len = juce::jlimit (1, 127,
                                      (int) std::lround (durationTicks / ticksPerStep));
        editor.setStepLength (track, step, len);

        ++result.placed;
        touched.insert (track);
    }

    if (options.setTempo && result.bpm >= 30.0 && result.bpm <= 300.0)
        editor.setTempo (result.bpm);

    const int used = (int) std::lround (lastTick / ticksPerStep);
    if (used > 0) editor.setLength (juce::jlimit (1, stepCount, used));
    result.patternLength = editor.length();

    for (int c : unmapped) result.unmappedChannels.add (c);
    for (int t : touched)  result.touchedTracks.add (t);
    return result;
}
