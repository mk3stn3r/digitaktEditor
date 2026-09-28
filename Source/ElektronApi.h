/*
    ElektronApi.h - the Elektron API protocol (SysEx type 0x10).

    This is a *different protocol* from the pattern dumps in DigitaktCore.h.
    Dumps move project data; the API moves files on the +Drive, and it is what
    sample upload needs.

    Ported from elk-herd's SysEx/Message.elm and SysEx/ApiUtil.elm.

    Envelope:

        F0 00 20 3C 10 00 <7-bit encoded body> F7

        body = uint16be msgId, uint16be respId, uint8 messageId, args...

    Requests carry a msgId that the device echoes back in the response's
    respId, so replies can be matched to requests. A response's messageId is
    the request's plus 0x80. Unlike dumps there is no checksum - the 7-bit
    envelope is the whole framing.
*/

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "DigitaktCore.h"

namespace dtk
{
namespace api
{

//==============================================================================
enum MessageId : uint8_t
{
    device          = 0x01,
    version         = 0x02,
    query           = 0x09,

    dirList         = 0x10,
    dirCreate       = 0x11,
    dirDelete       = 0x12,

    fileDelete      = 0x20,
    itemRename      = 0x21,
    sampleFileInfo  = 0x23,

    fileReadOpen    = 0x30,
    fileReadClose   = 0x31,
    fileRead        = 0x32,

    fileWriteOpen   = 0x40,
    fileWriteClose  = 0x41,
    fileWrite       = 0x42,

    responseFlag    = 0x80
};

/** One entry in a +Drive directory listing.

    `hash` and `size` together identify a sample's *contents*. Project sample
    pool slots reference files this way rather than by path, which is why a
    sample keeps working after you move it, and why re-uploading a deleted
    sample silently repairs the reference.
*/
struct DirEntry
{
    uint32_t hash { 0 };
    uint32_t size { 0 };
    bool     locked { false };
    char     type { 'F' };        // 'D' directory, 'F' file
    std::string name;

    bool isDirectory() const noexcept { return type == 'D'; }
};

/** A decoded API message. Only the fields relevant to its id are populated. */
struct Message
{
    uint8_t  id { 0 };
    bool     isResponse { false };
    uint16_t msgId { 0 };
    uint16_t respId { 0 };

    bool     ok { false };
    uint32_t fd { 0 };
    uint32_t totalLen { 0 };
    uint32_t chunkLen { 0 };
    uint32_t chunkStart { 0 };
    uint32_t chunkEnd { 0 };
    uint32_t hash { 0 };
    int      productId { 0 };
    std::string text, text2;
    std::vector<uint8_t> supportedMessages;
    Bytes    data;
    std::vector<DirEntry> entries;

    std::string describe() const;
};

//==============================================================================
/** Builds request bodies. Each returns the message body without the envelope;
    `wrap` adds the SysEx framing and the message id pair.
*/
namespace request
{
    Bytes deviceInfo();
    Bytes versionInfo();
    Bytes listDirectory (const std::string& path);
    Bytes createDirectory (const std::string& path);
    Bytes deleteDirectory (const std::string& path);
    Bytes deleteFile (const std::string& path);
    Bytes rename (const std::string& from, const std::string& to);
    Bytes sampleInfo (uint32_t hash, uint32_t length);

    Bytes writeOpen (uint32_t totalLength, const std::string& path);
    Bytes writeChunk (uint32_t fd, uint32_t start, const Bytes& data);
    Bytes writeClose (uint32_t fd, uint32_t totalLength);

    Bytes readOpen (const std::string& path);
    Bytes readChunk (uint32_t fd, uint32_t length, uint32_t start);
    Bytes readClose (uint32_t fd);
}

/** Wraps a message body in the full SysEx envelope. */
Bytes wrap (const Bytes& body, uint16_t msgId, uint16_t respId = 0);

/** Parses one complete F0..F7 API message. Throws DigitaktError if it is not
    one (a pattern dump, for instance, will throw). */
Message parse (const Bytes& sysex);

/** True if these bytes look like an Elektron API message rather than a dump. */
bool isApiMessage (const Bytes& sysex);

//==============================================================================
/** The chunked file-transfer state machine.

    A transfer is open, then a series of chunks, then close - each step waiting
    for the device's response. This mirrors elk-herd's "pump": feed it each
    response, it tells you what to send next.
*/
class Transfer
{
public:
    enum class State { idle, opening, transferring, closing, done, failed };
    enum class Direction { upload, download };

    static constexpr uint32_t chunkSize = 0x2000;   // 8 KB, as elk-herd uses

    /** Begins an upload. `data` is the complete Elektron-format sample file. */
    void startUpload (const std::string& path, Bytes data);
    void startDownload (const std::string& path);

    /** The next request to send, or empty when there is nothing to send. */
    Bytes nextRequest() const { return pending; }

    /** Feeds a device response in; returns false and sets `error` on failure. */
    bool handleResponse (const Message&);

    State state() const noexcept { return currentState; }
    bool  finished() const noexcept
        { return currentState == State::done || currentState == State::failed; }
    bool  failed() const noexcept { return currentState == State::failed; }
    const std::string& errorMessage() const noexcept { return error; }

    /** 0.0 to 1.0. */
    double progress() const;

    /** For a download, the assembled file once done. */
    const Bytes& result() const noexcept { return buffer; }
    const std::string& path() const noexcept { return targetPath; }

    /** Sends a close for an open fd so the device does not leak a handle. */
    Bytes cancel();

private:
    void fail (const std::string& why);
    void queueNextUploadChunk();
    void queueNextDownloadChunk();

    Direction direction { Direction::upload };
    State currentState { State::idle };
    std::string targetPath, error;
    Bytes buffer, pending;
    uint32_t fd { 0 }, totalLength { 0 }, offset { 0 }, expectedEnd { 0 };
};

//==============================================================================
/** Converts audio into the Digitakt's +Drive sample format.

    The format is a 64-byte header followed by big-endian 16-bit mono PCM:

        byte     0  type (0)
        bytes  1-3  reserved
        bytes  4-7  sample data length in bytes
        bytes  8-11 sample rate (48000)
        bytes 12-15 first sample index (0)
        bytes 16-19 last sample index (frames - 1)
        byte    20  loop type (0x7F = no loop)
        bytes 21-63 reserved
*/
Bytes buildSampleFile (const std::vector<float>& monoSamples,
                       uint32_t sampleRate = 48000);

/** Digitakt sample names carry no extension and are limited in length. */
std::string sanitiseSampleName (const std::string& filename);

/** A sample file read back off the +Drive. */
struct SampleFileInfo
{
    uint32_t dataBytes { 0 };
    uint32_t sampleRate { 0 };
    uint32_t firstSample { 0 };
    uint32_t lastSample { 0 };
    uint8_t  loopType { 0 };
    std::vector<float> samples;

    bool looping() const noexcept { return loopType != 0x7F; }
    double seconds() const
        { return sampleRate > 0 ? (double) samples.size() / (double) sampleRate : 0.0; }
};

/** Decodes the +Drive sample format back into floats.

    The instrument may return up to 16 bytes more than the header declares -
    older firmware pads the waveform - so the declared length wins over the
    buffer length. */
bool parseSampleFile (const Bytes& fileData, SampleFileInfo& out,
                      std::string& errorOut);

constexpr uint32_t digitaktSampleRate = 48000;
constexpr size_t   sampleHeaderSize = 64;

} // namespace api
} // namespace dtk
