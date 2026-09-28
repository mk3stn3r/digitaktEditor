#include "ElektronApi.h"

#include <algorithm>
#include <cmath>

namespace dtk
{
namespace api
{

static const uint8_t elektronId[3] = { 0x00, 0x20, 0x3C };

//==============================================================================
namespace
{
    void putU8 (Bytes& b, uint32_t v)  { b.push_back ((uint8_t) (v & 0xFF)); }

    void putU32 (Bytes& b, uint32_t v)
    {
        b.push_back ((uint8_t) ((v >> 24) & 0xFF));
        b.push_back ((uint8_t) ((v >> 16) & 0xFF));
        b.push_back ((uint8_t) ((v >> 8) & 0xFF));
        b.push_back ((uint8_t) (v & 0xFF));
    }

    /** Null-terminated Windows-1252 string, as the API uses throughout. */
    void putString (Bytes& b, const std::string& s)
    {
        for (char c : s) b.push_back ((uint8_t) c);
        b.push_back (0);
    }

    void putBytes (Bytes& b, const Bytes& d)
    {
        b.insert (b.end(), d.begin(), d.end());
    }

    Bytes message (uint8_t id) { Bytes b; putU8 (b, id); return b; }
}

//==============================================================================
namespace request
{

Bytes deviceInfo()  { return message (device); }
Bytes versionInfo() { return message (version); }

Bytes listDirectory (const std::string& path)
{
    auto b = message (dirList);
    putString (b, path);
    return b;
}

Bytes createDirectory (const std::string& path)
{
    auto b = message (dirCreate);
    putString (b, path);
    return b;
}

Bytes deleteDirectory (const std::string& path)
{
    auto b = message (dirDelete);
    putString (b, path);
    return b;
}

Bytes deleteFile (const std::string& path)
{
    auto b = message (fileDelete);
    putString (b, path);
    return b;
}

Bytes rename (const std::string& from, const std::string& to)
{
    auto b = message (itemRename);
    putString (b, from);
    putString (b, to);
    return b;
}

Bytes sampleInfo (uint32_t hash, uint32_t length)
{
    auto b = message (sampleFileInfo);
    putU32 (b, hash);
    putU32 (b, length);
    return b;
}

Bytes writeOpen (uint32_t totalLength, const std::string& path)
{
    auto b = message (fileWriteOpen);
    putU32 (b, totalLength);
    putString (b, path);
    return b;
}

Bytes writeChunk (uint32_t fd_, uint32_t start, const Bytes& data)
{
    auto b = message (fileWrite);
    putU32 (b, fd_);
    putU32 (b, (uint32_t) data.size());
    putU32 (b, start);
    putBytes (b, data);
    return b;
}

Bytes writeClose (uint32_t fd_, uint32_t totalLength)
{
    auto b = message (fileWriteClose);
    putU32 (b, fd_);
    putU32 (b, totalLength);
    return b;
}

Bytes readOpen (const std::string& path)
{
    auto b = message (fileReadOpen);
    putString (b, path);
    return b;
}

Bytes readChunk (uint32_t fd_, uint32_t length, uint32_t start)
{
    auto b = message (fileRead);
    putU32 (b, fd_);
    putU32 (b, length);
    putU32 (b, start);
    return b;
}

Bytes readClose (uint32_t fd_)
{
    auto b = message (fileReadClose);
    putU32 (b, fd_);
    return b;
}

} // namespace request

//==============================================================================
Bytes wrap (const Bytes& body, uint16_t msgId, uint16_t respId)
{
    Bytes inner;
    inner.push_back ((uint8_t) ((msgId >> 8) & 0xFF));
    inner.push_back ((uint8_t) (msgId & 0xFF));
    inner.push_back ((uint8_t) ((respId >> 8) & 0xFF));
    inner.push_back ((uint8_t) (respId & 0xFF));
    inner.insert (inner.end(), body.begin(), body.end());

    const auto encoded = sevenBitEncode (inner);

    Bytes out;
    out.push_back (0xF0);
    out.insert (out.end(), elektronId, elektronId + 3);
    out.push_back (0x10);          // Elektron API
    out.push_back (0x00);
    out.insert (out.end(), encoded.begin(), encoded.end());
    out.push_back (0xF7);
    return out;
}

bool isApiMessage (const Bytes& sysex)
{
    return sysex.size() >= 7 && sysex[0] == 0xF0
        && sysex[1] == 0x00 && sysex[2] == 0x20 && sysex[3] == 0x3C
        && sysex[4] == 0x10;
}

//==============================================================================
namespace
{
    struct BodyReader
    {
        const Bytes& d;
        size_t pos = 0;

        void need (size_t n) const
        {
            if (pos + n > d.size())
                throw DigitaktError ("truncated API message");
        }

        uint8_t u8()  { need (1); return d[pos++]; }

        uint32_t u32()
        {
            need (4);
            const uint32_t v = ((uint32_t) d[pos] << 24) | ((uint32_t) d[pos+1] << 16)
                             | ((uint32_t) d[pos+2] << 8) | (uint32_t) d[pos+3];
            pos += 4;
            return v;
        }

        bool boolean()
        {
            const uint8_t b = u8();
            if (b > 1) throw DigitaktError ("not a boolean byte in API response");
            return b == 1;
        }

        std::string str()
        {
            std::string s;
            for (;;)
            {
                need (1);
                const uint8_t c = d[pos++];
                if (c == 0) break;
                s += (char) c;
            }
            return s;
        }

        Bytes rest()
        {
            Bytes b (d.begin() + (long) pos, d.end());
            pos = d.size();
            return b;
        }

        bool atEnd() const { return pos >= d.size(); }
    };
}

Message parse (const Bytes& sysex)
{
    if (! isApiMessage (sysex))
        throw DigitaktError ("not an Elektron API message");
    if (sysex.back() != 0xF7)
        throw DigitaktError ("unterminated SysEx message");

    // strip F0 00 20 3C 10 00 ... F7
    const Bytes encoded (sysex.begin() + 6, sysex.end() - 1);
    const auto inner = sevenBitDecode (encoded.data(), encoded.size());
    if (inner.size() < 5)
        throw DigitaktError ("API message too short");

    Message m;
    m.msgId  = (uint16_t) ((inner[0] << 8) | inner[1]);
    m.respId = (uint16_t) ((inner[2] << 8) | inner[3]);

    const Bytes body (inner.begin() + 4, inner.end());
    BodyReader r { body };

    m.id = r.u8();
    m.isResponse = (m.id & responseFlag) != 0;
    const uint8_t base = (uint8_t) (m.id & ~responseFlag);

    if (! m.isResponse) return m;    // we only ever parse responses in practice

    switch (base)
    {
        case device:
            m.productId = r.u8();
            {
                const uint8_t n = r.u8();
                for (uint8_t i = 0; i < n; ++i) m.supportedMessages.push_back (r.u8());
            }
            m.text = r.str();
            m.ok = true;
            break;

        case version:
            m.text  = r.str();       // build
            m.text2 = r.str();       // version
            m.ok = true;
            break;

        case dirList:
            while (! r.atEnd())
            {
                DirEntry e;
                e.hash   = r.u32();
                e.size   = r.u32();
                e.locked = r.boolean();
                e.type   = (char) r.u8();
                e.name   = r.str();
                m.entries.push_back (e);
            }
            m.ok = true;
            break;

        case dirCreate:
        case dirDelete:
        case fileDelete:
        case itemRename:
            m.ok = r.boolean();
            break;

        case sampleFileInfo:
            m.ok       = r.boolean();
            m.totalLen = r.u32();     // the API sends length then hash here
            m.hash     = r.u32();
            m.text     = r.str();
            break;

        case fileReadOpen:
            m.ok       = r.boolean();
            m.fd       = r.u32();
            m.totalLen = r.u32();
            break;

        case fileReadClose:
            m.fd       = r.u32();
            m.totalLen = r.u32();
            m.ok = true;
            break;

        case fileRead:
            m.ok         = r.boolean();
            m.fd         = r.u32();
            m.chunkLen   = r.u32();
            m.chunkStart = r.u32();
            m.chunkEnd   = r.u32();
            m.data       = r.rest();
            break;

        case fileWriteOpen:
            m.ok = r.boolean();
            m.fd = r.u32();
            break;

        case fileWriteClose:
            m.ok       = r.boolean();
            m.fd       = r.u32();
            m.totalLen = r.u32();
            break;

        case fileWrite:
            m.ok       = r.boolean();
            m.totalLen = r.u32();     // bytes written so far
            break;

        default:
            m.data = r.rest();
            break;
    }
    return m;
}

std::string Message::describe() const
{
    auto name = [] (uint8_t base) -> const char*
    {
        switch (base)
        {
            case device:         return "Device";
            case version:        return "Version";
            case query:          return "Query";
            case dirList:        return "DirList";
            case dirCreate:      return "DirCreate";
            case dirDelete:      return "DirDelete";
            case fileDelete:     return "FileDelete";
            case itemRename:     return "ItemRename";
            case sampleFileInfo: return "SampleFileInfo";
            case fileReadOpen:   return "FileReadOpen";
            case fileReadClose:  return "FileReadClose";
            case fileRead:       return "FileRead";
            case fileWriteOpen:  return "FileWriteOpen";
            case fileWriteClose: return "FileWriteClose";
            case fileWrite:      return "FileWrite";
            default:             return "Unknown";
        }
    };
    std::string s = name ((uint8_t) (id & ~responseFlag));
    s += isResponse ? "Response" : "Request";
    if (isResponse) s += ok ? " ok" : " FAILED";
    return s;
}

//==============================================================================
void Transfer::fail (const std::string& why)
{
    currentState = State::failed;
    error = why;
    pending.clear();
}

void Transfer::startUpload (const std::string& path, Bytes data)
{
    direction   = Direction::upload;
    targetPath  = path;
    buffer      = std::move (data);
    totalLength = (uint32_t) buffer.size();
    offset      = 0;
    fd          = 0;
    error.clear();
    currentState = State::opening;
    pending = wrap (request::writeOpen (totalLength, path), 0);
}

void Transfer::startDownload (const std::string& path)
{
    direction   = Direction::download;
    targetPath  = path;
    buffer.clear();
    totalLength = 0;
    offset      = 0;
    fd          = 0;
    error.clear();
    currentState = State::opening;
    pending = wrap (request::readOpen (path), 0);
}

void Transfer::queueNextUploadChunk()
{
    if (offset >= totalLength)
    {
        currentState = State::closing;
        pending = wrap (request::writeClose (fd, totalLength), 0);
        return;
    }
    const uint32_t len = std::min (chunkSize, totalLength - offset);
    const Bytes chunk (buffer.begin() + (long) offset,
                       buffer.begin() + (long) (offset + len));
    pending = wrap (request::writeChunk (fd, offset, chunk), 0);
    expectedEnd = offset + len;
    currentState = State::transferring;
}

void Transfer::queueNextDownloadChunk()
{
    if (offset >= totalLength)
    {
        currentState = State::closing;
        pending = wrap (request::readClose (fd), 0);
        return;
    }
    const uint32_t len = std::min (chunkSize, totalLength - offset);
    pending = wrap (request::readChunk (fd, len, offset), 0);
    expectedEnd = offset + len;
    currentState = State::transferring;
}

bool Transfer::handleResponse (const Message& m)
{
    const uint8_t base = (uint8_t) (m.id & ~responseFlag);

    switch (currentState)
    {
        case State::opening:
            if (direction == Direction::upload)
            {
                if (base != fileWriteOpen) { fail ("unexpected reply to write open"); return false; }
                if (! m.ok) { fail ("the instrument refused to create the file - "
                                    "the +Drive may be full or the path may not exist"); return false; }
                fd = m.fd;
                queueNextUploadChunk();
            }
            else
            {
                if (base != fileReadOpen) { fail ("unexpected reply to read open"); return false; }
                if (! m.ok) { fail ("the instrument could not open that file"); return false; }
                fd = m.fd;
                totalLength = m.totalLen;
                queueNextDownloadChunk();
            }
            return true;

        case State::transferring:
            if (direction == Direction::upload)
            {
                if (base != fileWrite) { fail ("unexpected reply during write"); return false; }
                if (! m.ok) { fail ("the instrument rejected a chunk"); return false; }
                // The device echoes the offset it wrote at, not the new end.
                if (m.totalLen != offset)
                {
                    fail ("write offset mismatch: expected " + std::to_string (offset)
                          + ", instrument reported " + std::to_string (m.totalLen));
                    return false;
                }
                offset = expectedEnd;
                queueNextUploadChunk();
            }
            else
            {
                if (base != fileRead) { fail ("unexpected reply during read"); return false; }
                if (! m.ok) { fail ("the instrument rejected a read"); return false; }
                if (m.fd != fd) { fail ("the instrument returned the wrong file handle"); return false; }
                if (m.chunkLen + m.chunkStart != m.chunkEnd)
                {
                    fail ("inconsistent chunk bounds in read response");
                    return false;
                }
                buffer.insert (buffer.end(), m.data.begin(), m.data.end());
                offset = m.chunkEnd;
                queueNextDownloadChunk();
            }
            return true;

        case State::closing:
            if (direction == Direction::upload)
            {
                if (base != fileWriteClose) { fail ("unexpected reply to write close"); return false; }
                if (! m.ok) { fail ("the instrument reported an error on close"); return false; }
                if (m.totalLen != totalLength)
                {
                    fail ("the instrument stored " + std::to_string (m.totalLen)
                          + " bytes but we sent " + std::to_string (totalLength));
                    return false;
                }
            }
            else
            {
                if (base != fileReadClose) { fail ("unexpected reply to read close"); return false; }
                // Digitakt OS before 1.08 reported the size 16 bytes long.
                if (! (m.totalLen == totalLength || m.totalLen == totalLength + 0x10))
                {
                    fail ("size mismatch on close");
                    return false;
                }
            }
            currentState = State::done;
            pending.clear();
            return true;

        default:
            fail ("response arrived while the transfer was idle");
            return false;
    }
}

double Transfer::progress() const
{
    switch (currentState)
    {
        case State::idle:
        case State::opening:      return 0.0;
        case State::done:         return 1.0;
        case State::failed:       return 0.0;
        case State::closing:      return 1.0;
        case State::transferring: return totalLength > 0
                                       ? (double) offset / (double) totalLength : 0.0;
    }
    return 0.0;
}

Bytes Transfer::cancel()
{
    const bool open = currentState == State::transferring || currentState == State::closing;
    currentState = State::failed;
    if (error.empty()) error = "cancelled";
    pending.clear();

    if (! open) return {};
    return direction == Direction::upload ? wrap (request::writeClose (fd, 0), 0)
                                          : wrap (request::readClose (fd), 0);
}

//==============================================================================
Bytes buildSampleFile (const std::vector<float>& mono, uint32_t sampleRate)
{
    const uint32_t frames = (uint32_t) mono.size();

    Bytes out;
    out.reserve (sampleHeaderSize + mono.size() * 2);

    out.push_back (0);                       // type
    out.insert (out.end(), 3, 0);            // reserved
    putU32 (out, frames * 2);                // data length in bytes
    putU32 (out, sampleRate);
    putU32 (out, 0);                         // first sample index
    putU32 (out, frames > 0 ? frames - 1 : 0);
    out.push_back (0x7F);                    // loop type: none
    out.insert (out.end(), 43, 0);           // reserved

    for (float f : mono)
    {
        // elk-herd's conversion: floor(f * 32767.5), which maps the full
        // [-1, 1) range without clipping the negative rail.
        const float clamped = std::max (-1.0f, std::min (1.0f, f));
        int v = (int) std::floor (clamped * 32767.5f);
        v = std::max (-32768, std::min (32767, v));
        const uint16_t u = (uint16_t) (v & 0xFFFF);
        out.push_back ((uint8_t) ((u >> 8) & 0xFF));
        out.push_back ((uint8_t) (u & 0xFF));
    }
    return out;
}

bool parseSampleFile (const Bytes& d, SampleFileInfo& out, std::string& errorOut)
{
    if (d.size() < sampleHeaderSize)
    {
        errorOut = "file is shorter than the 64-byte sample header";
        return false;
    }

    auto be32 = [&] (size_t o) {
        return ((uint32_t) d[o] << 24) | ((uint32_t) d[o + 1] << 16)
             | ((uint32_t) d[o + 2] << 8) | (uint32_t) d[o + 3];
    };

    if (d[0] != 0)
    {
        errorOut = "unexpected sample type byte " + std::to_string ((int) d[0])
                 + "; this build only understands type 0";
        return false;
    }

    out.dataBytes   = be32 (4);
    out.sampleRate  = be32 (8);
    out.firstSample = be32 (12);
    out.lastSample  = be32 (16);
    out.loopType    = d[20];

    if (out.sampleRate == 0 || out.sampleRate > 192000)
    {
        errorOut = "implausible sample rate " + std::to_string (out.sampleRate);
        return false;
    }

    const size_t available = d.size() - sampleHeaderSize;
    // Trust the header, but never read past the buffer.
    const size_t bytes = std::min ((size_t) out.dataBytes, available) & ~(size_t) 1;

    out.samples.clear();
    out.samples.reserve (bytes / 2);
    for (size_t i = 0; i + 1 < bytes; i += 2)
    {
        const int16_t v = (int16_t) ((d[sampleHeaderSize + i] << 8)
                                     | d[sampleHeaderSize + i + 1]);
        out.samples.push_back ((float) v / 32767.5f);
    }

    if (out.samples.empty())
    {
        errorOut = "sample contains no audio data";
        return false;
    }
    return true;
}

std::string sanitiseSampleName (const std::string& filename)
{
    std::string s = filename;

    // strip a leading directory
    const auto slash = s.find_last_of ("/\\");
    if (slash != std::string::npos) s = s.substr (slash + 1);

    // strip a known audio extension, as elk-herd does
    for (const char* ext : { ".wav", ".WAV", ".aif", ".aiff", ".AIF", ".AIFF",
                             ".flac", ".FLAC", ".mp3", ".MP3", ".ogg", ".OGG",
                             ".aac", ".AAC", ".mp4", ".MP4" })
    {
        const size_t n = std::string (ext).size();
        if (s.size() > n && s.compare (s.size() - n, n, ext) == 0)
        {
            s = s.substr (0, s.size() - n);
            break;
        }
    }

    // the +Drive rejects path separators and control characters
    std::string clean;
    for (char c : s)
        if (c != '/' && c != '\\' && (unsigned char) c >= 0x20) clean += c;

    if (clean.empty()) clean = "sample";
    if (clean.size() > 63) clean = clean.substr (0, 63);
    return clean;
}

} // namespace api
} // namespace dtk
