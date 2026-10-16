#include "RawMidiOut.h"

#if JUCE_LINUX || JUCE_BSD
 #include <alsa/asoundlib.h>
 #define DTK_HAVE_RAWMIDI 1
#else
 #define DTK_HAVE_RAWMIDI 0
#endif

bool RawMidiOut::available()
{
    return DTK_HAVE_RAWMIDI != 0;
}

#if DTK_HAVE_RAWMIDI

std::vector<RawMidiOut::Port> RawMidiOut::list()
{
    std::vector<Port> ports;

    int card = -1;
    while (snd_card_next (&card) >= 0 && card >= 0)
    {
        const std::string ctlName = "hw:" + std::to_string (card);

        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open (&ctl, ctlName.c_str(), 0) < 0) continue;

        std::string cardName;
        {
            char* n = nullptr;
            if (snd_card_get_name (card, &n) >= 0 && n != nullptr)
            {
                cardName = n;
                free (n);
            }
        }

        int device = -1;
        while (snd_ctl_rawmidi_next_device (ctl, &device) >= 0 && device >= 0)
        {
            snd_rawmidi_info_t* info = nullptr;
            snd_rawmidi_info_alloca (&info);
            snd_rawmidi_info_set_device (info, (unsigned int) device);
            snd_rawmidi_info_set_stream (info, SND_RAWMIDI_STREAM_OUTPUT);
            snd_rawmidi_info_set_subdevice (info, 0);

            if (snd_ctl_rawmidi_info (ctl, info) < 0) continue;

            const unsigned int subs = snd_rawmidi_info_get_subdevices_count (info);
            for (unsigned int sub = 0; sub < subs; ++sub)
            {
                snd_rawmidi_info_set_subdevice (info, sub);
                if (snd_ctl_rawmidi_info (ctl, info) < 0) continue;

                Port p;
                p.id = "hw:" + std::to_string (card) + ","
                     + std::to_string (device) + "," + std::to_string (sub);

                const char* subName = snd_rawmidi_info_get_subdevice_name (info);
                const char* devName = snd_rawmidi_info_get_name (info);
                p.name = (subName != nullptr && *subName != 0) ? subName
                       : (devName != nullptr ? devName : p.id);
                p.card = cardName;
                ports.push_back (p);
            }
        }
        snd_ctl_close (ctl);
    }
    return ports;
}

bool RawMidiOut::open (const std::string& id, juce::String& errorOut)
{
    close();

    snd_rawmidi_t* out = nullptr;

    // APPEND matters: JUCE's MIDI output holds this same hardware port open
    // through the ALSA *sequencer*, and a plain open of an already-open output
    // device waits for it to be released - which never happens, so the caller
    // hangs. NONBLOCK guarantees the open itself returns immediately.
    int err = snd_rawmidi_open (nullptr, &out, id.c_str(),
                                SND_RAWMIDI_APPEND | SND_RAWMIDI_NONBLOCK);

    if (err == -EINVAL)
    {
        // Some drivers reject APPEND; a non-blocking open alone still cannot
        // hang, so fall back to that rather than giving up.
        err = snd_rawmidi_open (nullptr, &out, id.c_str(), SND_RAWMIDI_NONBLOCK);
    }

    if (err < 0)
    {
        errorOut = juce::String ("could not open ") + id + ": " + snd_strerror (err);
        if (err == -EBUSY)
            errorOut += " (another program has the port open for output)";
        return false;
    }

    // Writes go back to blocking so a 31 KB dump is delivered whole rather
    // than truncated at the first full buffer. Only the open was non-blocking.
    snd_rawmidi_nonblock (out, 0);

    handle = out;
    currentId = id;
    return true;
}

void RawMidiOut::close()
{
    if (handle != nullptr)
    {
        auto* out = static_cast<snd_rawmidi_t*> (handle);
        snd_rawmidi_drain (out);
        snd_rawmidi_close (out);
        handle = nullptr;
    }
    currentId.clear();
}

bool RawMidiOut::write (const uint8_t* data, size_t size, juce::String& errorOut)
{
    if (handle == nullptr) { errorOut = "no raw MIDI port is open"; return false; }

    auto* out = static_cast<snd_rawmidi_t*> (handle);
    size_t sent = 0;

    while (sent < size)
    {
        const ssize_t n = snd_rawmidi_write (out, data + sent, size - sent);

        if (n == -EAGAIN) continue;          // driver buffer full; keep going
        if (n < 0)
        {
            errorOut = juce::String ("write failed after ") + juce::String ((int) sent)
                     + " of " + juce::String ((int) size) + " bytes: "
                     + snd_strerror ((int) n);
            return false;
        }
        sent += (size_t) n;
    }

    const int err = snd_rawmidi_drain (out);
    if (err < 0)
    {
        errorOut = juce::String ("the port accepted the data but could not flush it: ")
                 + snd_strerror (err);
        return false;
    }
    return true;
}

#else  // not Linux

std::vector<RawMidiOut::Port> RawMidiOut::list() { return {}; }

bool RawMidiOut::open (const std::string&, juce::String& errorOut)
{
    errorOut = "raw MIDI output is only implemented for ALSA (Linux)";
    return false;
}

void RawMidiOut::close() {}

bool RawMidiOut::write (const uint8_t*, size_t, juce::String& errorOut)
{
    errorOut = "raw MIDI output is only implemented for ALSA (Linux)";
    return false;
}

#endif

bool RawMidiOut::findDigitakt (Port& out)
{
    const auto ports = list();
    for (const auto& p : ports)
    {
        const juce::String name = juce::String (p.name) + " " + juce::String (p.card);
        if (name.containsIgnoreCase ("digitakt") || name.containsIgnoreCase ("elektron"))
        {
            out = p;
            return true;
        }
    }
    return false;
}
