#include "InstrumentsPanel.h"

#include "Log.h"

namespace
{
    const juce::Colour bgColour   { 0xff1b1d21 };
    const juce::Colour rowAlt     { 0xff212429 };
    const juce::Colour textCol    { 0xffd8dee6 };
    const juce::Colour textDim    { 0xff7d858f };
    const juce::Colour usedColour { 0xff7fc7a8 };
    const juce::Colour errColour  { 0xffe07a5f };

    /** An editor window that lets the plugin go when it is closed. */
    class PluginWindow : public juce::DocumentWindow
    {
    public:
        PluginWindow (juce::AudioPluginInstance& p, const juce::String& title,
                      bool forceGeneric, std::function<void()> onCloseIn)
            : DocumentWindow (title, juce::Colour (0xff16181c), closeButton),
              onClose (std::move (onCloseIn))
        {
            setUsingNativeTitleBar (true);

            juce::AudioProcessorEditor* editor = nullptr;
            if (! forceGeneric) editor = p.createEditorIfNeeded();

            const bool generic = (editor == nullptr);
            if (generic) editor = new juce::GenericAudioProcessorEditor (p);

            // `true` makes the window adopt the editor's own size. That size
            // is the plugin's business, not ours.
            setContentOwned (editor, true);

            // Only make the window resizable if the editor actually is. A
            // non-resizable VST3 view that gets resized anyway is told its
            // dimensions are something it never agreed to, and plugins do
            // layout arithmetic on those numbers.
            const bool resizable = generic || editor->isResizable();
            setResizable (resizable, false);

            if (resizable)
                if (auto* constrainer = editor->getConstrainer())
                    setResizeLimits (juce::jmax (1, constrainer->getMinimumWidth()),
                                     juce::jmax (1, constrainer->getMinimumHeight()),
                                     constrainer->getMaximumWidth(),
                                     constrainer->getMaximumHeight());

            // Never force a minimum: only rescue a genuinely degenerate size,
            // which would otherwise be a zero to divide by.
            const int w = getWidth()  > 0 ? getWidth()  : 400;
            const int h = getHeight() > 0 ? getHeight() : 200;

            DTK_LOG ("plugin", title + " editor: "
                     + (generic ? juce::String ("generic") : juce::String ("native"))
                     + " " + juce::String (w) + "x" + juce::String (h)
                     + (resizable ? " resizable" : " fixed size"));

            centreWithSize (w, h);
            setVisible (true);
        }

        /** Destroying the window destroys the editor, which is what tells the
            plugin its editor is gone. Merely hiding it leaves the plugin
            believing an editor is still attached. */
        void closeButtonPressed() override { if (onClose) onClose(); }

    private:
        std::function<void()> onClose;
    };
}

InstrumentsPanel::InstrumentsPanel (PluginRack& r) : rack (r)
{
    formats.addDefaultFormats();
    loadKnownPlugins();
    known.addChangeListener (this);

    addAndMakeVisible (list);
    list.setColour (juce::ListBox::backgroundColourId, bgColour);
    list.setRowHeight (26);

    auto add = [this] (juce::TextButton& b, std::function<void()> fn)
    {
        addAndMakeVisible (b);
        b.onClick = std::move (fn);
    };
    add (scanButton,   [this] { scanForPlugins(); });
    add (assignButton, [this] { assignToSelected(); });
    add (clearButton,  [this] { clearSelected(); });
    add (editorButton, [this] { openEditor (list.getSelectedRow()); });

    addAndMakeVisible (header);
    header.setColour (juce::Label::textColourId, textCol);
    header.setFont (juce::FontOptions (13.0f, juce::Font::bold));

    addAndMakeVisible (hint);
    hint.setColour (juce::Label::textColourId, textDim);
    hint.setFont (juce::FontOptions (11.5f));
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setText (
        "The Digitakt's eight MIDI tracks send notes to external gear and make no "
        "sound themselves. Assign an instrument here and M1-M8 become playable "
        "parts of the same pattern, sequenced by the same trigs.\n\n"
        "Scan once; the list is remembered. Double-click a track to open its "
        "plugin editor. One note per step per track, as the grid stores - but "
        "live playing from the Digitakt below is not limited that way.",
        juce::dontSendNotification);

    addAndMakeVisible (genericEditorToggle);
    genericEditorToggle.setColour (juce::ToggleButton::textColourId, textCol);
    genericEditorToggle.setTooltip (
        "Opens a plain list of the plugin's parameters instead of its own "
        "interface. Useful when a plugin's GUI is unstable: the parameters "
        "still work.");

    addAndMakeVisible (playFromDigitakt);
    playFromDigitakt.setColour (juce::ToggleButton::textColourId, textCol);
    playFromDigitakt.onClick = [this]
    {
        baseChannelSlider.setEnabled (playFromDigitakt.getToggleState());
        if (listener != nullptr)
            listener->externalMidiChanged (playFromDigitakt.getToggleState(),
                                           (int) baseChannelSlider.getValue());
    };

    addAndMakeVisible (baseChannelLabel);
    baseChannelLabel.setColour (juce::Label::textColourId, textDim);
    baseChannelLabel.setFont (juce::FontOptions (12.0f));

    addAndMakeVisible (baseChannelSlider);
    baseChannelSlider.setRange (1, 16, 1);
    baseChannelSlider.setValue (9, juce::dontSendNotification);
    baseChannelSlider.setEnabled (false);
    baseChannelSlider.onValueChange = [this]
    {
        if (listener != nullptr)
            listener->externalMidiChanged (playFromDigitakt.getToggleState(),
                                           (int) baseChannelSlider.getValue());
    };

    addAndMakeVisible (pluginAudioToggle);
    pluginAudioToggle.setToggleState (true, juce::dontSendNotification);
    pluginAudioToggle.setColour (juce::ToggleButton::textColourId, textCol);
    pluginAudioToggle.onClick = [this]
    {
        if (listener != nullptr)
            listener->pluginAudioChanged (pluginAudioToggle.getToggleState());
    };

    addAndMakeVisible (sampleAudioToggle);
    sampleAudioToggle.setToggleState (true, juce::dontSendNotification);
    sampleAudioToggle.setColour (juce::ToggleButton::textColourId, textCol);
    sampleAudioToggle.onClick = [this]
    {
        if (listener != nullptr)
            listener->sampleAudioChanged (sampleAudioToggle.getToggleState());
    };

    addAndMakeVisible (routingLabel);
    routingLabel.setColour (juce::Label::textColourId, textDim);
    routingLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));

    addAndMakeVisible (routingHelp);
    routingHelp.setColour (juce::Label::textColourId, textDim);
    routingHelp.setFont (juce::FontOptions (11.5f));
    routingHelp.setJustificationType (juce::Justification::topLeft);
    routingHelp.setText (
        "To resample an instrument into one of the Digitakt's eight audio tracks:\n"
        "  1. Digitakt: SETTINGS > SYSTEM > USB CONFIG = USB AUDIO/MIDI.\n"
        "     This needs OS 1.20 or later; plain USB MIDI carries no audio.\n"
        "  2. Press Audio... in the toolbar and pick the Digitakt as the output\n"
        "     device. Its USB input is mixed into Main - check the level under\n"
        "     SETTINGS > AUDIO ROUTING if you hear nothing.\n"
        "  3. Turn 'Sample tracks to the output' off, so only the instrument\n"
        "     reaches the machine and the recording is clean.\n"
        "  4. Digitakt: SAMPLING, set SOURCE to MAIN L+R, arm and play.\n\n"
        "The Digitakt's own analog INPUT L/R jacks are not routed to Main over "
        "USB, but audio arriving from the computer is - which is why MAIN L+R "
        "is the source that works here.",
        juce::dontSendNotification);

    setAudioOutputName ({});
    refresh();
}

void InstrumentsPanel::setAudioOutputName (const juce::String& name)
{
    const bool looksLikeDigitakt = name.containsIgnoreCase ("digitakt")
                                || name.containsIgnoreCase ("elektron");
    routingLabel.setText (
        name.isEmpty() ? "Audio output: none"
                       : juce::String ("Audio output: ") + name
                             + (looksLikeDigitakt ? "  - instrument audio is going "
                                                    "to the Digitakt"
                                                  : "  - not the Digitakt"),
        juce::dontSendNotification);
    routingLabel.setColour (juce::Label::textColourId,
                            looksLikeDigitakt ? usedColour : textDim);
}

InstrumentsPanel::~InstrumentsPanel()
{
    known.removeChangeListener (this);
    // Editors must go before the plugins they belong to.
    for (auto& w : editors) w.reset();
}

//==============================================================================
juce::File InstrumentsPanel::settingsFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("digitakt-editor").getChildFile ("plugins.xml");
}

void InstrumentsPanel::loadKnownPlugins()
{
    const auto file = settingsFile();
    if (! file.existsAsFile()) return;
    if (auto xml = juce::XmlDocument::parse (file))
        known.recreateFromXml (*xml);
}

void InstrumentsPanel::saveKnownPlugins()
{
    const auto file = settingsFile();
    file.getParentDirectory().createDirectory();
    if (auto xml = std::unique_ptr<juce::XmlElement> (known.createXml()))
        xml->writeTo (file);
}

void InstrumentsPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    saveKnownPlugins();
}

//==============================================================================
void InstrumentsPanel::setTracksInUse (const std::vector<int>& used)
{
    tracksInUse = used;
    list.repaint();
}

void InstrumentsPanel::refresh()
{
    header.setText ("MIDI track instruments - " + juce::String (rack.numLoaded())
                    + " of 8 loaded, " + juce::String (known.getNumTypes())
                    + " plugin(s) known", juce::dontSendNotification);
    list.updateContent();
    list.repaint();
}

void InstrumentsPanel::paint (juce::Graphics& g) { g.fillAll (bgColour); }

void InstrumentsPanel::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto toolbar = area.removeFromTop (28);
    scanButton  .setBounds (toolbar.removeFromLeft (150).reduced (2));
    assignButton.setBounds (toolbar.removeFromLeft (140).reduced (2));
    clearButton .setBounds (toolbar.removeFromLeft (100).reduced (2));
    editorButton.setBounds (toolbar.removeFromLeft (110).reduced (2));
    toolbar.removeFromLeft (10);
    genericEditorToggle.setBounds (toolbar.removeFromLeft (230));

    area.removeFromTop (6);
    header.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);
    list.setBounds (area.removeFromTop (PluginRack::numSlots * 26 + 4));

    area.removeFromTop (10);
    hint.setBounds (area.removeFromTop (40));
    area.removeFromTop (8);

    playFromDigitakt.setBounds (area.removeFromTop (24));
    auto chRow = area.removeFromTop (28).withTrimmedLeft (22);
    baseChannelLabel .setBounds (chRow.removeFromLeft (90));
    baseChannelSlider.setBounds (chRow.removeFromLeft (150).reduced (2));

    area.removeFromTop (8);
    auto audioRow = area.removeFromTop (24);
    pluginAudioToggle.setBounds (audioRow.removeFromLeft (260));
    sampleAudioToggle.setBounds (audioRow.removeFromLeft (260));

    area.removeFromTop (10);
    routingLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (2);
    routingHelp.setBounds (area);
}

//==============================================================================
int InstrumentsPanel::getNumRows() { return PluginRack::numSlots; }

void InstrumentsPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h,
                                         bool selected)
{
    if (row < 0 || row >= PluginRack::numSlots) return;

    const bool inUse = std::find (tracksInUse.begin(), tracksInUse.end(), row)
                           != tracksInUse.end();

    g.fillAll (selected ? rowAlt.brighter (0.25f) : (row % 2 ? rowAlt : bgColour));

    g.setColour (inUse ? usedColour : textDim);
    g.setFont (juce::FontOptions (12.0f, inUse ? juce::Font::bold : juce::Font::plain));
    g.drawText ("M" + juce::String (row + 1), 8, 0, 40, h,
                juce::Justification::centredLeft);

    const auto name = rack.slotName (row);
    g.setColour (name.isEmpty() ? textDim.withAlpha (0.5f) : textCol);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText (name.isEmpty() ? "(no instrument)" : name, 52, 0, w - 130, h,
                juce::Justification::centredLeft);

    if (inUse)
    {
        g.setColour (usedColour.withAlpha (0.8f));
        g.setFont (juce::FontOptions (10.5f));
        g.drawText ("has trigs", w - 74, 0, 66, h, juce::Justification::centredRight);
    }
}

void InstrumentsPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (rack.slotLoaded (row)) openEditor (row);
    else assignToSelected();
}

//==============================================================================
void InstrumentsPanel::scanForPlugins()
{
    auto* content = new juce::PluginListComponent (formats, known,
                                                   settingsFile().getSiblingFile ("deadmans.txt"),
                                                   nullptr, true);
    content->setSize (620, 440);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content);
    options.dialogTitle = "Available instruments";
    options.dialogBackgroundColour = bgColour;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

void InstrumentsPanel::assignToSelected()
{
    const int slot = juce::jmax (0, list.getSelectedRow());

    if (known.getNumTypes() == 0)
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon, "No plugins known yet",
            "Press 'Scan for plugins...' first.\n\n"
            "This build hosts VST3 and LV2. On Linux the usual places are "
            "~/.vst3 and /usr/lib/vst3 for VST3, and ~/.lv2 and /usr/lib/lv2 "
            "for LV2.");
        return;
    }

    juce::PopupMenu menu;
    juce::Array<juce::PluginDescription> instruments;

    for (const auto& type : known.getTypes())
        if (type.isInstrument) instruments.add (type);

    if (instruments.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon, "No instruments found",
            juce::String (known.getNumTypes()) + " plugin(s) are known, but none "
            "of them is an instrument. Effects cannot be sequenced from a MIDI "
            "track, so only instruments are offered here.");
        return;
    }

    for (int i = 0; i < instruments.size(); ++i)
        menu.addItem (i + 1, instruments[i].name + "  (" + instruments[i].pluginFormatName + ")");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (assignButton),
        [this, slot, instruments] (int result)
        {
            if (result <= 0 || result > instruments.size()) return;

            // The outgoing plugin is about to be destroyed; its editor cannot
            // outlive it.
            closeEditor (slot);

            juce::String error;
            if (rack.loadPlugin (slot, instruments[result - 1], formats, error))
            {
                refresh();
                if (listener != nullptr)
                {
                    listener->instrumentsChanged();
                    listener->statusMessage ("M" + juce::String (slot + 1) + " -> "
                                             + instruments[result - 1].name, false);
                }
            }
            else if (listener != nullptr)
            {
                DTK_LOG ("plugin", "load failed: " + error);
                listener->statusMessage ("could not load "
                                         + instruments[result - 1].name + ": " + error,
                                         true);
            }
        });
}

void InstrumentsPanel::clearSelected()
{
    const int slot = list.getSelectedRow();
    if (slot < 0) return;

    // Only this slot's editor, and before the plugin behind it goes away.
    closeEditor (slot);
    rack.clearSlot (slot);
    refresh();
    if (listener != nullptr) listener->instrumentsChanged();
}

void InstrumentsPanel::closeEditor (int slot)
{
    if (slot < 0 || slot >= PluginRack::numSlots) return;
    if (editors[(size_t) slot] == nullptr) return;

    DTK_LOG ("plugin", "closing editor for M" + juce::String (slot + 1));
    editors[(size_t) slot].reset();
}

void InstrumentsPanel::openEditor (int slot)
{
    auto* plugin = rack.instance (slot);
    if (plugin == nullptr)
    {
        if (listener != nullptr)
            listener->statusMessage ("M" + juce::String (slot + 1)
                                     + " has no instrument assigned.", true);
        return;
    }
    // Reuse the existing window: opening a second one would hand the very
    // same editor object to two owners.
    if (auto& existing = editors[(size_t) slot])
    {
        existing->setVisible (true);
        existing->toFront (true);
        return;
    }

    DTK_LOG ("plugin", "opening editor for M" + juce::String (slot + 1)
             + " (" + rack.slotName (slot) + ")");

    juce::Component::SafePointer<InstrumentsPanel> safe (this);
    editors[(size_t) slot] = std::make_unique<PluginWindow> (
        *plugin,
        "M" + juce::String (slot + 1) + " - " + rack.slotName (slot),
        genericEditorToggle.getToggleState(),
        [safe, slot]
        {
            // Deleting the window from inside its own callback is not safe, so
            // it is deferred.
            juce::MessageManager::callAsync ([safe, slot]
            {
                if (auto* self = safe.getComponent()) self->closeEditor (slot);
            });
        });
}
