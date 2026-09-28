/*
    Main.cpp - application entry point.
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include "MainComponent.h"

class DigitaktEditorApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Digitakt Pattern Editor"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String& commandLine) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());

        // Opening a .syx straight from the command line makes the app usable
        // from a file manager and from scripts.
        auto args = juce::ArgumentList (getApplicationName(), commandLine);
        for (const auto& a : args.arguments)
        {
            const auto file = a.resolveAsExistingFile();
            if (file.hasFileExtension ("syx"))
            {
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->loadFile (file);
                break;
            }
        }
    }

    void shutdown() override { mainWindow = nullptr; }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              juce::Colour (0xff16181c),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (900, 520, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (DigitaktEditorApplication)
