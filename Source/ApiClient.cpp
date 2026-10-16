#include "ApiClient.h"

void ApiClient::sendRaw (const dtk::Bytes& sysex)
{
    if (output == nullptr || sysex.size() < 2) return;
    // createSysExMessage adds the F0/F7 framing itself, so pass only the body.
    output->sendMessageNow (juce::MidiMessage::createSysExMessage (
        sysex.data() + 1, (int) sysex.size() - 2));
}

void ApiClient::sendRequest (const dtk::Bytes& body)
{
    awaitingId = nextMsgId++;
    if (nextMsgId == 0) nextMsgId = 1;
    lastRequest = dtk::api::wrap (body, awaitingId);
    sendRaw (lastRequest);
    startTimer (timeoutMs);
}

void ApiClient::failWith (const juce::String& why)
{
    stopTimer();
    busy = false;
    op = Op::none;
    if (listener != nullptr) listener->apiOperationFailed (why);
}

void ApiClient::finishOperation()
{
    stopTimer();
    busy = false;
    op = Op::none;
}

void ApiClient::timerCallback()
{
    stopTimer();
    if (! busy) return;

    failWith ("The Digitakt did not reply within "
              + juce::String (timeoutMs / 1000) + " seconds.\n\n"
              "Check that USB CONFIG is set to USB MIDI rather than Overbridge, "
              "and that MIDI CONFIG > PORT CONFIG has INPUT FROM and OUTPUT TO "
              "including USB. That port setting is saved per project, and a new "
              "project defaults to MIDI only.");
}

//==============================================================================
void ApiClient::identifyDevice()
{
    if (busy) return;
    if (output == nullptr) { failWith ("No MIDI output is open."); return; }
    busy = true;
    op = Op::identify;
    sendRequest (dtk::api::request::deviceInfo());
}

void ApiClient::requestVersion()
{
    if (busy) return;
    if (output == nullptr) { failWith ("No MIDI output is open."); return; }
    busy = true;
    op = Op::version;
    sendRequest (dtk::api::request::versionInfo());
}

void ApiClient::listDirectory (const juce::String& path)
{
    if (busy) return;
    if (output == nullptr) { failWith ("No MIDI output is open."); return; }
    busy = true;
    op = Op::list;
    pendingPath = path;
    sendRequest (dtk::api::request::listDirectory (path.toStdString()));
}

void ApiClient::createDirectory (const juce::String& path)
{
    if (busy) return;
    busy = true;
    op = Op::mkdir;
    pendingPath = path;
    sendRequest (dtk::api::request::createDirectory (path.toStdString()));
}

void ApiClient::deleteFile (const juce::String& path)
{
    if (busy) return;
    busy = true;
    op = Op::del;
    pendingPath = path;
    sendRequest (dtk::api::request::deleteFile (path.toStdString()));
}

void ApiClient::uploadSamples (std::vector<PreparedSample> samples,
                               const juce::String& destinationDir)
{
    if (busy) return;
    if (output == nullptr) { failWith ("No MIDI output is open."); return; }
    if (samples.empty()) return;

    queue = std::move (samples);
    queueIndex = 0;
    succeeded = failedCount = 0;
    destination = destinationDir.endsWithChar ('/') ? destinationDir.dropLastCharacters (1)
                                                    : destinationDir;
    busy = true;
    op = Op::upload;
    beginNextUpload();
}

void ApiClient::beginNextUpload()
{
    if (queueIndex >= queue.size())
    {
        finishOperation();
        if (listener != nullptr) listener->apiUploadFinished (succeeded, failedCount);
        return;
    }

    const auto& s = queue[queueIndex];
    const auto path = (destination + "/" + s.name).toStdString();

    transfer.startUpload (path, s.fileData);
    if (listener != nullptr)
    {
        listener->apiStatus ("Uploading " + s.name + " (" + s.describe() + ")");
        listener->apiUploadProgress (s.name, 0.0, (int) queueIndex + 1, (int) queue.size());
    }

    sendTransferRequest();
}

/** The Transfer builds its requests with a placeholder id; re-wrap the same
    body with a real one so every request can be matched to its reply. */
void ApiClient::sendTransferRequest()
{
    const auto req = transfer.nextRequest();
    if (req.empty()) { failWith ("The transfer stalled unexpectedly."); return; }

    awaitingId = nextMsgId++;
    if (nextMsgId == 0) nextMsgId = 1;

    const dtk::Bytes encoded (req.begin() + 6, req.end() - 1);
    const auto inner = dtk::sevenBitDecode (encoded.data(), encoded.size());
    const dtk::Bytes body (inner.begin() + 4, inner.end());
    lastRequest = dtk::api::wrap (body, awaitingId);
    sendRaw (lastRequest);
    startTimer (timeoutMs);
}

void ApiClient::downloadFiles (std::vector<juce::String> paths)
{
    if (busy) return;
    if (output == nullptr) { failWith ("No MIDI output is open."); return; }
    if (paths.empty()) return;

    downloadQueue = std::move (paths);
    queueIndex = 0;
    succeeded = failedCount = 0;
    busy = true;
    op = Op::download;
    beginNextDownload();
}

void ApiClient::beginNextDownload()
{
    if (queueIndex >= downloadQueue.size())
    {
        finishOperation();
        if (listener != nullptr) listener->apiDownloadFinished (succeeded, failedCount);
        return;
    }

    const auto path = downloadQueue[queueIndex];
    transfer.startDownload (path.toStdString());

    if (listener != nullptr)
        listener->apiDownloadProgress (path, 0.0, (int) queueIndex + 1,
                                       (int) downloadQueue.size());
    sendTransferRequest();
}

void ApiClient::cancel()
{
    if (! busy) return;
    if (op == Op::upload || op == Op::download)
    {
        const auto closing = transfer.cancel();
        if (! closing.empty()) sendRaw (closing);
    }
    finishOperation();
    if (listener != nullptr) listener->apiStatus ("Cancelled.");
}

//==============================================================================
void ApiClient::handleIncomingSysEx (const dtk::Bytes& sysex)
{
    if (! busy || ! dtk::api::isApiMessage (sysex)) return;

    dtk::api::Message msg;
    try
    {
        msg = dtk::api::parse (sysex);
    }
    catch (const std::exception&)
    {
        return;   // not something we can read; ignore rather than derail
    }

    if (! msg.isResponse || msg.respId != awaitingId) return;   // stale or unrelated
    stopTimer();

    switch (op)
    {
        case Op::identify:
            finishOperation();
            if (listener != nullptr)
                listener->apiDeviceIdentified (msg.productId, juce::String (msg.text));
            return;

        case Op::version:
        {
            finishOperation();
            if (listener == nullptr) return;

            // The response carries a build and a version string. Rather than
            // trust their order, pick whichever looks like a 4-digit build.
            const juce::String a (msg.text), b (msg.text2);
            const bool aIsBuild = a.containsOnly ("0123456789") && a.length() == 4;
            listener->apiVersionIdentified (aIsBuild ? a : b, aIsBuild ? b : a);
            return;
        }

        case Op::list:
            finishOperation();
            if (listener != nullptr) listener->apiDriveListed (pendingPath, msg.entries);
            return;

        case Op::mkdir:
        case Op::del:
            finishOperation();
            if (listener != nullptr)
            {
                if (msg.ok) listener->apiStatus (pendingPath + " done");
                else        listener->apiOperationFailed (pendingPath + ": the instrument refused");
            }
            return;

        case Op::upload:
        {
            if (! transfer.handleResponse (msg))
            {
                ++failedCount;
                if (listener != nullptr)
                    listener->apiOperationFailed (queue[queueIndex].name + ": "
                                                  + transfer.errorMessage());
                ++queueIndex;
                beginNextUpload();
                return;
            }

            if (listener != nullptr)
                listener->apiUploadProgress (queue[queueIndex].name, transfer.progress(),
                                             (int) queueIndex + 1, (int) queue.size());

            if (transfer.state() == dtk::api::Transfer::State::done)
            {
                ++succeeded;
                ++queueIndex;
                beginNextUpload();
                return;
            }

            sendTransferRequest();
            return;
        }

        case Op::download:
        {
            const auto path = downloadQueue[queueIndex];

            if (! transfer.handleResponse (msg))
            {
                ++failedCount;
                if (listener != nullptr)
                    listener->apiOperationFailed (path + ": " + transfer.errorMessage());
                ++queueIndex;
                beginNextDownload();
                return;
            }

            if (listener != nullptr)
                listener->apiDownloadProgress (path, transfer.progress(),
                                               (int) queueIndex + 1,
                                               (int) downloadQueue.size());

            if (transfer.state() == dtk::api::Transfer::State::done)
            {
                ++succeeded;
                if (listener != nullptr)
                    listener->apiFileDownloaded (path, transfer.result());
                ++queueIndex;
                beginNextDownload();
                return;
            }

            sendTransferRequest();
            return;
        }

        default:
            return;
    }
}
