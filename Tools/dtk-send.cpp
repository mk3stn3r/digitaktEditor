/*
    dtk-send - a minimal command-line sender, for isolating transfer problems.

    If the GUI leaves the Digitakt sitting at WAITING DATA, run this. It uses
    the same raw MIDI path the app now uses, prints exactly how many bytes the
    driver accepted, and has no GUI, no audio and no sequencer in the way.

        dtk-send --list
        dtk-send --port hw:2,0,0 pattern.syx
        dtk-send --port hw:2,0,0 --verify pattern.syx
*/

#include "RawMidiOut.h"
#include "DigitaktCore.h"

#include <cstdio>
#include <fstream>
#include <string>

static dtk::Bytes readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    return dtk::Bytes ((std::istreambuf_iterator<char> (f)),
                        std::istreambuf_iterator<char>());
}

int main (int argc, char** argv)
{
    std::string port, file;
    bool listOnly = false, verify = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--list") listOnly = true;
        else if (a == "--verify") verify = true;
        else if (a == "--port" && i + 1 < argc) port = argv[++i];
        else file = a;
    }

    if (listOnly || (port.empty() && file.empty()))
    {
        const auto ports = RawMidiOut::list();
        printf ("raw MIDI output ports (%d):\n", (int) ports.size());
        for (const auto& p : ports)
            printf ("  %-14s %s  [%s]\n", p.id.c_str(), p.name.c_str(), p.card.c_str());

        RawMidiOut::Port dt;
        if (RawMidiOut::findDigitakt (dt))
            printf ("\nlooks like a Digitakt: %s\n", dt.id.c_str());
        else if (! ports.empty())
            printf ("\nno port name mentions Digitakt or Elektron; pass --port yourself\n");
        else
            printf ("\nnothing found. Is USB CONFIG set to USB MIDI rather than Overbridge?\n");
        return 0;
    }

    if (file.empty()) { fprintf (stderr, "no .syx file given\n"); return 2; }

    const auto data = readFile (file);
    if (data.empty()) { fprintf (stderr, "could not read %s\n", file.c_str()); return 2; }
    printf ("%s: %d bytes\n", file.c_str(), (int) data.size());

    // Parsing first turns "the machine ignored it" into a concrete reason.
    try
    {
        const auto dumps = dtk::parseSysExFile (data);
        printf ("parsed %d SysEx message(s):\n", (int) dumps.size());
        for (const auto& d : dumps)
        {
            printf ("  %-12s index=%-3d payload=%d bytes%s\n",
                    d.typeName().c_str(), d.index, (int) d.content.size(),
                    d.parseError.empty() ? "" : "  PARSE ERROR");
            if (! d.parseError.empty())
                printf ("      %s\n", d.parseError.c_str());
            if (d.patternKit)
                printf ("      storage version %d - the instrument will reject a "
                        "version its OS does not know\n",
                        d.patternKit->pattern.version.value);
        }
    }
    catch (const std::exception& e)
    {
        printf ("  NOT a valid Elektron dump: %s\n", e.what());
        printf ("  Sending it anyway would be pointless; stopping.\n");
        return 3;
    }

    if (verify) return 0;

    if (port.empty())
    {
        RawMidiOut::Port dt;
        if (! RawMidiOut::findDigitakt (dt))
        {
            fprintf (stderr, "no port given and none auto-detected; use --list\n");
            return 2;
        }
        port = dt.id;
        printf ("auto-selected %s\n", port.c_str());
    }

    printf ("\nOn the Digitakt, before continuing:\n"
            "  SETTINGS > SYSTEM > USB CONFIG      = USB MIDI (not Overbridge)\n"
            "  SETTINGS > MIDI CONFIG > PORT CONFIG > INPUT FROM = MIDI+USB\n"
            "  SETTINGS > SYSEX DUMP > SYSEX RECEIVE > PATTERN > (slot) > YES\n"
            "The screen must read WAITING DATA. Press Enter to send.\n");
    getchar();

    RawMidiOut out;
    juce::String error;
    if (! out.open (port, error))
    {
        fprintf (stderr, "open failed: %s\n", error.toRawUTF8());
        return 4;
    }

    if (! out.write (data.data(), data.size(), error))
    {
        fprintf (stderr, "send failed: %s\n", error.toRawUTF8());
        return 5;
    }

    printf ("all %d bytes accepted by the driver and flushed.\n", (int) data.size());
    printf ("If the Digitakt still shows WAITING DATA, the bytes left this\n"
            "machine but the instrument did not take them - INPUT FROM is the\n"
            "usual reason, and it is saved per project.\n");
    return 0;
}
