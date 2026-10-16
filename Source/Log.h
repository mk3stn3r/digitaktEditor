/*
    Log.h - a flushed-on-every-write log, plus a crash handler.

    The point is diagnosing crashes that only happen with real hardware or
    real plugins attached. A buffered log loses exactly the lines that matter,
    so every write is flushed immediately; the cost is irrelevant next to
    losing the last line before a SIGFPE.
*/

#pragma once

#include <juce_core/juce_core.h>

namespace dtklog
{
    /** Opens the log and installs the crash handler. Safe to call twice. */
    void start();

    /** Where the log is written. */
    juce::File file();

    void write (const juce::String& category, const juce::String& message);

    /** Records a stack trace under the given reason, e.g. a caught signal. */
    void backtrace (const juce::String& reason);
}

#define DTK_LOG(category, message)  ::dtklog::write (category, message)
