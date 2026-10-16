/*
    DigitaktCore.h - Elektron Digitakt pattern storage format.

    A C++ port of the format layer of elk-herd (Mark Lentczner, BSD 2-Clause),
    by way of the Python port in this project.

    Deliberately free of any JUCE dependency: this compiles and unit-tests on
    its own, which is what makes it possible to prove byte-for-byte agreement
    with the Python implementation. The JUCE layer sits on top and only ever
    calls into this.

    The safety property that matters: a pattern dump is ~27 KB and this code
    models about 2% of it. Every other byte is captured verbatim in a skip
    buffer and written back untouched. All ten Digitakt storage versions round
    trip byte-for-byte, and that is enforced by the tests.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dtk
{

using Bytes = std::vector<uint8_t>;

struct DigitaktError : std::runtime_error
{
    explicit DigitaktError (const std::string& what) : std::runtime_error (what) {}
};

//==============================================================================
enum class Device { digitakt, digitakt2 };

const char* productName (Device d) noexcept;
int         dumpCode    (Device d) noexcept;   // SysEx byte: 0x0A / 0x14
int         numSteps    (Device d) noexcept;   // 64 / 128
int         numSampleSlots (Device d) noexcept;

struct Version
{
    Device device { Device::digitakt };
    int    value  { 0 };

    bool operator== (const Version& o) const noexcept
        { return device == o.device && value == o.value; }
    std::string toString() const;
};

//==============================================================================
/** Seven-bit clean encoding used inside Elektron SysEx messages.

    Each group of 7 bytes is preceded by one byte carrying their high bits:
        0abcdefg 0aaaaaaa 0bbbbbbb ... 0ggggggg
    A short final group simply omits the missing bytes.
*/
Bytes sevenBitEncode (const Bytes& data);
Bytes sevenBitDecode (const uint8_t* data, size_t size);

//==============================================================================
/** A cursor over a byte buffer. `skipTo` offsets are measured from the start
    of the innermost struct, which is what StructScope tracks.
*/
class Reader
{
public:
    explicit Reader (const Bytes& d) : data (d) {}

    uint8_t  u8();
    uint16_t u16();
    uint32_t u32();
    Bytes    raw (size_t n);
    std::vector<uint16_t> u16Array (size_t n);
    Bytes    u8Array (size_t n)          { return raw (n); }

    /** Consume bytes up to `offset` from the current struct's base. */
    Bytes skipTo (size_t offset);

    uint32_t peekU32() const noexcept;
    bool     atEnd()   const noexcept { return pos >= data.size(); }
    size_t   position() const noexcept { return pos; }
    size_t   remaining() const noexcept { return data.size() - pos; }

    /** RAII: rebases skipTo for a nested struct, restoring the parent's base. */
    class StructScope
    {
    public:
        explicit StructScope (Reader& r) : reader (r), saved (r.mark)
            { reader.mark = reader.pos; }
        ~StructScope() { reader.mark = saved; }
        StructScope (const StructScope&) = delete;
        StructScope& operator= (const StructScope&) = delete;
    private:
        Reader& reader;
        size_t  saved;
    };

private:
    void need (size_t n) const;

    const Bytes& data;
    size_t pos  { 0 };
    size_t mark { 0 };
};

class Writer
{
public:
    Writer& u8  (uint32_t v);
    Writer& u16 (uint32_t v);
    Writer& u32 (uint32_t v);
    Writer& raw (const Bytes& b);
    Writer& u16Array (const std::vector<uint16_t>& v);
    Writer& u8Array  (const Bytes& v);

    const Bytes& bytes() const noexcept { return buf; }
    size_t size() const noexcept { return buf.size(); }

private:
    Bytes buf;
};

//==============================================================================
/** Field offsets and sizes of Elektron's C++ storage structures, ported from
    elk-herd's CppStructs.elm. Those numbers were generated from Elektron's
    private headers; they are the one part of this code that cannot be
    re-derived from the source alone. std::nullopt means "no such field in this
    version".
*/
namespace offsets
{
    std::optional<size_t> patternStorage_pLocksIndex (Version);
    std::optional<size_t> patternStorage_kitIndex    (Version);
    std::optional<size_t> patternStorage_sizeof      (Version);
    std::optional<size_t> trackStorage_soundSlotLocks (Version);
    std::optional<size_t> trackStorage_sizeof         (Version);
    std::optional<size_t> kitStorage_trackSounds (Version);
    std::optional<size_t> kitStorage_midiParams  (Version);
    std::optional<size_t> kitStorage_midiMask    (Version);
    std::optional<size_t> kitStorage_sizeof      (Version);
    std::optional<size_t> midiSetupStorage_enableMask (Version);
    std::optional<size_t> midiSetupStorage_sizeof     (Version);
    std::optional<size_t> soundStorage_sampleSlot (Version);
    std::optional<size_t> soundStorage_sampleFile (Version);
    std::optional<size_t> soundStorage_sizeof     (Version);
    std::optional<size_t> soundParameters_sampleParamId (Version);
    std::optional<size_t> projectSettingsStorage_sampleList (Version);
    std::optional<size_t> projectSettingsStorage_sizeof     (Version);

    struct SubStruct { int childVersion; int count; };
    std::optional<SubStruct> patternToTracks   (Version);
    std::optional<SubStruct> kitToSounds       (Version);
    std::optional<SubStruct> kitToMidiSetups   (Version);
}

//==============================================================================
constexpr uint8_t  noLock     = 0xFF;
constexpr uint16_t stepTrigBit = 0x0001;
constexpr uint32_t magicHead  = 0xBEEFBACE;
constexpr uint32_t inodeInvalid = 0xFFFFFFFF;
constexpr int      numPLocks  = 80;

/** Names of the per-step lock arrays, in storage order.

    Each track holds parallel 64-byte arrays, one entry per step, immediately
    after the step-flag words; 0xFF means "not locked, use the track default".
    elk-herd models only the sample-slot array. The note and length arrays were
    identified by diffing the shipped blank patterns across storage versions:
    the array at +0 defaults to 0x3C (60) and the one at +128 to 0x0E (14),
    matching the track default block which begins 3c 64 0e = note 60,
    velocity 100, length 14. Velocity sits between them by field order but is
    0xFF in every blank, so it carries less certainty - verify it with the
    Probe view before relying on it.
*/
struct LockMap
{
    std::map<std::string, size_t> arrays;   // name -> offset from skip1 start
    bool has (const std::string& n) const { return arrays.count (n) > 0; }
};

const LockMap* lockMapFor (Version trackVersion);

//==============================================================================
struct Track
{
    Version version;
    std::vector<uint16_t> steps;     // numSteps x uint16be flag words
    Bytes   skip1;                   // the per-step lock arrays
    Bytes   soundPLocks;             // may be empty on old versions
    Bytes   skip2;                   // per-track defaults, then reserved

    bool trig (int step) const  { return (steps[(size_t) step] & stepTrigBit) != 0; }
    void setTrig (int step, bool on);
    bool anyTrigs() const;

    std::optional<int> lock (const std::string& name, int step) const;
    void setLock (const std::string& name, int step, std::optional<int> value);

    std::optional<int> soundPLock (int step) const;
    void setSoundPLock (int step, std::optional<int> slot);

    int defaultNote()     const { return skip2.empty()     ? 60  : skip2[0]; }
    int defaultVelocity() const { return skip2.size() < 2 ? 100 : skip2[1]; }

    static Track parse (Reader&, Version);
    void build (Writer&) const;
};

struct PLock
{
    Version version;
    int paramId { 0xFF };
    int track   { 0 };
    std::vector<uint16_t> steps;

    bool active() const noexcept { return paramId != 0xFF; }

    static PLock parse (Reader&, Version);
    void build (Writer&) const;
};

struct Pattern
{
    Version version;
    std::vector<Track> tracks;
    Bytes  skip1;
    std::vector<PLock> plocks;
    Bytes  nameRaw;
    Bytes  skip2;          // pattern settings: tempo, length, and unknowns
    int    kitIndex { 0 };
    Bytes  skip3;

    std::string name() const;
    void setName (const std::string&);

    /* Confirmed against every shipped blank: tempo in 1/120 BPM units at
       skip2[2..3], length in steps at skip2[4..5]. */
    double tempo() const;
    void   setTempo (double bpm);
    int    length() const;
    void   setLength (int steps);

    static Pattern parse (Reader&, Device);
    void build (Writer&) const;
};

struct Sample
{
    uint32_t inode { inodeInvalid }, hash { 0 }, filesize { 0 }, seqnr { 0 };
    bool empty() const noexcept { return inode == inodeInvalid; }

    /** Binds a project sample slot to a file on the +Drive.

        Pool entries reference samples by content hash and size, not by path,
        so the link survives moving the file around the drive - and even
        deleting and re-uploading it. inode and seqnr are left unknown; the
        instrument recomputes them when the project loads. */
    static Sample fromDriveFile (uint32_t hash, uint32_t size)
        { return { 0, hash, size, 0 }; }

    static Sample parse (Reader&);
    void build (Writer&) const;
};

struct Sound
{
    uint32_t magic { magicHead };
    Version  version;
    uint32_t tagMask { 0 };
    Bytes    nameRaw;
    Bytes    skip1;
    int      sampleSlot { 0 };
    Bytes    skip2;
    Sample   sample;
    Bytes    skip3;

    std::string name() const;
    void setName (const std::string&);

    static Sound parse (Reader&, Version);
    void build (Writer&) const;
};

struct MidiSetup
{
    std::optional<uint32_t> magic;
    Version  version;
    Bytes    skip1;
    uint16_t enableMask { 0 };
    Bytes    skip2;

    bool enabled() const noexcept { return (enableMask & 0x0001) != 0; }

    static MidiSetup parse (Reader&, Version);
    void build (Writer&) const;
};

struct Kit
{
    std::optional<uint32_t> magic;
    Version version;
    Bytes   nameRaw;
    Bytes   skip1;
    std::vector<Sound> sounds;
    Bytes   skip2;
    std::vector<MidiSetup> midiSetups;
    Bytes   skip3;
    std::optional<uint16_t> midiMask;
    Bytes   skip4;

    std::string name() const;
    void setName (const std::string&);
    bool isDefault() const;

    static Kit parse (Reader&, Version);
    void build (Writer&) const;
};

/** Sample-slot parameter locks.

    This is the Digitakt's actual "sample sequencer" mechanism. A track plays
    one sample from the kit, but any step can override which +Drive sample
    slot fires, via a parameter-lock lane in the pattern.

    Lanes live in `Pattern::plocks` (80 of them). A lane is claimed by setting
    its paramId to the sound version's sample parameter id and its track to the
    audio track. An unused lane has paramId 0xFF.

    On Digitakt I the per-step value packs the slot into the high byte, with
    0xFF00 meaning "no lock". Digitakt II uses the full 16 bits and 0xFFFF.
*/
struct SampleLockLane
{
    static std::optional<int> sampleParamId (Version kitVersion);

    /** The lane locking sample slots for `track`, or nullptr if there isn't one. */
    static const PLock* find (const struct PatternKit&, int track);
    static PLock*       find (struct PatternKit&, int track);

    /** Finds the lane, or claims a free one. Returns nullptr if all 80 are used. */
    static PLock* findOrCreate (struct PatternKit&, int track);

    static std::optional<int> get (const struct PatternKit&, int track, int step);

    /** Sets or clears one step's sample lock. Clearing the last lock in a lane
        releases the lane back to the free pool. */
    static bool set (struct PatternKit&, int track, int step,
                     std::optional<int> slot);

    static void clearTrack (struct PatternKit&, int track);
    static int  countLocks (const struct PatternKit&, int track);
    static int  lanesInUse (const struct PatternKit&);
};

/** A whole pattern: the sequence plus its kit. This is what a slot dump holds. */
struct PatternKit
{
    Pattern pattern;
    Kit     kit;

    std::string name() const { return pattern.name(); }
    void setName (const std::string& s);   // device shows the pattern's copy

    /** Is this an audio track rather than a MIDI track? */
    bool isAudioTrack (int t) const;

    static PatternKit parse (Reader&, Device);
    static PatternKit fromBytes (const Bytes&, Device = Device::digitakt);
    void  build (Writer&) const;
    Bytes toBytes() const;
};

struct ProjectSettings
{
    Version version;
    Bytes   skip1;
    std::vector<Sample> samples;
    Bytes   skip2;

    static ProjectSettings parse (Reader&, Device);
    void build (Writer&) const;
};

//==============================================================================
/** An Elektron SysEx dump message.

        F0 00 20 3C <dumpCode> 00 <type> 01 01 <index>
           <7-bit payload> <checksum:14be> <count:14be> F7

    The checksum is a plain sum of the encoded payload truncated to 14 bits and
    count is its length plus 5, both outside the 7-bit region. Get either wrong
    and the Digitakt ignores the message in complete silence - no error on the
    display, nothing. That silence is the main reason to use this rather than
    hand-rolling a builder.
*/
struct ElkDump
{
    enum Type
    {
        patternKitResponse = 0x50, patternKitRequest = 0x60,
        patternResponse    = 0x51, patternRequest    = 0x61,
        kitResponse        = 0x52, kitRequest        = 0x62,
        soundResponse      = 0x53, soundRequest      = 0x63,
        projectResponse    = 0x54, projectRequest    = 0x64,
        wholeProjectRequest = 0x6F
    };

    Device device { Device::digitakt };
    int    type   { patternKitResponse };
    int    index  { 0 };
    Bytes  content;                       // decoded 8-bit payload

    std::optional<PatternKit>      patternKit;
    std::optional<ProjectSettings> projectSettings;
    std::string parseError;               // empty when content decoded cleanly

    std::string typeName() const;
    bool isRequest() const noexcept;

    static ElkDump parse (Reader&);
    Bytes toBytes() const;

    /** Re-encode the decoded structure back into `content`. Call after edits. */
    void refreshContent();
};

std::vector<ElkDump> parseSysExFile (const Bytes& data);
Bytes buildSysExFile (const std::vector<ElkDump>& dumps);
ElkDump makePatternKitDump (const PatternKit&, int index,
                            Device = Device::digitakt);

//==============================================================================
/** Blank (initialised) patterns, one per storage version, from elk-herd.
    A blank pattern contains hundreds of undecoded fields, so the only way to
    make one is to have a real one on hand.
*/
Bytes            blankData (const std::string& kind, int version);
std::vector<int> blankVersions (const std::string& kind);
PatternKit       blankPatternKit (int storageVersion = 9);

constexpr int latestDT1PatternVersion = 9;

//==============================================================================
/** Which storage structure versions a given instrument build writes.

    Ported from elk-herd's `digitaktVersions` table. The lookup is by *build*
    string ("0095"), not the human-readable OS version ("1.52"), because the
    build is a zero-padded number that compares correctly and the version
    string's format is not guaranteed.

    A device can load older structures but always writes these, so this is what
    decides which blank to start from when making a new pattern. */
struct StorageVersions
{
    int projectSettings { 0 };
    int patternAndKit { 0 };
};

/** The last table entry not exceeding `build`, or nullopt if the build is
    older than anything known. A build newer than the table returns the newest
    entry - see `buildIsNewerThanKnown`. */
std::optional<StorageVersions> storageVersionsForBuild (Device, const std::string& build);

/** True when the instrument is newer than this table knows about, in which
    case the versions returned are a best guess rather than a fact. */
bool buildIsNewerThanKnown (Device, const std::string& build);

/** The human-readable OS version for a known build, e.g. "0095" -> "1.52". */
std::string osVersionForBuild (Device, const std::string& build);

//==============================================================================
/** Editing helpers. All of these leave untouched bytes untouched. */
struct Step
{
    bool trig { false };
    std::optional<int> note, velocity, length;
    std::optional<int> sampleSlot;   // sound-pool lock (per track)
    std::optional<int> sampleLock;   // sample-slot p-lock (the sample sequencer)
};

class PatternEditor
{
public:
    explicit PatternEditor (PatternKit& pk) : kit (pk) {}

    PatternKit& patternKit() noexcept { return kit; }
    const PatternKit& patternKit() const noexcept { return kit; }

    int numTracks() const { return (int) kit.pattern.tracks.size(); }
    int stepCount() const { return numSteps (kit.pattern.version.device); }

    std::string name() const              { return kit.name(); }
    void setName (const std::string& s)   { kit.setName (s); }
    double tempo() const                  { return kit.pattern.tempo(); }
    void setTempo (double b)              { kit.pattern.setTempo (b); }
    int  length() const                   { return kit.pattern.length(); }
    void setLength (int s)                { kit.pattern.setLength (s); }

    Step get (int track, int step) const;

    /** Per-step sample-slot lock: the sample sequencer. Returns false when all
        80 parameter-lock lanes are already claimed. */
    bool setSampleLock (int track, int step, std::optional<int> slot);
    std::optional<int> sampleLock (int track, int step) const;
    void setTrig (int track, int step, bool on);
    void setNote (int track, int step, std::optional<int>);
    void setVelocity (int track, int step, std::optional<int>);
    void setStepLength (int track, int step, std::optional<int>);
    void setSoundPoolLock (int track, int step, std::optional<int>);

    void clearTrack (int track);
    void clearAll();

    bool isAudioTrack (int t) const { return kit.isAudioTrack (t); }
    int  sampleSlot (int t) const;
    void setSampleSlot (int t, int slot);

private:
    PatternKit& kit;
};

/** Track labels: eight audio tracks then eight MIDI tracks. */
std::string trackLabel (int index);
std::string noteName (int midiNote);

//==============================================================================
/** Byte-level diff, labelled by structure and field. This is how the lock map
    was worked out and how you verify or extend it: dump a pattern, change one
    thing on the device, dump again, diff.
*/
struct Region { size_t start, end; std::string label; };
struct ByteDiff { size_t offset; uint8_t before, after; std::string label; };

std::vector<Region>   patternKitRegions (const PatternKit&);
std::string           labelFor (const std::vector<Region>&, size_t offset);
std::vector<ByteDiff> diffBytes (const Bytes& a, const Bytes& b,
                                 const std::vector<Region>& regions,
                                 size_t maxReport = 2000);

} // namespace dtk
