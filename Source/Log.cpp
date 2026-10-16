#include "Log.h"

#include <csignal>

namespace dtklog
{

namespace
{
    juce::CriticalSection logLock;
    std::unique_ptr<juce::FileOutputStream> stream;
    bool started = false;

    juce::File defaultFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                  .getChildFile ("digitakt-editor")
                  .getChildFile ("digitakt-editor.log");
    }

    void crashHandler (void*)
    {
        // Deliberately minimal: this runs from a signal handler, so it does as
        // little as possible beyond getting the trace onto disk.
        backtrace ("CRASH - fatal signal");
        if (stream != nullptr) stream->flush();
    }
}

juce::File file() { return defaultFile(); }

void start()
{
    const juce::ScopedLock sl (logLock);
    if (started) return;
    started = true;

    auto f = defaultFile();
    f.getParentDirectory().createDirectory();

    // Keep one previous run, so a crash report is not overwritten by the
    // restart that follows it.
    if (f.existsAsFile())
        f.copyFileTo (f.getSiblingFile ("digitakt-editor.previous.log"));
    f.deleteFile();

    stream = std::make_unique<juce::FileOutputStream> (f);

    juce::SystemStats::setApplicationCrashHandler (crashHandler);

    write ("start", "Digitakt Pattern Editor log");
    write ("start", "time        " + juce::Time::getCurrentTime().toString (true, true));
    write ("start", "os          " + juce::SystemStats::getOperatingSystemName());
    write ("start", "cpu         " + juce::String (juce::SystemStats::getNumCpus())
                                   + " cores, "
                                   + juce::String (juce::SystemStats::getMemorySizeInMegabytes())
                                   + " MB");
    write ("start", "log file    " + f.getFullPathName());
}

void write (const juce::String& category, const juce::String& message)
{
    const auto line = juce::Time::getCurrentTime().toString (false, true, true, true)
                    + "  " + category.paddedRight (' ', 10) + "  " + message;

    const juce::ScopedLock sl (logLock);
    if (stream != nullptr)
    {
        stream->writeText (line + juce::newLine, false, false, nullptr);
        stream->flush();          // a buffered log loses the crash
    }
    std::cerr << line << std::endl;
}

void backtrace (const juce::String& reason)
{
    write ("trace", reason);
    write ("trace", juce::SystemStats::getStackBacktrace());
}

} // namespace dtklog
