/*
    DigitaktCore.cpp - implementation of the Digitakt storage format.
    See DigitaktCore.h for the design notes.
*/

#include "DigitaktCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dtk
{

//==============================================================================
const char* productName (Device d) noexcept
{
    return d == Device::digitakt ? "Digitakt" : "Digitakt II";
}

int dumpCode (Device d) noexcept        { return d == Device::digitakt ? 0x0A : 0x14; }
int numSteps (Device d) noexcept        { return d == Device::digitakt ? 64 : 128; }
int numSampleSlots (Device d) noexcept  { return d == Device::digitakt ? 128 : 1024; }

std::string Version::toString() const
{
    return std::string (productName (device)) + " v" + std::to_string (value);
}

//==============================================================================
Bytes sevenBitEncode (const Bytes& data)
{
    Bytes out;
    out.reserve (data.size() + data.size() / 7 + 1);

    for (size_t i = 0; i < data.size(); i += 7)
    {
        const size_t n = std::min<size_t> (7, data.size() - i);

        // High bits are packed MSB-first across a full seven-slot field, so a
        // short final group still shifts as though seven bytes were present.
        uint8_t hi = 0;
        for (size_t j = 0; j < 7; ++j)
            hi = (uint8_t) ((hi << 1) | (j < n ? (data[i + j] >> 7) : 0));

        out.push_back ((uint8_t) (hi & 0x7F));
        for (size_t j = 0; j < n; ++j)
            out.push_back ((uint8_t) (data[i + j] & 0x7F));
    }
    return out;
}

Bytes sevenBitDecode (const uint8_t* data, size_t size)
{
    Bytes out;
    out.reserve (size);

    for (size_t i = 0; i < size; i += 8)
    {
        const size_t n = std::min<size_t> (8, size - i);
        if (n == 0) break;
        const uint8_t hi = data[i];
        for (size_t k = 1; k < n; ++k)
        {
            const uint8_t bit = (uint8_t) ((hi >> (7 - k)) & 1);
            out.push_back ((uint8_t) ((bit << 7) | (data[i + k] & 0x7F)));
        }
    }
    return out;
}

//==============================================================================
void Reader::need (size_t n) const
{
    if (pos + n > data.size())
        throw DigitaktError ("unexpected end of data: wanted "
                             + std::to_string (n) + " bytes at "
                             + std::to_string (pos) + ", have "
                             + std::to_string (data.size() - pos));
}

uint8_t Reader::u8()
{
    need (1);
    return data[pos++];
}

uint16_t Reader::u16()
{
    need (2);
    const uint16_t v = (uint16_t) ((data[pos] << 8) | data[pos + 1]);
    pos += 2;
    return v;
}

uint32_t Reader::u32()
{
    need (4);
    const uint32_t v = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16)
                     | ((uint32_t) data[pos + 2] << 8) | (uint32_t) data[pos + 3];
    pos += 4;
    return v;
}

uint32_t Reader::peekU32() const noexcept
{
    if (pos + 4 > data.size()) return 0;
    return ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16)
         | ((uint32_t) data[pos + 2] << 8) | (uint32_t) data[pos + 3];
}

Bytes Reader::raw (size_t n)
{
    need (n);
    Bytes out (data.begin() + (long) pos, data.begin() + (long) (pos + n));
    pos += n;
    return out;
}

std::vector<uint16_t> Reader::u16Array (size_t n)
{
    need (2 * n);
    std::vector<uint16_t> out (n);
    for (size_t i = 0; i < n; ++i)
        out[i] = (uint16_t) ((data[pos + 2 * i] << 8) | data[pos + 2 * i + 1]);
    pos += 2 * n;
    return out;
}

Bytes Reader::skipTo (size_t offset)
{
    const size_t target = mark + offset;
    if (target < pos)
        throw DigitaktError ("skipTo(" + std::to_string (offset)
                             + ") went backwards: already at "
                             + std::to_string (pos - mark));
    return raw (target - pos);
}

Writer& Writer::u8 (uint32_t v)  { buf.push_back ((uint8_t) (v & 0xFF)); return *this; }

Writer& Writer::u16 (uint32_t v)
{
    buf.push_back ((uint8_t) ((v >> 8) & 0xFF));
    buf.push_back ((uint8_t) (v & 0xFF));
    return *this;
}

Writer& Writer::u32 (uint32_t v)
{
    buf.push_back ((uint8_t) ((v >> 24) & 0xFF));
    buf.push_back ((uint8_t) ((v >> 16) & 0xFF));
    buf.push_back ((uint8_t) ((v >> 8) & 0xFF));
    buf.push_back ((uint8_t) (v & 0xFF));
    return *this;
}

Writer& Writer::raw (const Bytes& b)
{
    buf.insert (buf.end(), b.begin(), b.end());
    return *this;
}

Writer& Writer::u16Array (const std::vector<uint16_t>& v)
{
    for (auto x : v) u16 (x);
    return *this;
}

Writer& Writer::u8Array (const Bytes& v) { return raw (v); }

//==============================================================================
namespace offsets
{

using Key = std::pair<int, int>;   // (device, version)

static Key key (Version v) { return { (int) v.device, v.value }; }

static std::optional<size_t> look (const std::map<Key, size_t>& t, Version v)
{
    auto it = t.find (key (v));
    if (it == t.end()) return std::nullopt;
    return it->second;
}

static const int D1 = (int) Device::digitakt;
static const int D2 = (int) Device::digitakt2;

std::optional<size_t> patternStorage_pLocksIndex (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},12468},{{D1,1},14516},{{D1,2},14516},{{D1,3},14548},{{D1,4},14548},
        {{D1,5},14548},{{D1,6},14548},{{D1,7},14564},{{D1,8},14564},{{D1,9},14580},
        {{D2,0},35316},{{D2,3},68100},{{D2,4},68100} };
    return look (t, v);
}

std::optional<size_t> patternStorage_kitIndex (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},22896},{{D1,1},24944},{{D1,2},24944},{{D1,3},24976},{{D1,4},24976},
        {{D1,5},24976},{{D1,6},24976},{{D1,7},24992},{{D1,8},24992},{{D1,9},25008},
        {{D2,0},55984},{{D2,3},88768},{{D2,4},88768} };
    return look (t, v);
}

std::optional<size_t> patternStorage_sizeof (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},23040},{{D1,1},25088},{{D1,2},25088},{{D1,3},25088},{{D1,4},25088},
        {{D1,5},25088},{{D1,6},25088},{{D1,7},25088},{{D1,8},25088},{{D1,9},25088},
        {{D2,0},56320},{{D2,3},89088},{{D2,4},89088} };
    return look (t, v);
}

std::optional<size_t> trackStorage_soundSlotLocks (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,1},832},{{D1,2},832},{{D1,3},832},{{D1,4},832},{{D1,5},832},
        {{D2,0},1920},{{D2,2},1024} };
    return look (t, v);
}

std::optional<size_t> trackStorage_sizeof (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},779},{{D1,1},907},{{D1,2},909},{{D1,3},909},{{D1,4},910},{{D1,5},911},
        {{D2,0},2207},{{D2,2},1184} };
    return look (t, v);
}

std::optional<size_t> kitStorage_trackSounds (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},36},{{D1,1},36},{{D1,2},36},{{D1,3},36},{{D1,4},36},
        {{D1,5},36},{{D1,6},36},{{D1,7},36},{{D1,8},36},{{D1,9},36},
        {{D2,0},60},{{D2,3},60},{{D2,4},60} };
    return look (t, v);
}

std::optional<size_t> kitStorage_midiParams (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},1424},{{D1,1},1424},{{D1,2},1424},{{D1,3},1424},{{D1,4},1424},
        {{D1,5},1424},{{D1,6},1424},{{D1,7},1424},{{D1,8},1424},{{D1,9},1424},
        {{D2,0},5004},{{D2,3},5676},{{D2,4},17964} };
    return look (t, v);
}

std::optional<size_t> kitStorage_midiMask (Version v)
{
    static const std::map<Key, size_t> t {
        {{D2,0},9296},{{D2,3},9972},{{D2,4},22260} };
    return look (t, v);
}

std::optional<size_t> kitStorage_sizeof (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},2560},{{D1,1},2560},{{D1,2},2560},{{D1,3},2560},{{D1,4},2560},
        {{D1,5},2560},{{D1,6},2560},{{D1,7},2560},{{D1,8},2560},{{D1,9},2560},
        {{D2,0},9728},{{D2,3},10240},{{D2,4},22528} };
    return look (t, v);
}

std::optional<size_t> midiSetupStorage_enableMask (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},100},{{D1,1},100},{{D2,0},256},{{D2,2},256},{{D2,3},256} };
    return look (t, v);
}

std::optional<size_t> midiSetupStorage_sizeof (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},108},{{D1,1},108},{{D2,0},268},{{D2,2},268},{{D2,3},268} };
    return look (t, v);
}

std::optional<size_t> soundStorage_sampleSlot (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},52},{{D1,1},68},{{D1,2},68},
        {{D2,0},100},{{D2,2},100},{{D2,3},100} };
    return look (t, v);
}

std::optional<size_t> soundStorage_sampleFile (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},132},{{D1,1},132},{{D1,2},132},
        {{D2,0},279},{{D2,2},297},{{D2,3},297} };
    return look (t, v);
}

std::optional<size_t> soundStorage_sizeof (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},160},{{D1,1},160},{{D1,2},160},
        {{D2,0},299},{{D2,2},341},{{D2,3},1109} };
    return look (t, v);
}

std::optional<size_t> soundParameters_sampleParamId (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},12},{{D1,1},20},{{D1,2},20},
        {{D2,0},36},{{D2,2},36},{{D2,3},36} };
    return look (t, v);
}

std::optional<size_t> projectSettingsStorage_sampleList (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},69},{{D1,1},70},{{D1,2},70},{{D1,3},70},
        {{D1,4},70},{{D1,5},70},{{D1,6},70},{{D1,7},70},
        {{D2,0},251},{{D2,1},251} };
    return look (t, v);
}

std::optional<size_t> projectSettingsStorage_sizeof (Version v)
{
    static const std::map<Key, size_t> t {
        {{D1,0},2560},{{D1,1},2560},{{D1,2},2560},{{D1,3},2560},
        {{D1,4},2560},{{D1,5},2560},{{D1,6},2560},{{D1,7},2560},
        {{D2,0},16896},{{D2,1},16896} };
    return look (t, v);
}

static std::optional<SubStruct> lookSub (const std::map<Key, SubStruct>& t, Version v)
{
    auto it = t.find (key (v));
    if (it == t.end()) return std::nullopt;
    return it->second;
}

std::optional<SubStruct> patternToTracks (Version v)
{
    static const std::map<Key, SubStruct> t {
        {{D1,0},{0,16}},{{D1,1},{1,16}},{{D1,2},{1,16}},{{D1,3},{2,16}},{{D1,4},{2,16}},
        {{D1,5},{3,16}},{{D1,6},{3,16}},{{D1,7},{4,16}},{{D1,8},{4,16}},{{D1,9},{5,16}},
        {{D2,0},{0,16}},{{D2,3},{2,16}},{{D2,4},{2,16}} };
    return lookSub (t, v);
}

std::optional<SubStruct> kitToSounds (Version v)
{
    static const std::map<Key, SubStruct> t {
        {{D1,0},{0,8}},{{D1,1},{0,8}},{{D1,2},{0,8}},{{D1,3},{0,8}},{{D1,4},{0,8}},
        {{D1,5},{0,8}},{{D1,6},{1,8}},{{D1,7},{1,8}},{{D1,8},{2,8}},{{D1,9},{2,8}},
        {{D2,0},{0,16}},{{D2,3},{2,16}},{{D2,4},{3,16}} };
    return lookSub (t, v);
}

std::optional<SubStruct> kitToMidiSetups (Version v)
{
    static const std::map<Key, SubStruct> t {
        {{D1,0},{0,8}},{{D1,1},{0,8}},{{D1,2},{0,8}},{{D1,3},{0,8}},{{D1,4},{0,8}},
        {{D1,5},{0,8}},{{D1,6},{1,8}},{{D1,7},{1,8}},{{D1,8},{1,8}},{{D1,9},{1,8}},
        {{D2,0},{0,16}},{{D2,3},{2,16}},{{D2,4},{3,16}} };
    return lookSub (t, v);
}

} // namespace offsets

//==============================================================================
const LockMap* lockMapFor (Version v)
{
    // Digitakt I track v1..v5 share one layout. v0 lacks the length array,
    // which shifts everything after it. Digitakt II is left unmapped: it has
    // 128 steps and a different array count that has not been verified, so its
    // tracks round-trip but expose no locks.
    static const LockMap v1plus {{ {"note",0},{"velocity",64},{"length",128},
                                   {"unk_192",192},{"unk_256",256},{"unk_320",320},
                                   {"unk_384",384},{"unk_448",448},{"unk_512",512},
                                   {"unk_576",576},{"unk_640",640} }};
    static const LockMap v0 {{ {"note",0},{"velocity",64},
                               {"unk_128",128},{"unk_192",192},{"unk_256",256},
                               {"unk_320",320},{"unk_384",384},{"unk_448",448},
                               {"unk_512",512},{"unk_576",576} }};

    if (v.device != Device::digitakt) return nullptr;
    if (v.value == 0) return &v0;
    if (v.value >= 1 && v.value <= 5) return &v1plus;
    return nullptr;
}

//==============================================================================
static std::string decodeName (const Bytes& raw)
{
    std::string s;
    for (auto b : raw)
        if (b != 0) s += (char) b;
    return s;
}

static Bytes encodeName (const std::string& s, size_t length = 16)
{
    Bytes out (length, 0);
    for (size_t i = 0; i < std::min (length, s.size()); ++i)
        out[i] = (uint8_t) s[i];
    return out;
}

//==============================================================================
void Track::setTrig (int step, bool on)
{
    auto& s = steps[(size_t) step];
    if (on) s = (uint16_t) (s | stepTrigBit);
    else    s = (uint16_t) (s & ~stepTrigBit);
}

bool Track::anyTrigs() const
{
    for (auto s : steps)
        if (s & stepTrigBit) return true;
    return false;
}

std::optional<int> Track::lock (const std::string& name, int step) const
{
    const auto* map = lockMapFor (version);
    if (map == nullptr || ! map->has (name)) return std::nullopt;
    const size_t off = map->arrays.at (name) + (size_t) step;
    if (off >= skip1.size()) return std::nullopt;
    const uint8_t v = skip1[off];
    return v == noLock ? std::nullopt : std::optional<int> ((int) v);
}

void Track::setLock (const std::string& name, int step, std::optional<int> value)
{
    const auto* map = lockMapFor (version);
    if (map == nullptr || ! map->has (name)) return;
    const size_t off = map->arrays.at (name) + (size_t) step;
    if (off >= skip1.size()) return;
    skip1[off] = value.has_value() ? (uint8_t) (*value & 0xFF) : noLock;
}

std::optional<int> Track::soundPLock (int step) const
{
    if (soundPLocks.empty()) return std::nullopt;
    const uint8_t v = soundPLocks[(size_t) step];
    return v == noLock ? std::nullopt : std::optional<int> ((int) v);
}

void Track::setSoundPLock (int step, std::optional<int> slot)
{
    if (soundPLocks.empty()) return;
    soundPLocks[(size_t) step] = slot.has_value() ? (uint8_t) *slot : noLock;
}

Track Track::parse (Reader& r, Version version)
{
    Reader::StructScope scope (r);
    Track t;
    t.version = version;

    const size_t n = (size_t) numSteps (version.device);
    t.steps = r.u16Array (n);

    const auto slotOff = offsets::trackStorage_soundSlotLocks (version);
    const auto size    = offsets::trackStorage_sizeof (version);
    if (! size)
        throw DigitaktError ("unknown trackStorage size for " + version.toString());

    if (! slotOff)
    {
        t.skip1 = r.skipTo (*size);
        return t;
    }
    t.skip1       = r.skipTo (*slotOff);
    t.soundPLocks = r.u8Array (n);
    t.skip2       = r.skipTo (*size);
    return t;
}

void Track::build (Writer& w) const
{
    w.u16Array (steps).raw (skip1);
    if (! soundPLocks.empty()) w.raw (soundPLocks);
    w.raw (skip2);
}

//==============================================================================
PLock PLock::parse (Reader& r, Version version)
{
    PLock p;
    p.version = version;
    p.paramId = r.u8();
    p.track   = r.u8();
    p.steps   = r.u16Array ((size_t) numSteps (version.device));
    return p;
}

void PLock::build (Writer& w) const
{
    w.u8 ((uint32_t) paramId).u8 ((uint32_t) track).u16Array (steps);
}

//==============================================================================
std::string Pattern::name() const { return decodeName (nameRaw); }
void Pattern::setName (const std::string& s) { nameRaw = encodeName (s); }

double Pattern::tempo() const
{
    if (skip2.size() < 4) return 120.0;
    return (double) ((skip2[2] << 8) | skip2[3]) / 120.0;
}

void Pattern::setTempo (double bpm)
{
    if (skip2.size() < 4) return;
    const int v = (int) std::lround (bpm * 120.0);
    skip2[2] = (uint8_t) ((v >> 8) & 0xFF);
    skip2[3] = (uint8_t) (v & 0xFF);
}

int Pattern::length() const
{
    if (skip2.size() < 6) return 16;
    return (skip2[4] << 8) | skip2[5];
}

void Pattern::setLength (int steps_)
{
    if (skip2.size() < 6) return;
    skip2[4] = (uint8_t) ((steps_ >> 8) & 0xFF);
    skip2[5] = (uint8_t) (steps_ & 0xFF);
}

Pattern Pattern::parse (Reader& r, Device device)
{
    Reader::StructScope scope (r);
    Pattern p;
    p.version = { device, (int) r.u32() };

    const auto sub = offsets::patternToTracks (p.version);
    if (! sub)
        throw DigitaktError ("unsupported pattern storage " + p.version.toString());

    const Version trackVer { device, sub->childVersion };
    for (int i = 0; i < sub->count; ++i)
        p.tracks.push_back (Track::parse (r, trackVer));

    const auto plockOff = offsets::patternStorage_pLocksIndex (p.version);
    const auto kitOff   = offsets::patternStorage_kitIndex (p.version);
    const auto size     = offsets::patternStorage_sizeof (p.version);
    if (! plockOff || ! kitOff || ! size)
        throw DigitaktError ("missing offsets for pattern " + p.version.toString());

    p.skip1 = r.skipTo (*plockOff);

    // PLocks carry no version field of their own; they are always v0.
    const Version plockVer { device, 0 };
    for (int i = 0; i < numPLocks; ++i)
        p.plocks.push_back (PLock::parse (r, plockVer));

    p.nameRaw  = r.raw (16);
    p.skip2    = r.skipTo (*kitOff);
    p.kitIndex = r.u8();
    p.skip3    = r.skipTo (*size);
    return p;
}

void Pattern::build (Writer& w) const
{
    w.u32 ((uint32_t) version.value);
    for (const auto& t : tracks) t.build (w);
    w.raw (skip1);
    for (const auto& p : plocks) p.build (w);
    w.raw (nameRaw).raw (skip2).u8 ((uint32_t) kitIndex).raw (skip3);
}

//==============================================================================
Sample Sample::parse (Reader& r)
{
    Sample s;
    s.inode = r.u32(); s.hash = r.u32(); s.filesize = r.u32(); s.seqnr = r.u32();
    return s;
}

void Sample::build (Writer& w) const
{
    w.u32 (inode).u32 (hash).u32 (filesize).u32 (seqnr);
}

//==============================================================================
std::string Sound::name() const { return decodeName (nameRaw); }
void Sound::setName (const std::string& s) { nameRaw = encodeName (s); }

Sound Sound::parse (Reader& r, Version version)
{
    Reader::StructScope scope (r);
    Sound s;
    s.magic = r.u32();
    if (s.magic != magicHead)
        throw DigitaktError ("bad sound magic");
    const uint32_t got = r.u32();
    if ((int) got != version.value)
        throw DigitaktError ("sound version " + std::to_string (got)
                             + " does not match expected "
                             + std::to_string (version.value));
    s.version = version;
    s.tagMask = r.u32();
    s.nameRaw = r.raw (16);
    s.skip1   = r.skipTo (*offsets::soundStorage_sampleSlot (version));
    s.sampleSlot = numSampleSlots (version.device) > 128 ? (int) r.u16() : (int) r.u8();
    s.skip2   = r.skipTo (*offsets::soundStorage_sampleFile (version));
    s.sample  = Sample::parse (r);
    s.skip3   = r.skipTo (*offsets::soundStorage_sizeof (version));
    return s;
}

void Sound::build (Writer& w) const
{
    w.u32 (magic).u32 ((uint32_t) version.value).u32 (tagMask);
    w.raw (nameRaw).raw (skip1);
    if (numSampleSlots (version.device) > 128) w.u16 ((uint32_t) sampleSlot);
    else                                       w.u8  ((uint32_t) sampleSlot);
    w.raw (skip2);
    sample.build (w);
    w.raw (skip3);
}

//==============================================================================
MidiSetup MidiSetup::parse (Reader& r, Version version)
{
    Reader::StructScope scope (r);
    MidiSetup m;
    if (r.peekU32() == magicHead) m.magic = r.u32();
    const uint32_t got = r.u32();
    if ((int) got != version.value)
        throw DigitaktError ("midiSetup version " + std::to_string (got)
                             + " != expected " + std::to_string (version.value));
    m.version    = version;
    m.skip1      = r.skipTo (*offsets::midiSetupStorage_enableMask (version));
    m.enableMask = r.u16();
    m.skip2      = r.skipTo (*offsets::midiSetupStorage_sizeof (version));
    return m;
}

void MidiSetup::build (Writer& w) const
{
    if (magic) w.u32 (*magic);
    w.u32 ((uint32_t) version.value).raw (skip1).u16 (enableMask).raw (skip2);
}

//==============================================================================
std::string Kit::name() const { return decodeName (nameRaw); }
void Kit::setName (const std::string& s) { nameRaw = encodeName (s); }

Kit Kit::parse (Reader& r, Version version)
{
    Reader::StructScope scope (r);
    Kit k;
    // The magic head exists on DT2 but not DT1, and it sits *before* the
    // version field, so we cannot branch on version yet. A magic value is
    // never a valid version, so peeking is safe.
    if (r.peekU32() == magicHead) k.magic = r.u32();
    const uint32_t got = r.u32();
    if ((int) got != version.value)
        throw DigitaktError ("kit version " + std::to_string (got)
                             + " != expected " + std::to_string (version.value));
    k.version = version;
    k.nameRaw = r.raw (16);
    k.skip1   = r.skipTo (*offsets::kitStorage_trackSounds (version));

    const auto snd = offsets::kitToSounds (version);
    const Version soundVer { version.device, snd->childVersion };
    for (int i = 0; i < snd->count; ++i)
        k.sounds.push_back (Sound::parse (r, soundVer));

    k.skip2 = r.skipTo (*offsets::kitStorage_midiParams (version));
    const auto mid = offsets::kitToMidiSetups (version);
    const Version midiVer { version.device, mid->childVersion };
    for (int i = 0; i < mid->count; ++i)
        k.midiSetups.push_back (MidiSetup::parse (r, midiVer));

    if (const auto maskOff = offsets::kitStorage_midiMask (version))
    {
        k.skip3    = r.skipTo (*maskOff);
        k.midiMask = r.u16();
    }
    k.skip4 = r.skipTo (*offsets::kitStorage_sizeof (version));
    return k;
}

void Kit::build (Writer& w) const
{
    if (magic) w.u32 (*magic);
    w.u32 ((uint32_t) version.value).raw (nameRaw).raw (skip1);
    for (const auto& s : sounds) s.build (w);
    w.raw (skip2);
    for (const auto& m : midiSetups) m.build (w);
    w.raw (skip3);
    if (midiMask) w.u16 (*midiMask);
    w.raw (skip4);
}

bool Kit::isDefault() const
{
    bool ordered = true, allZero = true, allSmall = true;
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        const int s = sounds[i].sampleSlot;
        if (s != (int) i + 1) ordered = false;
        if (s != 0) allZero = false;
        if (s < 0 || s > 8) allSmall = false;
    }
    for (const auto& m : midiSetups)
        if (m.enabled()) return false;
    return ordered || allZero || allSmall;
}

//==============================================================================
// Sample-slot parameter lock lanes - the sample sequencer.

std::optional<int> SampleLockLane::sampleParamId (Version kitVersion)
{
    const auto snd = offsets::kitToSounds (kitVersion);
    if (! snd) return std::nullopt;
    return offsets::soundParameters_sampleParamId (
        Version { kitVersion.device, snd->childVersion });
}

static uint16_t encodeSampleLock (Device device, std::optional<int> slot)
{
    if (device == Device::digitakt)
        return slot.has_value() ? (uint16_t) ((*slot & 0xFF) << 8) : (uint16_t) 0xFF00;
    return slot.has_value() ? (uint16_t) *slot : (uint16_t) 0xFFFF;
}

static std::optional<int> decodeSampleLock (Device device, uint16_t v)
{
    if (device == Device::digitakt)
        return (v & 0xFF00) == 0xFF00 ? std::nullopt : std::optional<int> (v >> 8);
    return v == 0xFFFF ? std::nullopt : std::optional<int> (v);
}

const PLock* SampleLockLane::find (const PatternKit& pk, int track)
{
    const auto paramId = sampleParamId (pk.kit.version);
    if (! paramId || ! pk.isAudioTrack (track)) return nullptr;

    for (const auto& p : pk.pattern.plocks)
        if (p.active() && p.paramId == *paramId && p.track == track)
            return &p;
    return nullptr;
}

PLock* SampleLockLane::find (PatternKit& pk, int track)
{
    return const_cast<PLock*> (find (const_cast<const PatternKit&> (pk), track));
}

PLock* SampleLockLane::findOrCreate (PatternKit& pk, int track)
{
    if (auto* existing = find (pk, track)) return existing;

    const auto paramId = sampleParamId (pk.kit.version);
    if (! paramId || ! pk.isAudioTrack (track)) return nullptr;

    for (auto& p : pk.pattern.plocks)
    {
        if (p.active()) continue;

        // Claim this lane. Every step starts unlocked, or the instrument would
        // read whatever stale values the lane happened to be carrying.
        p.paramId = *paramId;
        p.track   = track;
        const auto blank = encodeSampleLock (pk.pattern.version.device, std::nullopt);
        for (auto& s : p.steps) s = blank;
        return &p;
    }
    return nullptr;   // all 80 lanes claimed
}

std::optional<int> SampleLockLane::get (const PatternKit& pk, int track, int step)
{
    const auto* lane = find (pk, track);
    if (lane == nullptr || step < 0 || step >= (int) lane->steps.size())
        return std::nullopt;
    return decodeSampleLock (pk.pattern.version.device, lane->steps[(size_t) step]);
}

bool SampleLockLane::set (PatternKit& pk, int track, int step,
                          std::optional<int> slot)
{
    const auto device = pk.pattern.version.device;

    if (! slot.has_value())
    {
        auto* lane = find (pk, track);
        if (lane == nullptr) return true;          // nothing to clear
        lane->steps[(size_t) step] = encodeSampleLock (device, std::nullopt);

        // If that was the last lock, hand the lane back so the 80-lane budget
        // is not quietly eaten by empty lanes.
        const auto blank = encodeSampleLock (device, std::nullopt);
        for (auto v : lane->steps)
            if (v != blank) return true;
        lane->paramId = 0xFF;
        lane->track   = 0;
        return true;
    }

    auto* lane = findOrCreate (pk, track);
    if (lane == nullptr) return false;
    if (step < 0 || step >= (int) lane->steps.size()) return false;
    lane->steps[(size_t) step] = encodeSampleLock (device, slot);
    return true;
}

void SampleLockLane::clearTrack (PatternKit& pk, int track)
{
    if (auto* lane = find (pk, track))
    {
        lane->paramId = 0xFF;
        lane->track   = 0;
        const auto blank = encodeSampleLock (pk.pattern.version.device, std::nullopt);
        for (auto& s : lane->steps) s = blank;
    }
}

int SampleLockLane::countLocks (const PatternKit& pk, int track)
{
    const auto* lane = find (pk, track);
    if (lane == nullptr) return 0;
    int n = 0;
    for (auto v : lane->steps)
        if (decodeSampleLock (pk.pattern.version.device, v).has_value()) ++n;
    return n;
}

int SampleLockLane::lanesInUse (const PatternKit& pk)
{
    int n = 0;
    for (const auto& p : pk.pattern.plocks) if (p.active()) ++n;
    return n;
}

//==============================================================================
void PatternKit::setName (const std::string& s)
{
    pattern.setName (s);
    kit.setName (s);
}

bool PatternKit::isAudioTrack (int t) const
{
    if (! kit.midiMask) return t >= 0 && t < 8;
    return t >= 0 && t < 16 && ((*kit.midiMask & (1u << t)) == 0);
}

PatternKit PatternKit::parse (Reader& r, Device device)
{
    PatternKit pk;
    pk.pattern = Pattern::parse (r, device);
    pk.kit     = Kit::parse (r, Version { device, pk.pattern.version.value });
    return pk;
}

PatternKit PatternKit::fromBytes (const Bytes& data, Device device)
{
    Reader r (data);
    return parse (r, device);
}

void PatternKit::build (Writer& w) const
{
    pattern.build (w);
    kit.build (w);
}

Bytes PatternKit::toBytes() const
{
    Writer w;
    build (w);
    return w.bytes();
}

//==============================================================================
ProjectSettings ProjectSettings::parse (Reader& r, Device device)
{
    Reader::StructScope scope (r);
    ProjectSettings p;
    p.version = { device, (int) r.u32() };
    p.skip1 = r.skipTo (*offsets::projectSettingsStorage_sampleList (p.version));
    for (int i = 0; i < numSampleSlots (device); ++i)
        p.samples.push_back (Sample::parse (r));
    p.skip2 = r.skipTo (*offsets::projectSettingsStorage_sizeof (p.version));
    return p;
}

void ProjectSettings::build (Writer& w) const
{
    w.u32 ((uint32_t) version.value).raw (skip1);
    for (const auto& s : samples) s.build (w);
    w.raw (skip2);
}

//==============================================================================
std::string ElkDump::typeName() const
{
    switch (type)
    {
        case patternKitResponse:  return "PatternKit";
        case patternKitRequest:   return "PatternKitRequest";
        case patternResponse:     return "Pattern";
        case patternRequest:      return "PatternRequest";
        case kitResponse:         return "Kit";
        case kitRequest:          return "KitRequest";
        case soundResponse:       return "Sound";
        case soundRequest:        return "SoundRequest";
        case projectResponse:     return "ProjectSettings";
        case projectRequest:      return "ProjectSettingsRequest";
        case wholeProjectRequest: return "WholeProjectRequest";
        default: break;
    }
    return "Unknown(0x" + std::to_string (type) + ")";
}

bool ElkDump::isRequest() const noexcept
{
    return type == patternKitRequest || type == patternRequest
        || type == kitRequest || type == soundRequest
        || type == projectRequest || type == wholeProjectRequest;
}

ElkDump ElkDump::parse (Reader& r)
{
    if (r.u8() != 0xF0)
        throw DigitaktError ("not a SysEx message (no F0)");
    const auto id = r.raw (3);
    if (! (id.size() == 3 && id[0] == 0x00 && id[1] == 0x20 && id[2] == 0x3C))
        throw DigitaktError ("not an Elektron SysEx message");

    ElkDump d;
    const uint8_t code = r.u8();
    if      (code == 0x0A) d.device = Device::digitakt;
    else if (code == 0x14) d.device = Device::digitakt2;
    else throw DigitaktError ("SysEx type is not a Digitakt dump (0x10 would be "
                              "the Elektron API, a separate protocol)");

    if (r.u8() != 0x00) throw DigitaktError ("unexpected device id");
    d.type = r.u8();
    const auto ver = r.raw (2);
    if (! (ver[0] == 0x01 && ver[1] == 0x01))
        throw DigitaktError ("unexpected dump version bytes");
    d.index = r.u8();

    Bytes payload;
    for (;;)
    {
        const uint8_t b = r.u8();
        if (b == 0xF7) break;
        payload.push_back (b);
    }
    if (payload.size() < 4) throw DigitaktError ("truncated dump payload");

    const size_t n = payload.size() - 4;
    const uint32_t checksum = (uint32_t) ((payload[n] << 7) | payload[n + 1]);
    const uint32_t count    = (uint32_t) ((payload[n + 2] << 7) | payload[n + 3]);

    if ((count & 0x3FFF) != ((n + 5) & 0x3FFF))
        throw DigitaktError ("count mismatch: header says " + std::to_string (count)
                             + ", payload implies " + std::to_string (n + 5));

    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += payload[i];
    if ((checksum & 0x3FFF) != (sum & 0x3FFF))
        throw DigitaktError ("checksum mismatch: header says "
                             + std::to_string (checksum) + ", computed "
                             + std::to_string (sum & 0x3FFF));

    d.content = sevenBitDecode (payload.data(), n);

    if (! d.isRequest() && ! d.content.empty())
    {
        try
        {
            Reader cr (d.content);
            if (d.type == patternKitResponse)
                d.patternKit = PatternKit::parse (cr, d.device);
            else if (d.type == projectResponse)
                d.projectSettings = ProjectSettings::parse (cr, d.device);
        }
        catch (const std::exception& e)
        {
            d.parseError = e.what();
        }
    }
    return d;
}

void ElkDump::refreshContent()
{
    if (patternKit)           content = patternKit->toBytes();
    else if (projectSettings) { Writer w; projectSettings->build (w); content = w.bytes(); }
}

Bytes ElkDump::toBytes() const
{
    const Bytes encoded = sevenBitEncode (content);
    Writer w;
    w.u8 (0xF0).u8 (0x00).u8 (0x20).u8 (0x3C).u8 ((uint32_t) dumpCode (device));
    w.u8 (0x00).u8 ((uint32_t) type).u8 (0x01).u8 (0x01).u8 ((uint32_t) index);
    w.raw (encoded);

    uint32_t sum = 0;
    for (auto b : encoded) sum += b;
    const uint32_t count = (uint32_t) encoded.size() + 5;

    w.u8 ((sum >> 7) & 0x7F).u8 (sum & 0x7F);
    w.u8 ((count >> 7) & 0x7F).u8 (count & 0x7F);
    w.u8 (0xF7);
    return w.bytes();
}

std::vector<ElkDump> parseSysExFile (const Bytes& data)
{
    std::vector<ElkDump> out;
    Reader r (data);
    // Tolerate padding between messages, as some librarians add it.
    Bytes scratch = data;
    size_t pos = 0;
    while (pos < scratch.size())
    {
        if (scratch[pos] != 0xF0) { ++pos; continue; }
        Bytes slice (scratch.begin() + (long) pos, scratch.end());
        Reader sr (slice);
        ElkDump d = ElkDump::parse (sr);
        out.push_back (d);
        pos += sr.position();
    }
    return out;
}

Bytes buildSysExFile (const std::vector<ElkDump>& dumps)
{
    Bytes out;
    for (const auto& d : dumps)
    {
        const auto b = d.toBytes();
        out.insert (out.end(), b.begin(), b.end());
    }
    return out;
}

ElkDump makePatternKitDump (const PatternKit& pk, int index, Device device)
{
    ElkDump d;
    d.device     = device;
    d.type       = ElkDump::patternKitResponse;
    d.index      = index;
    d.patternKit = pk;
    d.content    = pk.toBytes();
    return d;
}

//==============================================================================
PatternKit blankPatternKit (int storageVersion)
{
    return PatternKit::fromBytes (blankData ("blankPatternKit", storageVersion),
                                  Device::digitakt);
}

//==============================================================================
Step PatternEditor::get (int track, int step) const
{
    const auto& tr = kit.pattern.tracks[(size_t) track];
    Step s;
    s.trig       = tr.trig (step);
    s.note       = tr.lock ("note", step);
    s.velocity   = tr.lock ("velocity", step);
    s.length     = tr.lock ("length", step);
    s.sampleSlot = tr.soundPLock (step);
    s.sampleLock = SampleLockLane::get (kit, track, step);
    return s;
}

void PatternEditor::setTrig (int track, int step, bool on)
{
    kit.pattern.tracks[(size_t) track].setTrig (step, on);
}

void PatternEditor::setNote (int track, int step, std::optional<int> v)
{
    kit.pattern.tracks[(size_t) track].setLock ("note", step, v);
}

void PatternEditor::setVelocity (int track, int step, std::optional<int> v)
{
    kit.pattern.tracks[(size_t) track].setLock ("velocity", step, v);
}

void PatternEditor::setStepLength (int track, int step, std::optional<int> v)
{
    kit.pattern.tracks[(size_t) track].setLock ("length", step, v);
}

void PatternEditor::setSoundPoolLock (int track, int step, std::optional<int> v)
{
    kit.pattern.tracks[(size_t) track].setSoundPLock (step, v);
}

bool PatternEditor::setSampleLock (int track, int step, std::optional<int> slot)
{
    return SampleLockLane::set (kit, track, step, slot);
}

std::optional<int> PatternEditor::sampleLock (int track, int step) const
{
    return SampleLockLane::get (kit, track, step);
}

void PatternEditor::clearTrack (int track)
{
    auto& tr = kit.pattern.tracks[(size_t) track];
    for (size_t i = 0; i < tr.steps.size(); ++i)
        tr.setTrig ((int) i, false);

    if (const auto* map = lockMapFor (tr.version))
        for (const char* name : { "note", "velocity", "length" })
            if (map->has (name))
                for (int i = 0; i < stepCount(); ++i)
                    tr.setLock (name, i, std::nullopt);

    for (auto& b : tr.soundPLocks) b = noLock;
    SampleLockLane::clearTrack (kit, track);
}

void PatternEditor::clearAll()
{
    for (int t = 0; t < numTracks(); ++t) clearTrack (t);
}

int PatternEditor::sampleSlot (int t) const
{
    if (t < 0 || t >= (int) kit.kit.sounds.size()) return 0;
    return kit.kit.sounds[(size_t) t].sampleSlot;
}

void PatternEditor::setSampleSlot (int t, int slot)
{
    if (t < 0 || t >= (int) kit.kit.sounds.size()) return;
    kit.kit.sounds[(size_t) t].sampleSlot = slot;
}

std::string trackLabel (int index)
{
    if (index < 8)  return "T" + std::to_string (index + 1);
    return "M" + std::to_string (index - 7);
}

std::string noteName (int n)
{
    static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    if (n < 0 || n > 127) return "-";
    return std::string (names[n % 12]) + std::to_string (n / 12 - 1);
}

//==============================================================================
std::vector<Region> patternKitRegions (const PatternKit& pk)
{
    std::vector<Region> regions;
    const auto v      = pk.pattern.version;
    const auto device = v.device;
    const size_t n    = (size_t) numSteps (device);

    regions.push_back ({ 0, 4, "pattern.version" });

    const auto sub = offsets::patternToTracks (v);
    const Version trackVer { device, sub->childVersion };
    const size_t tsize   = *offsets::trackStorage_sizeof (trackVer);
    const auto   slotOff = offsets::trackStorage_soundSlotLocks (trackVer);

    size_t base = 4;
    for (int t = 0; t < sub->count; ++t)
    {
        const std::string tag = "track[" + std::to_string (t) + "]";
        regions.push_back ({ base, base + 2 * n, tag + ".steps" });
        const size_t lockBase = base + 2 * n;

        if (const auto* map = lockMapFor (trackVer))
        {
            for (const auto& kv : map->arrays)
                regions.push_back ({ lockBase + kv.second, lockBase + kv.second + n,
                                     tag + ".lock:" + kv.first });
        }
        else
        {
            regions.push_back ({ lockBase, base + (slotOff ? *slotOff : tsize),
                                 tag + ".locks(unmapped)" });
        }
        if (slotOff)
        {
            regions.push_back ({ base + *slotOff, base + *slotOff + n,
                                 tag + ".lock:sample_slot" });
            regions.push_back ({ base + *slotOff + n, base + tsize, tag + ".defaults" });
        }
        base += tsize;
    }

    const size_t plockOff = *offsets::patternStorage_pLocksIndex (v);
    const size_t kitOff   = *offsets::patternStorage_kitIndex (v);
    const size_t psize    = *offsets::patternStorage_sizeof (v);
    regions.push_back ({ base, plockOff, "pattern.skip1" });

    const size_t plsize = 2 + 2 * n;
    for (int i = 0; i < numPLocks; ++i)
    {
        const size_t s = plockOff + (size_t) i * plsize;
        std::string tag = "plock[" + std::to_string (i) + "]";
        if (pk.pattern.plocks[(size_t) i].active())
            tag += "(param=" + std::to_string (pk.pattern.plocks[(size_t) i].paramId)
                 + ",trk=" + std::to_string (pk.pattern.plocks[(size_t) i].track) + ")";
        regions.push_back ({ s, s + plsize, tag });
    }

    const size_t nameOff = plockOff + (size_t) numPLocks * plsize;
    regions.push_back ({ nameOff,      nameOff + 16, "pattern.name" });
    regions.push_back ({ nameOff + 16, nameOff + 18, "settings.unk0" });
    regions.push_back ({ nameOff + 18, nameOff + 20, "settings.tempo" });
    regions.push_back ({ nameOff + 20, nameOff + 22, "settings.length" });
    regions.push_back ({ nameOff + 22, kitOff,       "settings.unk1" });
    regions.push_back ({ kitOff, kitOff + 1, "pattern.kitIndex" });
    regions.push_back ({ kitOff + 1, psize, "pattern.skip3" });

    const auto kv = pk.kit.version;
    const size_t kbase = psize;
    regions.push_back ({ kbase, kbase + 4, "kit.version" });
    regions.push_back ({ kbase + 4, kbase + 20, "kit.name" });

    const size_t sndOff = *offsets::kitStorage_trackSounds (kv);
    regions.push_back ({ kbase + 20, kbase + sndOff, "kit.skip1" });

    const auto snd = offsets::kitToSounds (kv);
    const size_t ssize = *offsets::soundStorage_sizeof (Version { device, snd->childVersion });
    for (int i = 0; i < snd->count; ++i)
    {
        const size_t s = kbase + sndOff + (size_t) i * ssize;
        regions.push_back ({ s, s + ssize, "kit.sound[" + std::to_string (i) + "]" });
    }

    const size_t midiOff = *offsets::kitStorage_midiParams (kv);
    regions.push_back ({ kbase + sndOff + (size_t) snd->count * ssize,
                         kbase + midiOff, "kit.skip2" });

    const auto mid = offsets::kitToMidiSetups (kv);
    const size_t msize = *offsets::midiSetupStorage_sizeof (Version { device, mid->childVersion });
    for (int i = 0; i < mid->count; ++i)
    {
        const size_t s = kbase + midiOff + (size_t) i * msize;
        regions.push_back ({ s, s + msize, "kit.midiSetup[" + std::to_string (i) + "]" });
    }
    regions.push_back ({ kbase + midiOff + (size_t) mid->count * msize,
                         kbase + *offsets::kitStorage_sizeof (kv), "kit.tail" });
    return regions;
}

std::string labelFor (const std::vector<Region>& regions, size_t off)
{
    for (const auto& r : regions)
        if (off >= r.start && off < r.end)
            return r.label + "[" + std::to_string (off - r.start) + "]";
    return "?";
}

std::vector<ByteDiff> diffBytes (const Bytes& a, const Bytes& b,
                                 const std::vector<Region>& regions,
                                 size_t maxReport)
{
    std::vector<ByteDiff> out;
    const size_t n = std::min (a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (a[i] != b[i])
        {
            out.push_back ({ i, a[i], b[i], labelFor (regions, i) });
            if (out.size() >= maxReport) break;
        }
    }
    return out;
}

} // namespace dtk
