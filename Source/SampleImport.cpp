#include "SampleImport.h"

juce::String PreparedSample::describe() const
{
    juce::StringArray bits;
    bits.add (juce::String (seconds, 2) + " s");
    bits.add (juce::String ((int) (sizeBytes() / 1024)) + " KB");
    if (downmixed) bits.add (juce::String (originalChannels) + " ch -> mono");
    if (resampled) bits.add (juce::String ((int) originalRate) + " -> 48000 Hz");
    if (clipped)   bits.add ("peaks clipped");
    return bits.joinIntoString (", ");
}

bool loadAudioAsMono48k (const juce::File& file,
                         std::vector<float>& samplesOut,
                         juce::String& errorOut,
                         int* originalChannelsOut,
                         double* originalRateOut)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        errorOut = file.getFileName() + ": not an audio file this build can read "
                   "(WAV, AIFF, FLAC, Ogg and MP3 are supported)";
        return false;
    }

    const auto numFrames = (int) reader->lengthInSamples;
    if (numFrames <= 0) { errorOut = file.getFileName() + ": contains no audio"; return false; }

    const int numChannels = (int) reader->numChannels;
    if (originalChannelsOut != nullptr) *originalChannelsOut = numChannels;
    if (originalRateOut != nullptr)     *originalRateOut = reader->sampleRate;

    juce::AudioBuffer<float> source ((int) numChannels, numFrames);
    if (! reader->read (&source, 0, numFrames, 0, true, true))
    {
        errorOut = file.getFileName() + ": could not be decoded";
        return false;
    }

    // Downmix to mono by averaging, which keeps the level sane rather than
    // summing into clipping.
    bool downmixed = false, resampled = false;
    juce::AudioBuffer<float> mono (1, numFrames);
    if (numChannels == 1)
    {
        mono.copyFrom (0, 0, source, 0, 0, numFrames);
    }
    else
    {
        mono.clear();
        for (int ch = 0; ch < numChannels; ++ch)
            mono.addFrom (0, 0, source, ch, 0, numFrames, 1.0f / (float) numChannels);
        downmixed = true;
    }

    // Resample to 48 kHz if needed.
    std::vector<float> samples;
    if (std::abs (reader->sampleRate - (double) dtk::api::digitaktSampleRate) > 0.5
        && reader->sampleRate > 0.0)
    {
        const double ratio = reader->sampleRate / (double) dtk::api::digitaktSampleRate;
        const int outFrames = (int) std::floor ((double) numFrames / ratio);
        if (outFrames <= 0) { errorOut = file.getFileName() + ": too short to resample"; return false; }

        juce::LagrangeInterpolator interp;
        std::vector<float> resampledData ((size_t) outFrames, 0.0f);
        interp.process (ratio, mono.getReadPointer (0), resampledData.data(), outFrames);
        samples = std::move (resampledData);
        resampled = true;
    }
    else
    {
        samples.assign (mono.getReadPointer (0), mono.getReadPointer (0) + numFrames);
    }

    samplesOut = std::move (samples);
    juce::ignoreUnused (downmixed, resampled);
    return true;
}

bool prepareSampleForUpload (const juce::File& file,
                             PreparedSample& out,
                             juce::String& errorOut,
                             double normaliseToPeak)
{
    std::vector<float> samples;
    if (! loadAudioAsMono48k (file, samples, errorOut,
                              &out.originalChannels, &out.originalRate))
        return false;

    out.name      = dtk::api::sanitiseSampleName (file.getFileName().toStdString());
    out.downmixed = out.originalChannels > 1;
    out.resampled = std::abs (out.originalRate - (double) dtk::api::digitaktSampleRate) > 0.5;

    float peak = 0.0f;
    for (float f : samples) peak = juce::jmax (peak, std::abs (f));

    if (normaliseToPeak > 0.0 && peak > 0.0f)
    {
        const float gain = (float) normaliseToPeak / peak;
        for (auto& f : samples) f *= gain;
    }
    else if (peak > 1.0f)
    {
        out.clipped = true;   // buildSampleFile clamps; say so rather than hide it
    }

    out.seconds  = (double) samples.size() / (double) dtk::api::digitaktSampleRate;
    out.fileData = dtk::api::buildSampleFile (samples);
    return true;
}
