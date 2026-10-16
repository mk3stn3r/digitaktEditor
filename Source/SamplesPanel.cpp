#include "SamplesPanel.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
    const juce::Colour bgColour   { 0xff1b1d21 };
    const juce::Colour rowAlt     { 0xff212429 };
    const juce::Colour textCol    { 0xffd8dee6 };
    const juce::Colour textDim    { 0xff7d858f };
    const juce::Colour dirColour  { 0xff4c8ed9 };
    const juce::Colour lockColour { 0xffe0a33c };
    const juce::Colour errColour  { 0xffe07a5f };
    const juce::Colour dropColour { 0xff8fbf7f };

    juce::String formatSize (juce::int64 bytes)
    {
        if (bytes < 1024) return juce::String (bytes) + " B";
        if (bytes < 1024 * 1024) return juce::String (bytes / 1024) + " KB";
        return juce::String (bytes / (1024.0 * 1024.0), 1) + " MB";
    }
}

SamplesPanel::SamplesPanel()
{
    api.setListener (this);

    addAndMakeVisible (list);
    list.setColour (juce::ListBox::backgroundColourId, bgColour);
    list.setRowHeight (24);
    list.setMultipleSelectionEnabled (true);

    auto add = [this] (juce::TextButton& b, std::function<void()> fn)
    {
        addAndMakeVisible (b);
        b.onClick = std::move (fn);
    };
    add (refreshButton,   [this] { refresh(); });
    add (upButton,        [this] { goUp(); });
    add (uploadButton,    [this] { chooseFilesToUpload(); });
    add (newFolderButton, [this]
    {
        auto* aw = new juce::AlertWindow ("New folder", "Name for the new folder:",
                                          juce::MessageBoxIconType::QuestionIcon);
        aw->addTextEditor ("name", "", "Name:");
        aw->addButton ("Create", 1);
        aw->addButton ("Cancel", 0);
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw] (int result)
            {
                const auto name = aw->getTextEditorContents ("name").trim();
                std::unique_ptr<juce::AlertWindow> owner (aw);
                if (result == 1 && name.isNotEmpty())
                    api.createDirectory (currentPath == "/" ? "/" + name
                                                            : currentPath + "/" + name);
            }), false);
    });
    add (downloadButton, [this] { downloadSelected(); });
    add (cancelButton, [this] { api.cancel(); });
    cancelButton.setEnabled (false);

    addAndMakeVisible (pathLabel);
    pathLabel.setColour (juce::Label::textColourId, textCol);
    pathLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));

    addAndMakeVisible (statusLabel);
    statusLabel.setFont (juce::FontOptions (12.0f));
    pathLabel.setText ("+Drive: " + currentPath, juce::dontSendNotification);
    setStatus ("Press Refresh to read the +Drive. Drop audio files here to upload.");

    addChildComponent (progressBar);
    progressBar.setColour (juce::ProgressBar::backgroundColourId, rowAlt);
}

SamplesPanel::~SamplesPanel() { api.setListener (nullptr); }

//==============================================================================
void SamplesPanel::paint (juce::Graphics& g)
{
    g.fillAll (bgColour);

    if (dragHighlight)
    {
        g.setColour (dropColour.withAlpha (0.15f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (3.0f), 6.0f);
        g.setColour (dropColour);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (3.0f), 6.0f, 2.0f);
    }
}

void SamplesPanel::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto toolbar = area.removeFromTop (28);
    refreshButton  .setBounds (toolbar.removeFromLeft (74).reduced (2));
    upButton       .setBounds (toolbar.removeFromLeft (48).reduced (2));
    uploadButton   .setBounds (toolbar.removeFromLeft (140).reduced (2));
    downloadButton .setBounds (toolbar.removeFromLeft (90).reduced (2));
    newFolderButton.setBounds (toolbar.removeFromLeft (100).reduced (2));
    cancelButton   .setBounds (toolbar.removeFromLeft (74).reduced (2));

    area.removeFromTop (4);
    pathLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);

    statusLabel.setBounds (area.removeFromBottom (20));
    progressBar.setBounds (area.removeFromBottom (18).reduced (0, 2));
    area.removeFromBottom (4);

    list.setBounds (area);
}

//==============================================================================
int SamplesPanel::getNumRows() { return (int) entries.size(); }

void SamplesPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) entries.size()) return;
    const auto& e = entries[(size_t) row];

    g.fillAll (selected ? rowAlt.brighter (0.2f) : (row % 2 ? rowAlt : bgColour));

    g.setColour (e.isDirectory() ? dirColour : textCol);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ((e.isDirectory() ? "[ " + juce::String (e.name) + " ]"
                                 : juce::String (e.name)),
                8, 0, w - 200, h, juce::Justification::centredLeft);

    if (e.locked)
    {
        g.setColour (lockColour);
        g.drawText ("locked", w - 190, 0, 50, h, juce::Justification::centredLeft);
    }

    if (! e.isDirectory())
    {
        g.setColour (textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (formatSize ((juce::int64) e.size), w - 136, 0, 60, h,
                    juce::Justification::centredRight);
        g.drawText ("#" + juce::String::toHexString ((int) e.hash),
                    w - 72, 0, 66, h, juce::Justification::centredRight);
    }
}

void SamplesPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= (int) entries.size()) return;
    const auto& e = entries[(size_t) row];

    if (e.isDirectory())
    {
        navigateTo (currentPath == "/" ? "/" + juce::String (e.name)
                                       : currentPath + "/" + juce::String (e.name));
        return;
    }

    // A file: offer to bind it to a project sample slot.
    if (listener == nullptr) return;

    auto* aw = new juce::AlertWindow (
        "Assign to sample slot",
        juce::String (e.name) + "\n\nWhich of the project's 128 sample slots should "
        "point at this file?\n\nSlots are matched by content, not path, so this "
        "survives moving the file on the +Drive.",
        juce::MessageBoxIconType::QuestionIcon);
    aw->addTextEditor ("slot", "1", "Slot (1-127):");
    aw->addButton ("Assign", 1);
    aw->addButton ("Cancel", 0);

    const auto entry = e;
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, aw, entry] (int result)
        {
            const int slot = aw->getTextEditorContents ("slot").getIntValue();
            std::unique_ptr<juce::AlertWindow> owner (aw);
            if (result == 1 && slot >= 1 && slot <= 127 && listener != nullptr)
                listener->assignSampleToSlot (entry, slot);
        }), false);
}

//==============================================================================
bool SamplesPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        const auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac"
            || ext == ".ogg" || ext == ".mp3")
            return true;
    }
    return false;
}

void SamplesPanel::fileDragEnter (const juce::StringArray&, int, int)
{
    dragHighlight = true;
    repaint();
}

void SamplesPanel::fileDragExit (const juce::StringArray&)
{
    dragHighlight = false;
    repaint();
}

void SamplesPanel::filesDropped (const juce::StringArray& files, int, int)
{
    dragHighlight = false;
    repaint();
    uploadPaths (files);
}

void SamplesPanel::chooseFilesToUpload()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Choose audio files to upload to the +Drive", juce::File(),
        "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::canSelectMultipleItems,
        [this] (const juce::FileChooser& fc)
        {
            juce::StringArray paths;
            for (const auto& f : fc.getResults()) paths.add (f.getFullPathName());
            if (! paths.isEmpty()) uploadPaths (paths);
        });
}

void SamplesPanel::uploadPaths (const juce::StringArray& paths)
{
    if (api.isBusy()) { setStatus ("A transfer is already running.", true); return; }

    std::vector<PreparedSample> prepared;
    juce::StringArray problems, notes;
    juce::int64 total = 0;

    for (const auto& p : paths)
    {
        PreparedSample s;
        juce::String error;
        if (! prepareSampleForUpload (juce::File (p), s, error))
        {
            problems.add (error);
            continue;
        }
        total += (juce::int64) s.sizeBytes();
        if (s.resampled || s.downmixed || s.clipped)
            notes.add (s.name + ": " + s.describe());
        prepared.push_back (std::move (s));
    }

    if (prepared.empty())
    {
        setStatus (problems.isEmpty() ? "Nothing to upload."
                                      : problems.joinIntoString ("  "), true);
        return;
    }

    juce::String summary;
    summary << "Upload " << prepared.size() << " sample"
            << (prepared.size() == 1 ? "" : "s") << " (" << formatSize (total)
            << ") to " << currentPath << "?\n\n";

    if (! notes.isEmpty())
        summary << "Conversions applied:\n" << notes.joinIntoString ("\n") << "\n\n";
    if (! problems.isEmpty())
        summary << "Skipped:\n" << problems.joinIntoString ("\n") << "\n\n";

    if (total > digitaktSampleMemoryBytes)
        summary << "Warning: this is larger than the Digitakt's 64 MB of sample "
                   "memory, so not all of it will be loadable into a project.\n\n";

    summary << "Existing files with the same name will be overwritten.";

    juce::AlertWindow::showOkCancelBox (
        juce::MessageBoxIconType::QuestionIcon, "Upload to +Drive", summary,
        "Upload", "Cancel", nullptr,
        juce::ModalCallbackFunction::create (
            [this, prepared] (int result) mutable
            {
                if (result == 0) return;
                cancelButton.setEnabled (true);
                progressBar.setVisible (true);
                api.uploadSamples (std::move (prepared), currentPath);
            }));
}

//==============================================================================
void SamplesPanel::navigateTo (const juce::String& path)
{
    currentPath = path;
    refresh();
}

void SamplesPanel::goUp()
{
    if (currentPath == "/") return;
    const int slash = currentPath.lastIndexOfChar ('/');
    navigateTo (slash <= 0 ? "/" : currentPath.substring (0, slash));
}

void SamplesPanel::refresh()
{
    pathLabel.setText ("+Drive: " + currentPath, juce::dontSendNotification);
    setStatus ("Reading " + currentPath + "...");
    api.listDirectory (currentPath);
}

void SamplesPanel::setStatus (const juce::String& text, bool isError)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, isError ? errColour
                                                              : textDim);
    if (listener != nullptr) listener->statusMessage (text, isError);
}

//==============================================================================
void SamplesPanel::apiDeviceIdentified (int productId, const juce::String& name)
{
    setStatus ("Connected to " + name + " (product id " + juce::String (productId) + ")");
}

void SamplesPanel::apiVersionIdentified (const juce::String& build,
                                         const juce::String& osVersion)
{
    setStatus ("Digitakt OS " + osVersion + " (build " + build + ")");
    if (listener != nullptr) listener->deviceVersionKnown (build, osVersion);
}

void SamplesPanel::apiDriveListed (const juce::String& path,
                                   const std::vector<dtk::api::DirEntry>& listed)
{
    currentPath = path;
    entries = listed;

    // Directories first, then files, each alphabetically - the drive returns
    // them in whatever order it likes.
    std::sort (entries.begin(), entries.end(),
               [] (const dtk::api::DirEntry& a, const dtk::api::DirEntry& b)
               {
                   if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
                   return juce::String (a.name).compareIgnoreCase (juce::String (b.name)) < 0;
               });

    list.updateContent();
    list.repaint();
    pathLabel.setText ("+Drive: " + currentPath, juce::dontSendNotification);

    int files = 0, dirs = 0;
    juce::int64 bytes = 0;
    for (const auto& e : entries)
    {
        if (e.isDirectory()) ++dirs;
        else { ++files; bytes += (juce::int64) e.size; }
    }
    setStatus (juce::String (files) + " sample(s), " + juce::String (dirs)
               + " folder(s), " + formatSize (bytes)
               + "  -  double-click a sample to assign it to a project slot");
}

void SamplesPanel::apiUploadProgress (const juce::String& name, double fraction,
                                      int index, int total)
{
    progress = fraction;
    progressBar.setVisible (true);
    setStatus ("Uploading " + name + "  (" + juce::String (index) + " of "
               + juce::String (total) + ")  " + juce::String ((int) (fraction * 100)) + "%");
}

void SamplesPanel::apiUploadFinished (int ok, int failed)
{
    progressBar.setVisible (false);
    cancelButton.setEnabled (false);
    progress = 0.0;
    setStatus (juce::String (ok) + " sample(s) uploaded"
               + (failed > 0 ? ", " + juce::String (failed) + " failed" : ""),
               failed > 0);
    refresh();
}

void SamplesPanel::apiOperationFailed (const juce::String& what)
{
    progressBar.setVisible (false);
    cancelButton.setEnabled (false);
    setStatus (what, true);
}

void SamplesPanel::apiDownloadProgress (const juce::String& name, double fraction,
                                        int index, int total)
{
    progress = fraction;
    progressBar.setVisible (true);
    setStatus ("Downloading " + name.fromLastOccurrenceOf ("/", false, false)
               + "  (" + juce::String (index) + " of " + juce::String (total) + ")  "
               + juce::String ((int) (fraction * 100)) + "%");
}

void SamplesPanel::apiFileDownloaded (const juce::String& path, const dtk::Bytes& data)
{
    juce::String error;
    if (! writeSampleAsWav (path, data, error))
        downloadProblems.add (error);
}

void SamplesPanel::apiDownloadFinished (int ok, int failed)
{
    progressBar.setVisible (false);
    cancelButton.setEnabled (false);
    progress = 0.0;

    const int written = ok - downloadProblems.size();
    juce::String text = juce::String (written) + " sample(s) saved to "
                      + downloadDir.getFileName();
    if (failed > 0) text << ", " << failed << " failed to transfer";
    setStatus (text, failed > 0 || ! downloadProblems.isEmpty());

    if (! downloadProblems.isEmpty())
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon, "Some samples could not be saved",
            downloadProblems.joinIntoString ("\n"));

    downloadProblems.clear();
}

bool SamplesPanel::writeSampleAsWav (const juce::String& drivePath,
                                     const dtk::Bytes& data,
                                     juce::String& errorOut)
{
    const auto leaf = drivePath.fromLastOccurrenceOf ("/", false, false);

    dtk::api::SampleFileInfo info;
    std::string err;
    if (! dtk::api::parseSampleFile (data, info, err))
    {
        errorOut = leaf + ": " + juce::String (err);
        return false;
    }

    auto out = downloadDir.getChildFile (leaf + ".wav");
    out = out.getNonexistentSibling();      // never silently overwrite local work

    std::unique_ptr<juce::FileOutputStream> stream (out.createOutputStream());
    if (stream == nullptr)
    {
        errorOut = leaf + ": could not create " + out.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(), (double) info.sampleRate, 1, 16, {}, 0));
    if (writer == nullptr)
    {
        errorOut = leaf + ": could not create a WAV writer";
        return false;
    }
    stream.release();   // the writer owns it now

    juce::AudioBuffer<float> buffer (1, (int) info.samples.size());
    buffer.copyFrom (0, 0, info.samples.data(), (int) info.samples.size());
    const bool ok = writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    writer.reset();

    if (! ok) { errorOut = leaf + ": writing the WAV failed"; return false; }
    return true;
}

void SamplesPanel::downloadSelected()
{
    if (api.isBusy()) { setStatus ("A transfer is already running.", true); return; }

    std::vector<juce::String> paths;
    juce::StringArray names;
    juce::int64 total = 0;

    for (int i = 0; i < list.getNumSelectedRows(); ++i)
    {
        const int row = list.getSelectedRow (i);
        if (row < 0 || row >= (int) entries.size()) continue;
        const auto& e = entries[(size_t) row];

        // Directories would need a recursive walk; say so rather than skip
        // them silently.
        if (e.isDirectory()) continue;

        paths.push_back (currentPath == "/" ? "/" + juce::String (e.name)
                                            : currentPath + "/" + juce::String (e.name));
        names.add (juce::String (e.name));
        total += (juce::int64) e.size;
    }

    if (paths.empty())
    {
        setStatus ("Select one or more samples first (folders are not "
                   "downloaded recursively).", true);
        return;
    }

    chooser = std::make_unique<juce::FileChooser> (
        "Where should the samples be saved?", juce::File());

    const auto count = paths.size();
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectDirectories,
        [this, paths, count, total] (const juce::FileChooser& fc) mutable
        {
            const auto dir = fc.getResult();
            if (dir == juce::File() || ! dir.isDirectory()) return;

            downloadDir = dir;
            downloadProblems.clear();
            cancelButton.setEnabled (true);
            progressBar.setVisible (true);
            setStatus ("Downloading " + juce::String ((int) count) + " sample(s), "
                       + formatSize (total) + " - this runs at MIDI speed, so "
                       "large samples take a while.");
            api.downloadFiles (std::move (paths));
        });
}

void SamplesPanel::apiStatus (const juce::String& text) { setStatus (text); }
