/*  Tests for audio import: downmix, resample, and conversion to the +Drive
    sample format. Needs JUCE for the audio file readers, so it is a separate
    target from the JUCE-free core and API tests.
*/

#include <juce_audio_formats/juce_audio_formats.h>

#include "SampleImport.h"

#include <cmath>
#include <iostream>

static int failures = 0, checks = 0;

static void check (bool ok, const juce::String& what)
{
    ++checks;
    if (ok) std::cout << "  PASS  " << what << "\n";
    else  { std::cout << "  FAIL  " << what << "\n"; ++failures; }
}

/** Writes a test WAV: `seconds` of a sine at the given rate and channel count. */
static juce::File makeWav (const juce::String& name, double rate, int channels,
                           double seconds, float amplitude = 0.5f)
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (name);
    file.deleteFile();

    const int frames = (int) (rate * seconds);
    juce::AudioBuffer<float> buf (channels, frames);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < frames; ++i)
            // Give each channel a different phase so a downmix bug shows up.
            buf.setSample (ch, i, amplitude * std::sin ((float) i * 0.01f
                                                        + (float) ch * 1.0f));

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.release(), rate, (unsigned int) channels, 16, {}, 0));
    writer->writeFromAudioSampleBuffer (buf, 0, frames);
    writer.reset();
    return file;
}

int main()
{
    // --- a 48 kHz mono file needs no conversion ----------------------------
    {
        auto f = makeWav ("dtk_mono48.wav", 48000, 1, 1.0);
        PreparedSample s;
        juce::String err;
        check (prepareSampleForUpload (f, s, err), "48 kHz mono WAV imports");
        check (! s.resampled && ! s.downmixed, "and needs neither resampling nor downmix");
        check (s.name == "dtk_mono48", "name strips the extension");
        check (std::abs (s.seconds - 1.0) < 0.01, "duration is preserved");
        check (s.sizeBytes() == 64 + 48000 * 2,
               "file is the 64-byte header plus one second of 16-bit mono");
        f.deleteFile();
    }

    // --- 44.1 kHz stereo needs both ----------------------------------------
    {
        auto f = makeWav ("dtk_st441.wav", 44100, 2, 2.0);
        PreparedSample s;
        juce::String err;
        check (prepareSampleForUpload (f, s, err), "44.1 kHz stereo WAV imports");
        check (s.resampled && s.downmixed, "it is resampled and downmixed");
        check (s.originalChannels == 2 && (int) s.originalRate == 44100,
               "the original format is reported");
        check (std::abs (s.seconds - 2.0) < 0.02,
               "resampling preserves duration (not just sample count)");

        // 2 s at 48 kHz = 96000 frames, so 192000 bytes of PCM.
        const size_t expected = 64 + 96000 * 2;
        check (s.sizeBytes() > expected - 400 && s.sizeBytes() < expected + 400,
               "output length matches 48 kHz for the same duration");
        f.deleteFile();
    }

    // --- 96 kHz downsamples ------------------------------------------------
    {
        auto f = makeWav ("dtk_96k.wav", 96000, 1, 0.5);
        PreparedSample s;
        juce::String err;
        check (prepareSampleForUpload (f, s, err) && s.resampled,
               "96 kHz downsamples to 48 kHz");
        check (std::abs (s.seconds - 0.5) < 0.01, "and keeps its duration");
        f.deleteFile();
    }

    // --- the header the device will read -----------------------------------
    {
        auto f = makeWav ("dtk_hdr.wav", 48000, 1, 0.25);
        PreparedSample s;
        juce::String err;
        prepareSampleForUpload (f, s, err);

        const auto& d = s.fileData;
        auto be32 = [&] (size_t o) { return ((uint32_t) d[o] << 24) | ((uint32_t) d[o+1] << 16)
                                          | ((uint32_t) d[o+2] << 8) | (uint32_t) d[o+3]; };
        const uint32_t frames = 12000;
        check (d[0] == 0 && be32 (4) == frames * 2 && be32 (8) == 48000
               && be32 (12) == 0 && be32 (16) == frames - 1 && d[20] == 0x7F,
               "header fields are correct for a real imported file");
        f.deleteFile();
    }

    // --- failures are reported ---------------------------------------------
    {
        auto notAudio = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("dtk_notaudio.wav");
        notAudio.replaceWithText ("this is not a wav file");
        PreparedSample s;
        juce::String err;
        check (! prepareSampleForUpload (notAudio, s, err) && err.isNotEmpty(),
               "a non-audio file is rejected with a message");
        notAudio.deleteFile();

        PreparedSample s2;
        juce::String err2;
        check (! prepareSampleForUpload (juce::File ("/nonexistent/x.wav"), s2, err2),
               "a missing file is rejected");
    }

    std::cout << "\n" << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
