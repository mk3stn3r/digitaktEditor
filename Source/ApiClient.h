/*
    ApiClient.h - drives Elektron API transactions over a MIDI port.

    The API is strictly request/response: send one message, wait for the reply
    carrying the same id, then send the next. This class owns that loop, the
    id counter, the timeout, and the multi-message operations built on top
    (scan the drive, upload a queue of samples).
*/

#pragma once

#include <deque>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ElektronApi.h"
#include "SampleImport.h"

class ApiClient : private juce::Timer
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void apiDeviceIdentified (int productId, const juce::String& name) {}
        virtual void apiDriveListed (const juce::String& path,
                                     const std::vector<dtk::api::DirEntry>&) {}
        virtual void apiUploadProgress (const juce::String& name, double fraction,
                                        int index, int total) {}
        virtual void apiUploadFinished (int succeeded, int failed) {}
        virtual void apiDownloadProgress (const juce::String& name, double fraction,
                                          int index, int total) {}
        /** One file arrived; the raw +Drive bytes, not yet decoded. */
        virtual void apiFileDownloaded (const juce::String& path, const dtk::Bytes&) {}
        virtual void apiDownloadFinished (int succeeded, int failed) {}
        virtual void apiOperationFailed (const juce::String& what) {}
        virtual void apiStatus (const juce::String& text) {}
    };

    ApiClient() = default;
    ~ApiClient() override { stopTimer(); }

    void setOutput (juce::MidiOutput* o) { output = o; }
    void setListener (Listener* l) { listener = l; }

    /** Feed every incoming SysEx message here; non-API messages are ignored. */
    void handleIncomingSysEx (const dtk::Bytes& sysex);

    bool isBusy() const noexcept { return busy; }

    //== operations ===========================================================
    void identifyDevice();
    void listDirectory (const juce::String& path);
    void createDirectory (const juce::String& path);
    void deleteFile (const juce::String& path);

    /** Uploads prepared samples one after another into `destinationDir`. */
    void uploadSamples (std::vector<PreparedSample> samples,
                        const juce::String& destinationDir);

    /** Downloads +Drive files one after another. Each arrives via
        apiFileDownloaded as raw bytes for the caller to decode. */
    void downloadFiles (std::vector<juce::String> paths);

    void cancel();

    /** Milliseconds to wait for a reply before giving up. Large writes on a
        busy +Drive are not instant, so this is generous. */
    static constexpr int timeoutMs = 6000;

private:
    enum class Op { none, identify, list, mkdir, del, upload, download };

    void timerCallback() override;
    void sendRaw (const dtk::Bytes& sysex);
    void sendRequest (const dtk::Bytes& body);
    void finishOperation();
    void beginNextUpload();
    void beginNextDownload();
    void sendTransferRequest();
    void failWith (const juce::String& why);

    juce::MidiOutput* output = nullptr;
    Listener* listener = nullptr;

    Op op { Op::none };
    bool busy = false;
    uint16_t nextMsgId = 1;
    uint16_t awaitingId = 0;
    dtk::Bytes lastRequest;

    dtk::api::Transfer transfer;
    std::vector<PreparedSample> queue;
    std::vector<juce::String> downloadQueue;
    size_t queueIndex = 0;
    int succeeded = 0, failedCount = 0;
    juce::String destination, pendingPath;
};
