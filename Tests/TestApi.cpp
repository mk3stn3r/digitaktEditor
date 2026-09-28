/*  Tests for the Elektron API layer (sample upload / +Drive operations).

    The device obviously is not here, so the transfer tests run against a mock
    that implements the protocol the way the instrument is documented to: it
    validates every request, tracks the file handle, assembles the chunks, and
    returns the same responses. That catches state-machine bugs, off-by-one
    chunking and offset accounting, which are exactly the things that would
    otherwise only show up as a silently corrupt sample on the +Drive.
*/

#include "../Source/ElektronApi.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>

using namespace dtk;
using namespace dtk::api;

static int failures = 0, checks = 0;

static void check (bool ok, const std::string& what)
{
    ++checks;
    if (ok) std::cout << "  PASS  " << what << "\n";
    else  { std::cout << "  FAIL  " << what << "\n"; ++failures; }
}

//==============================================================================
/** A mock +Drive that speaks the protocol. */
class MockDevice
{
public:
    Bytes respond (const Bytes& requestSysEx)
    {
        // Decode the request the same way the instrument would.
        const Bytes encoded (requestSysEx.begin() + 6, requestSysEx.end() - 1);
        const auto inner = sevenBitDecode (encoded.data(), encoded.size());
        const uint16_t msgId = (uint16_t) ((inner[0] << 8) | inner[1]);

        size_t p = 4;
        auto u8  = [&] { return inner[p++]; };
        auto u32 = [&] { uint32_t v = ((uint32_t) inner[p] << 24) | ((uint32_t) inner[p+1] << 16)
                                    | ((uint32_t) inner[p+2] << 8) | (uint32_t) inner[p+3];
                         p += 4; return v; };
        auto str = [&] { std::string s; while (inner[p] != 0) s += (char) inner[p++]; ++p; return s; };

        const uint8_t id = u8();
        Bytes body;
        auto putU8  = [&] (uint32_t v) { body.push_back ((uint8_t) v); };
        auto putU32 = [&] (uint32_t v) { body.push_back ((uint8_t) (v >> 24));
                                         body.push_back ((uint8_t) (v >> 16));
                                         body.push_back ((uint8_t) (v >> 8));
                                         body.push_back ((uint8_t) v); };

        switch (id)
        {
            case fileWriteOpen:
            {
                const uint32_t len = u32();
                const std::string path = str();
                ++openCount;
                lastPath = path;
                expectedLength = len;
                writeBuffer.clear();
                nextFd = 0x42;
                putU8 (fileWriteOpen | responseFlag);
                putU8 (1);              // ok
                putU32 (nextFd);
                break;
            }
            case fileWrite:
            {
                const uint32_t fd = u32();
                const uint32_t chunkLen = u32();
                const uint32_t start = u32();
                if (fd != nextFd) { badFd = true; }
                if (start != writeBuffer.size()) { badOffset = true; }
                if (inner.size() - p != chunkLen) { badChunkLen = true; }
                writeBuffer.insert (writeBuffer.end(), inner.begin() + (long) p, inner.end());
                maxChunkSeen = std::max (maxChunkSeen, chunkLen);
                ++chunkCount;
                putU8 (fileWrite | responseFlag);
                putU8 (1);
                putU32 (start);          // device echoes the offset written at
                break;
            }
            case fileWriteClose:
            {
                const uint32_t fd = u32();
                const uint32_t len = u32();
                if (fd != nextFd) badFd = true;
                closedLength = len;
                putU8 (fileWriteClose | responseFlag);
                putU8 (len == writeBuffer.size() ? 1 : 0);
                putU32 (fd);
                putU32 ((uint32_t) writeBuffer.size());
                break;
            }
            case fileReadOpen:
            {
                lastPath = str();
                nextFd = 0x77;
                putU8 (fileReadOpen | responseFlag);
                putU8 (1);
                putU32 (nextFd);
                putU32 ((uint32_t) fileOnDrive.size());
                break;
            }
            case fileRead:
            {
                const uint32_t fd = u32();
                const uint32_t len = u32();
                const uint32_t start = u32();
                if (fd != nextFd) badFd = true;
                putU8 (fileRead | responseFlag);
                putU8 (1);
                putU32 (fd);
                putU32 (len);
                putU32 (start);
                putU32 (start + len);
                body.insert (body.end(), fileOnDrive.begin() + (long) start,
                             fileOnDrive.begin() + (long) (start + len));
                break;
            }
            case fileReadClose:
            {
                const uint32_t fd = u32();
                putU8 (fileReadClose | responseFlag);
                putU32 (fd);
                putU32 ((uint32_t) fileOnDrive.size());
                break;
            }
            case dirList:
            {
                lastPath = str();
                putU8 (dirList | responseFlag);
                auto entry = [&] (uint32_t h, uint32_t sz, bool lk, char t, const char* n)
                {
                    putU32 (h); putU32 (sz); putU8 (lk ? 1 : 0); putU8 ((uint32_t) t);
                    for (const char* c = n; *c; ++c) putU8 ((uint32_t) *c);
                    putU8 (0);
                };
                entry (0xAABBCCDD, 4096, false, 'F', "kick");
                entry (0x11223344, 8192, true,  'F', "snare locked");
                entry (0, 0, false, 'D', "drums");
                break;
            }
            case device:
            {
                putU8 (device | responseFlag);
                putU8 (12);              // Digitakt product id
                putU8 (3);
                putU8 (0x10); putU8 (0x40); putU8 (0x42);
                for (const char* c = "Digitakt"; *c; ++c) putU8 ((uint32_t) *c);
                putU8 (0);
                break;
            }
            default:
                putU8 (id | responseFlag);
                putU8 (0);
                break;
        }
        return wrap (body, 0, msgId);
    }

    Bytes writeBuffer, fileOnDrive;
    std::string lastPath;
    uint32_t nextFd = 0, expectedLength = 0, closedLength = 0, maxChunkSeen = 0;
    int openCount = 0, chunkCount = 0;
    bool badFd = false, badOffset = false, badChunkLen = false;
};

//==============================================================================
static bool runTransfer (Transfer& t, MockDevice& dev, int maxSteps = 100000)
{
    for (int i = 0; i < maxSteps; ++i)
    {
        const auto req = t.nextRequest();
        if (req.empty()) break;
        const auto reply = dev.respond (req);
        const auto msg = parse (reply);
        if (! t.handleResponse (msg)) return false;
        if (t.finished()) break;
    }
    return t.state() == Transfer::State::done;
}

int main()
{
    // --- envelope ----------------------------------------------------------
    {
        const auto w = wrap (request::deviceInfo(), 7);
        check (w.front() == 0xF0 && w.back() == 0xF7, "API message is framed F0..F7");
        bool clean = true;
        for (size_t i = 1; i + 1 < w.size(); ++i) if (w[i] & 0x80) clean = false;
        check (clean, "API message body is 7-bit clean");
        check (isApiMessage (w), "recognised as an API message");
        check (w[4] == 0x10, "carries the Elektron API type byte, not a dump code");
    }

    // --- responses ---------------------------------------------------------
    {
        MockDevice dev;
        const auto m = parse (dev.respond (wrap (request::deviceInfo(), 1)));
        check (m.isResponse && m.productId == 12 && m.text == "Digitakt",
               "device response decodes product id and name");
        check (m.supportedMessages.size() == 3,
               "device response decodes the supported message list");
        check (m.respId == 1, "response echoes the request id, so replies can be matched");
    }

    {
        MockDevice dev;
        const auto m = parse (dev.respond (wrap (request::listDirectory ("/"), 2)));
        check (m.entries.size() == 3, "directory listing decodes three entries");
        check (m.entries[0].name == "kick" && m.entries[0].hash == 0xAABBCCDD
               && m.entries[0].size == 4096 && ! m.entries[0].isDirectory(),
               "file entry carries name, hash and size");
        check (m.entries[1].locked, "locked flag decodes");
        check (m.entries[2].isDirectory() && m.entries[2].name == "drums",
               "directory entry decodes");
        check (dev.lastPath == "/", "path string round-trips to the device");
    }

    // --- sample file format ------------------------------------------------
    {
        std::vector<float> mono (1000);
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] = std::sin ((float) i * 0.05f);

        const auto file = buildSampleFile (mono);
        check (file.size() == 64 + 2000, "sample file is a 64-byte header plus 16-bit PCM");
        check (file[0] == 0, "header type byte is zero");

        auto be32 = [&] (size_t o) { return ((uint32_t) file[o] << 24) | ((uint32_t) file[o+1] << 16)
                                          | ((uint32_t) file[o+2] << 8) | (uint32_t) file[o+3]; };
        check (be32 (4) == 2000, "header records the data length in bytes");
        check (be32 (8) == 48000, "header records the 48 kHz sample rate");
        check (be32 (12) == 0 && be32 (16) == 999,
               "header records first and last sample indices");
        check (file[20] == 0x7F, "loop type is 'no loop'");

        // Check a known conversion: full scale and silence.
        const auto loud = buildSampleFile ({ 1.0f, -1.0f, 0.0f });
        const int16_t s0 = (int16_t) ((loud[64] << 8) | loud[65]);
        const int16_t s1 = (int16_t) ((loud[66] << 8) | loud[67]);
        const int16_t s2 = (int16_t) ((loud[68] << 8) | loud[69]);
        check (s0 == 32767 && s1 == -32768 && s2 == 0,
               "float to 16-bit conversion hits both rails exactly");
    }

    {
        check (sanitiseSampleName ("/home/me/Kick Drum.wav") == "Kick Drum",
               "sample name strips directory and extension");
        check (sanitiseSampleName ("loop.FLAC") == "loop", "uppercase extensions too");
        check (sanitiseSampleName ("") == "sample", "empty name gets a fallback");
        check (sanitiseSampleName (std::string (200, 'x')).size() == 63,
               "over-long names are truncated");
    }

    // --- upload against the mock -------------------------------------------
    {
        std::vector<float> mono (30000);
        std::mt19937 rng (7);
        for (auto& f : mono) f = (float) ((int) (rng() % 2000) - 1000) / 1000.0f;
        const auto file = buildSampleFile (mono);

        MockDevice dev;
        Transfer t;
        t.startUpload ("/kick", file);
        const bool ok = runTransfer (t, dev);

        check (ok, "upload completes");
        check (! dev.badFd, "every chunk used the file handle the device issued");
        check (! dev.badOffset, "chunk offsets are contiguous with no gaps or overlap");
        check (! dev.badChunkLen, "declared chunk length matches the bytes sent");
        check (dev.writeBuffer == file, "the device received the file byte-for-byte");
        check (dev.maxChunkSeen <= Transfer::chunkSize, "no chunk exceeds 8 KB");
        check (dev.chunkCount == (int) ((file.size() + Transfer::chunkSize - 1)
                                        / Transfer::chunkSize),
               "chunk count matches the file size");
        check (dev.lastPath == "/kick", "the file landed at the requested path");
        check (t.progress() == 1.0, "progress reaches 1.0");
    }

    // --- a file that is an exact multiple of the chunk size -----------------
    {
        MockDevice dev;
        Transfer t;
        Bytes exact (Transfer::chunkSize * 3, 0xAB);
        t.startUpload ("/exact", exact);
        check (runTransfer (t, dev) && dev.writeBuffer == exact && dev.chunkCount == 3,
               "a file that is an exact multiple of the chunk size transfers correctly");
    }

    // --- a file smaller than one chunk -------------------------------------
    {
        MockDevice dev;
        Transfer t;
        Bytes tiny (100, 0x5A);
        t.startUpload ("/tiny", tiny);
        check (runTransfer (t, dev) && dev.writeBuffer == tiny && dev.chunkCount == 1,
               "a file smaller than one chunk transfers in a single chunk");
    }

    // --- download ----------------------------------------------------------
    {
        MockDevice dev;
        dev.fileOnDrive.resize (20000);
        for (size_t i = 0; i < dev.fileOnDrive.size(); ++i)
            dev.fileOnDrive[i] = (uint8_t) (i * 7);

        Transfer t;
        t.startDownload ("/kick");
        check (runTransfer (t, dev) && t.result() == dev.fileOnDrive,
               "download reassembles the file byte-for-byte");
    }

    // --- failure is reported, not swallowed --------------------------------
    {
        Transfer t;
        t.startUpload ("/full", Bytes (1000, 0));

        Message refuse;
        refuse.id = fileWriteOpen | responseFlag;
        refuse.isResponse = true;
        refuse.ok = false;

        check (! t.handleResponse (refuse) && t.failed(),
               "a refused open fails the transfer");
        check (t.errorMessage().find ("+Drive") != std::string::npos,
               "the error message says what to check");
    }

    {
        MockDevice dev;
        Transfer t;
        t.startUpload ("/x", Bytes (20000, 0));
        t.handleResponse (parse (dev.respond (t.nextRequest())));   // open

        Message wrongOffset;
        wrongOffset.id = fileWrite | responseFlag;
        wrongOffset.isResponse = true;
        wrongOffset.ok = true;
        wrongOffset.totalLen = 999999;

        check (! t.handleResponse (wrongOffset) && t.failed(),
               "a write acknowledging the wrong offset fails rather than corrupting");
    }

    // --- decoding a sample read back off the drive -------------------------
    {
        std::vector<float> mono (5000);
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] = std::sin ((float) i * 0.02f) * 0.8f;
        const auto encoded = buildSampleFile (mono);

        SampleFileInfo info;
        std::string err;
        check (parseSampleFile (encoded, info, err), "a built sample file parses back");
        check (info.sampleRate == 48000 && info.samples.size() == mono.size(),
               "rate and length survive the round trip");
        check (! info.looping(), "loop type reads back as no-loop");
        check (std::abs (info.seconds() - 5000.0 / 48000.0) < 1e-6,
               "duration is computed from the header rate");

        float worst = 0.0f;
        for (size_t i = 0; i < mono.size(); ++i)
            worst = std::max (worst, std::abs (info.samples[i] - mono[i]));
        check (worst < 1.0f / 32000.0f,
               "sample values survive to within one 16-bit step");
    }

    // --- the instrument may return a padded file ---------------------------
    {
        auto padded = buildSampleFile (std::vector<float> (100, 0.25f));
        padded.insert (padded.end(), 16, 0xAA);   // old firmware pads the waveform

        SampleFileInfo info;
        std::string err;
        check (parseSampleFile (padded, info, err) && info.samples.size() == 100,
               "trailing pad bytes are ignored, header length wins");
    }

    // --- malformed input is rejected, not misread --------------------------
    {
        SampleFileInfo info;
        std::string err;
        check (! parseSampleFile (Bytes (10, 0), info, err),
               "a file shorter than the header is rejected");

        auto badType = buildSampleFile (std::vector<float> (10, 0.0f));
        badType[0] = 9;
        check (! parseSampleFile (badType, info, err) && err.find ("type") != std::string::npos,
               "an unknown sample type is rejected by name");

        auto badRate = buildSampleFile (std::vector<float> (10, 0.0f));
        badRate[8] = badRate[9] = badRate[10] = badRate[11] = 0;
        check (! parseSampleFile (badRate, info, err),
               "a zero sample rate is rejected");

        auto headerOnly = buildSampleFile ({});
        check (! parseSampleFile (headerOnly, info, err),
               "a file with no audio data is rejected");
    }

    // --- upload then download the same file --------------------------------
    {
        std::vector<float> mono (12345);
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] = std::cos ((float) i * 0.011f) * 0.6f;
        const auto original = buildSampleFile (mono);

        MockDevice dev;
        Transfer up;
        up.startUpload ("/round/trip", original);
        check (runTransfer (up, dev), "upload for the round trip completes");

        // What the device stored is what a download should hand back.
        dev.fileOnDrive = dev.writeBuffer;
        Transfer down;
        down.startDownload ("/round/trip");
        check (runTransfer (down, dev), "download of the same file completes");
        check (down.result() == original,
               "the file survives upload and download byte-for-byte");

        SampleFileInfo info;
        std::string err;
        check (parseSampleFile (down.result(), info, err)
               && info.samples.size() == mono.size(),
               "and decodes back to the original audio length");
    }

    // --- a download that is an exact multiple of the chunk size ------------
    {
        MockDevice dev;
        dev.fileOnDrive.assign (Transfer::chunkSize * 2, 0x33);
        Transfer t;
        t.startDownload ("/exact");
        check (runTransfer (t, dev) && t.result() == dev.fileOnDrive,
               "a download that is an exact chunk multiple reassembles correctly");
    }

    std::cout << "\n" << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
