/*
    SamplesPanel.h - +Drive browser and sample upload.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ApiClient.h"
#include "DigitaktCore.h"

class SamplesPanel : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     private juce::ListBoxModel,
                     private ApiClient::Listener
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        /** The user asked to bind a +Drive file to a project sample slot. */
        virtual void assignSampleToSlot (const dtk::api::DirEntry&, int slot) = 0;
        virtual void statusMessage (const juce::String&, bool isError) = 0;
        /** The instrument reported its OS; used to pick the storage version. */
        virtual void deviceVersionKnown (const juce::String& build,
                                         const juce::String& osVersion) = 0;
    };

    SamplesPanel();
    ~SamplesPanel() override;

    void setListener (Listener* l) { listener = l; }
    void setMidiOutput (juce::MidiOutput* o) { api.setOutput (o); }
    void handleIncomingSysEx (const dtk::Bytes& b) { api.handleIncomingSysEx (b); }

    /** So the slot chooser can show what is already in the project pool. */
    void setProjectSlots (const std::vector<int>& usedSlots) { poolSlots = usedSlots; }

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;

    void refresh();
    /** Asks the instrument which OS it runs. */
    void identifyDevice() { api.requestVersion(); }

private:
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void apiDeviceIdentified (int productId, const juce::String& name) override;
    void apiVersionIdentified (const juce::String& build,
                               const juce::String& osVersion) override;
    void apiDriveListed (const juce::String& path,
                         const std::vector<dtk::api::DirEntry>&) override;
    void apiUploadProgress (const juce::String& name, double fraction,
                            int index, int total) override;
    void apiUploadFinished (int ok, int failed) override;
    void apiDownloadProgress (const juce::String& name, double fraction,
                              int index, int total) override;
    void apiFileDownloaded (const juce::String& path, const dtk::Bytes&) override;
    void apiDownloadFinished (int ok, int failed) override;
    void apiOperationFailed (const juce::String& what) override;
    void apiStatus (const juce::String& text) override;

    void chooseFilesToUpload();
    void downloadSelected();
    bool writeSampleAsWav (const juce::String& drivePath, const dtk::Bytes&,
                           juce::String& errorOut);
    void uploadPaths (const juce::StringArray& paths);
    void navigateTo (const juce::String& path);
    void goUp();
    void setStatus (const juce::String&, bool isError = false);

    ApiClient api;
    Listener* listener = nullptr;

    juce::String currentPath { "/" };
    std::vector<dtk::api::DirEntry> entries;
    std::vector<int> poolSlots;

    juce::ListBox list { "drive", this };
    juce::TextButton refreshButton { "Refresh" };
    juce::TextButton upButton { "Up" };
    juce::TextButton uploadButton { "Upload samples..." };
    juce::TextButton newFolderButton { "New folder" };
    juce::TextButton downloadButton { "Download" };
    juce::TextButton cancelButton { "Cancel" };
    juce::Label pathLabel;
    juce::Label statusLabel;
    juce::ProgressBar progressBar { progress };
    double progress { 0.0 };
    bool dragHighlight = false;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::File downloadDir;
    juce::StringArray downloadProblems;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplesPanel)
};
