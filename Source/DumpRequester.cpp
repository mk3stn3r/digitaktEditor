#include "DumpRequester.h"

void DumpRequester::start (What w, int slot, int messageType)
{
    if (busy) return;
    if (output == nullptr) { fail ("No MIDI output is open."); return; }

    what = w;
    requestedSlot = slot;
    collected.clear();
    sawAnything = false;
    busy = true;

    dtk::ElkDump req;
    req.device = dtk::Device::digitakt;
    req.type   = messageType;
    req.index  = slot;
    // A request is the same envelope with an empty payload.
    const auto wire = req.toBytes();

    output->sendMessageNow (juce::MidiMessage::createSysExMessage (
        wire.data() + 1, (int) wire.size() - 2));

    startTimer (firstReplyTimeoutMs);
}

void DumpRequester::requestPattern (int slot)
{
    start (What::pattern, slot, dtk::ElkDump::patternKitRequest);
}

void DumpRequester::requestProjectSettings()
{
    start (What::projectSettings, 0, dtk::ElkDump::projectRequest);
}

void DumpRequester::requestWholeProject()
{
    start (What::wholeProject, 0, dtk::ElkDump::wholeProjectRequest);
}

void DumpRequester::cancel()
{
    if (! busy) return;
    stopTimer();
    busy = false;
    collected.clear();
    if (listener != nullptr) listener->dumpsFailed ("Cancelled.");
}

void DumpRequester::finish()
{
    stopTimer();
    busy = false;
    auto result = std::move (collected);
    collected.clear();
    if (listener != nullptr) listener->dumpsComplete (std::move (result));
}

void DumpRequester::fail (const juce::String& why)
{
    stopTimer();
    busy = false;
    collected.clear();
    if (listener != nullptr) listener->dumpsFailed (why);
}

void DumpRequester::timerCallback()
{
    stopTimer();
    if (! busy) return;

    if (! sawAnything)
    {
        fail ("The Digitakt did not reply to the dump request.\n\n"
              "Check SETTINGS > SYSTEM > USB CONFIG is USB MIDI rather than "
              "Overbridge, and that SETTINGS > MIDI CONFIG > PORT CONFIG has "
              "both INPUT FROM and OUTPUT TO including USB. That port setting "
              "is saved per project, and a new project defaults to MIDI only.");
        return;
    }

    // A whole-project transfer that went quiet: keep whatever arrived rather
    // than throwing away a nearly complete download.
    finish();
}

void DumpRequester::handleSysEx (const dtk::Bytes& sysex)
{
    if (! busy) return;
    if (sysex.size() < 6 || sysex[4] == 0x10) return;   // an API message, not a dump

    dtk::ElkDump d;
    try
    {
        dtk::Reader r (sysex);
        d = dtk::ElkDump::parse (r);
    }
    catch (const std::exception&)
    {
        return;   // not a dump we can read; ignore rather than abort
    }

    if (d.isRequest()) return;    // our own message echoed back by a MIDI loop

    sawAnything = true;
    collected.push_back (d);

    if (listener != nullptr)
        listener->dumpsArriving ((int) collected.size(),
                                 juce::String (d.typeName()) + " "
                                 + juce::String (d.index));

    switch (what)
    {
        case What::pattern:
            if (d.type == dtk::ElkDump::patternKitResponse) { finish(); return; }
            break;

        case What::projectSettings:
            if (d.type == dtk::ElkDump::projectResponse) { finish(); return; }
            break;

        case What::wholeProject:
            // Project settings come last; that is the only end marker there is.
            if (d.type == dtk::ElkDump::projectResponse) { finish(); return; }
            break;
    }

    startTimer (idleTimeoutMs);
}
