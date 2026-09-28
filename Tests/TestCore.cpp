/*  Standalone tests for DigitaktCore - no JUCE required.

    The critical tests are the round-trip ones and the cross-check against the
    Python implementation: a pattern dump is ~27 KB of which this code models
    about 2%, so byte-exactness is the whole contract.

    Build:  g++ -std=c++17 -O2 -o test_core Tests/TestCore.cpp \
                Source/DigitaktCore.cpp Source/BlankData.cpp -ISource
*/

#include "../Source/DigitaktCore.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>

using namespace dtk;

static int failures = 0, checks = 0;

static void check (bool ok, const std::string& what)
{
    ++checks;
    if (ok) { std::cout << "  PASS  " << what << "\n"; }
    else    { std::cout << "  FAIL  " << what << "\n"; ++failures; }
}

static Bytes readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    return Bytes ((std::istreambuf_iterator<char> (f)),
                   std::istreambuf_iterator<char>());
}

static void writeFile (const std::string& path, const Bytes& b)
{
    std::ofstream f (path, std::ios::binary);
    f.write ((const char*) b.data(), (long) b.size());
}

int main (int argc, char** argv)
{
    const std::string outDir = argc > 1 ? argv[1] : "/tmp";

    // --- seven bit codec ---------------------------------------------------
    {
        std::mt19937 rng (12345);
        bool ok = true, clean = true;
        for (size_t n : { 0,1,2,3,6,7,8,9,13,14,15,127,128,1000,27648 })
        {
            Bytes d (n);
            for (auto& b : d) b = (uint8_t) (rng() & 0xFF);
            const auto enc = sevenBitEncode (d);
            for (auto b : enc) if (b & 0x80) clean = false;
            if (sevenBitDecode (enc.data(), enc.size()) != d) ok = false;
        }
        check (ok, "seven-bit codec is an exact inverse at every length");
        check (clean, "seven-bit output has the high bit clear");

        const Bytes src { 0x80, 0x01, 0xFF, 0, 0, 0, 0 };
        const auto e = sevenBitEncode (src);
        check (e[0] == 0b1010000 && e[1] == 0x00 && e[2] == 0x01 && e[3] == 0x7F,
               "seven-bit known vector");
    }

    // --- storage round trip ------------------------------------------------
    {
        bool ok = true;
        for (int v : blankVersions ("blankPatternKit"))
        {
            const auto raw = blankData ("blankPatternKit", v);
            const auto pk  = PatternKit::fromBytes (raw, Device::digitakt);
            if (pk.toBytes() != raw)
            {
                std::cout << "        version " << v << " did not round-trip\n";
                ok = false;
            }
        }
        check (ok, "all 10 blank patternKit versions round-trip byte-exactly");

        bool psOk = true;
        for (int v : blankVersions ("blankProjectSettings"))
        {
            const auto raw = blankData ("blankProjectSettings", v);
            Reader r (raw);
            const auto ps = ProjectSettings::parse (r, Device::digitakt);
            Writer w; ps.build (w);
            if (w.bytes() != raw) psOk = false;
        }
        check (psOk, "all blank projectSettings round-trip byte-exactly");
    }

    // --- defaults ----------------------------------------------------------
    {
        bool ok = true;
        for (int v : blankVersions ("blankPatternKit"))
        {
            const auto pk = blankPatternKit (v);
            if (pk.pattern.tempo() != 120.0) ok = false;
            if (pk.pattern.length() != 16) ok = false;
            if (pk.pattern.tracks.size() != 16) ok = false;
            if (pk.kit.sounds.size() != 8) ok = false;
            for (const auto& t : pk.pattern.tracks) if (t.anyTrigs()) ok = false;
        }
        check (ok, "blank patterns read 120 BPM / 16 steps / 16 tracks / no trigs");
    }

    // --- sysex envelope ----------------------------------------------------
    {
        auto pk = blankPatternKit (9);
        const auto wire = makePatternKitDump (pk, 5).toBytes();
        bool clean = wire.front() == 0xF0 && wire.back() == 0xF7;
        for (size_t i = 1; i + 1 < wire.size(); ++i) if (wire[i] & 0x80) clean = false;
        check (clean, "built dump is framed F0..F7 and 7-bit clean");

        const auto back = parseSysExFile (wire);
        check (back.size() == 1 && back[0].index == 5
               && back[0].parseError.empty()
               && back[0].patternKit->toBytes() == pk.toBytes()
               && back[0].toBytes() == wire,
               "sysex message round-trips exactly");

        auto bad = wire; bad[100] ^= 0x01;
        bool caught = false;
        try { parseSysExFile (bad); }
        catch (const DigitaktError&) { caught = true; }
        check (caught, "corrupted payload is rejected by the checksum");

        std::vector<ElkDump> two { makePatternKitDump (pk, 0), makePatternKitDump (pk, 1) };
        const auto file = buildSysExFile (two);
        const auto got  = parseSysExFile (file);
        check (got.size() == 2 && got[0].index == 0 && got[1].index == 1,
               "multiple messages in one file");
    }

    // --- editing -----------------------------------------------------------
    {
        auto pk = blankPatternKit (9);
        PatternEditor ed (pk);
        ed.setTrig (0, 0, true);
        ed.setNote (0, 0, 60);
        ed.setVelocity (0, 0, 100);
        ed.setStepLength (0, 0, 4);
        const auto s = ed.get (0, 0);
        check (s.trig && s.note == 60 && s.velocity == 100 && s.length == 4,
               "trig and per-step locks read back");

        ed.setNote (0, 0, std::nullopt);
        check (! ed.get (0, 0).note.has_value(), "clearing a lock works");

        ed.setTempo (174.0); ed.setLength (64); ed.setName ("ACID");
        check (ed.tempo() == 174.0 && ed.length() == 64 && ed.name() == "ACID",
               "pattern settings round-trip in memory");

        const auto wire = makePatternKitDump (pk, 0).toBytes();
        auto back = parseSysExFile (wire);
        PatternEditor ed2 (*back[0].patternKit);
        check (ed2.tempo() == 174.0 && ed2.length() == 64 && ed2.name() == "ACID",
               "pattern settings survive the wire");

        ed.clearTrack (0);
        const auto c = ed.get (0, 0);
        check (! c.trig && ! c.note && ! c.velocity, "clearTrack removes trigs and locks");
    }

    // --- edits touch only what they should ---------------------------------
    {
        const auto before = blankData ("blankPatternKit", 9);
        auto pk = PatternKit::fromBytes (before, Device::digitakt);
        PatternEditor ed (pk);
        ed.setTrig (2, 7, true);
        ed.setNote (2, 7, 64);
        const auto after = pk.toBytes();

        const auto regions = patternKitRegions (pk);
        const auto diffs   = diffBytes (before, after, regions);
        bool only = ! diffs.empty();
        for (const auto& d : diffs)
        {
            const auto base = d.label.substr (0, d.label.rfind ('['));
            if (base != "track[2].steps" && base != "track[2].lock:note") only = false;
        }
        check (only, "a note edit changes only track[2].steps and track[2].lock:note");
    }

    // --- dump requests: asking the device to send ---------------------------
    {
        // A request is the same envelope with an empty payload. Get a byte
        // wrong and the Digitakt ignores it in silence, so pin the exact bytes.
        ElkDump req;
        req.device = Device::digitakt;
        req.type   = ElkDump::patternKitRequest;
        req.index  = 5;
        const auto wire = req.toBytes();

        const Bytes expected {
            0xF0, 0x00, 0x20, 0x3C, 0x0A, 0x00,
            0x60,                    // patternKit request
            0x01, 0x01,
            0x05,                    // slot
            0x00, 0x00,              // checksum of an empty payload
            0x00, 0x05,              // count = 0 + 5
            0xF7 };
        check (wire == expected, "a pattern request is exactly 15 known bytes");

        const auto back = parseSysExFile (wire);
        check (back.size() == 1 && back[0].isRequest()
               && back[0].type == ElkDump::patternKitRequest
               && back[0].index == 5,
               "a request parses back as a request for the right slot");
        check (back[0].content.empty(), "a request carries no payload");
    }

    {
        ElkDump whole;
        whole.device = Device::digitakt;
        whole.type   = ElkDump::wholeProjectRequest;
        whole.index  = 0;
        const auto wire = whole.toBytes();
        check (wire[6] == 0x6F && wire[9] == 0x00,
               "a whole-project request uses type 0x6F with slot 0");

        ElkDump settings;
        settings.device = Device::digitakt;
        settings.type   = ElkDump::projectRequest;
        const auto w2 = settings.toBytes();
        check (w2[6] == 0x64, "a project settings request uses type 0x64");
        check (parseSysExFile (w2)[0].isRequest(), "and is recognised as a request");
    }

    {
        // Requests and responses must not be confused for one another: a
        // request has no structure to decode and must not report a parse error.
        ElkDump req;
        req.device = Device::digitakt;
        req.type   = ElkDump::patternRequest;
        const auto parsed = parseSysExFile (req.toBytes());
        check (parsed[0].parseError.empty() && ! parsed[0].patternKit.has_value(),
               "a request is not mistaken for a malformed response");
    }

    // --- sample-slot p-locks: the sample sequencer -------------------------
    {
        auto pk = blankPatternKit (9);
        PatternEditor ed (pk);

        check (SampleLockLane::sampleParamId (pk.kit.version) == 20,
               "sample param id for kit v9 is 20 (sound storage v2)");
        check (SampleLockLane::lanesInUse (pk) == 0, "blank pattern has no p-lock lanes");

        // A different sample on every fourth step of track 1.
        bool ok = true;
        const int slots[] = { 3, 17, 42, 8 };
        for (int i = 0; i < 4; ++i)
            ok &= ed.setSampleLock (0, i * 4, slots[i]);
        check (ok, "setting sample locks succeeds");
        check (SampleLockLane::lanesInUse (pk) == 1,
               "four locks on one track claim exactly one lane");

        bool readBack = true;
        for (int i = 0; i < 4; ++i)
            if (ed.sampleLock (0, i * 4) != slots[i]) readBack = false;
        check (readBack, "sample locks read back at the right steps");
        check (! ed.sampleLock (0, 1).has_value(), "unlocked steps stay unlocked");
        check (SampleLockLane::countLocks (pk, 0) == 4, "lock count is right");

        // Survives the wire.
        const auto wire = makePatternKitDump (pk, 0).toBytes();
        auto back = parseSysExFile (wire);
        check (back[0].parseError.empty(), "pattern with sample locks still parses");
        PatternEditor ed2 (*back[0].patternKit);
        bool survived = true;
        for (int i = 0; i < 4; ++i)
            if (ed2.sampleLock (0, i * 4) != slots[i]) survived = false;
        check (survived, "sample locks survive encode and decode");

        // Clearing the last lock hands the lane back.
        for (int i = 0; i < 4; ++i) ed.setSampleLock (0, i * 4, std::nullopt);
        check (SampleLockLane::lanesInUse (pk) == 0,
               "clearing every lock releases the lane");

        // MIDI tracks have no samples, so they must be refused.
        check (! ed.setSampleLock (8, 0, 5) || ! ed.sampleLock (8, 0).has_value(),
               "MIDI tracks reject sample locks");

        // The 80-lane budget is real: one lane per audio track, so 8 max here.
        auto pk2 = blankPatternKit (9);
        PatternEditor ed3 (pk2);
        bool allTracks = true;
        for (int t = 0; t < 8; ++t) allTracks &= ed3.setSampleLock (t, t, t + 1);
        check (allTracks && SampleLockLane::lanesInUse (pk2) == 8,
               "eight audio tracks claim eight lanes");
    }

    // --- editing sample locks leaves the rest of the file alone ------------
    {
        const auto before = blankData ("blankPatternKit", 9);
        auto pk = PatternKit::fromBytes (before, Device::digitakt);
        PatternEditor ed (pk);
        ed.setSampleLock (1, 5, 33);
        const auto after = pk.toBytes();

        const auto regions = patternKitRegions (pk);
        const auto diffs = diffBytes (before, after, regions);
        bool onlyPlock = ! diffs.empty();
        for (const auto& d : diffs)
            if (d.label.rfind ("plock[", 0) != 0) onlyPlock = false;
        check (onlyPlock, "a sample lock changes only p-lock bytes");
    }

    // --- binding a pool slot to a +Drive file ------------------------------
    {
        const auto s = Sample::fromDriveFile (0xDEADBEEF, 12345);
        check (s.hash == 0xDEADBEEF && s.filesize == 12345
               && s.inode == 0 && s.seqnr == 0 && ! s.empty(),
               "pool slot binds to a drive file by hash and size");
    }

    // --- cross-check artefacts for the Python comparison -------------------
    {
        auto pk = blankPatternKit (9);
        PatternEditor ed (pk);
        ed.setName ("XCHECK");
        ed.setTempo (138.0);
        ed.setLength (16);
        for (int s = 0; s < 16; s += 4) { ed.setTrig (0, s, true); ed.setVelocity (0, s, 110); }
        for (int s : { 2, 6, 10, 14 }) { ed.setTrig (2, s, true); ed.setNote (2, s, 42); }
        ed.setSampleSlot (0, 17);
        writeFile (outDir + "/cpp_xcheck.syx", buildSysExFile ({ makePatternKitDump (pk, 3) }));
        std::cout << "\n  wrote " << outDir << "/cpp_xcheck.syx for the Python cross-check\n";
    }

    std::cout << "\n" << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
