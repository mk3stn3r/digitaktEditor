/*
    SampleImport.h - turning audio files into Digitakt +Drive samples.

    The Digitakt I sample engine is mono 16-bit at 48 kHz, so anything else has
    to be downmixed and resampled before upload. Doing it here rather than
    making the user pre-convert is the difference between "drag a file in" and
    "open an editor first".
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "ElektronApi.h"

struct PreparedSample
{
    juce::String name;              // +Drive name, no extension
    dtk::Bytes   fileData;          // header + big-endian PCM, ready to upload
    double       seconds { 0.0 };
    int          originalChannels { 0 };
    double       originalRate { 0.0 };
    bool         resampled { false };
    bool         downmixed { false };
    bool         clipped { false };

    size_t sizeBytes() const { return fileData.size(); }
    juce::String describe() const;
};

/** Reads any format JUCE knows and returns mono 48 kHz floats.

    This is the shared front half of both jobs: uploading a sample to the
    +Drive, and loading one into the local pool for offline playback. */
bool loadAudioAsMono48k (const juce::File& file,
                         std::vector<float>& samplesOut,
                         juce::String& errorOut,
                         int* originalChannelsOut = nullptr,
                         double* originalRateOut = nullptr);

/** Reads any format JUCE knows (WAV, AIFF, FLAC, Ogg, MP3) and converts it.
    Returns false and fills `errorOut` on failure. */
bool prepareSampleForUpload (const juce::File& file,
                             PreparedSample& out,
                             juce::String& errorOut,
                             double normaliseToPeak = 0.0);

/** Total sample memory on a Digitakt I, for warning about oversized imports. */
constexpr juce::int64 digitaktSampleMemoryBytes = 64 * 1024 * 1024;
