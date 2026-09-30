#include "AudioEngine.h"
#include "../Model/OfflineRenderer.h"
#include "../Nodes/DSPUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace daw {

AudioEngine::AudioEngine (Project& p, ClipLibrary& lib, Transport& t)
    : project (p), library (lib), transport (t),
      alive (std::make_shared<std::atomic<bool>> (true))
{
    formatManager.registerBasicFormats();   // WAV, AIFF, FLAC, OGG, MP3 (+ los del sistema)
}

AudioEngine::~AudioEngine()
{
    alive->store (false);
    cancelFlag.store (true);
    pool.removeAllJobs (true, 10000);
    shutdown();
}

juce::String AudioEngine::initialise()
{
    const juce::String err = deviceManager.initialiseWithDefaultDevices (0, 2);
    deviceManager.addAudioCallback (this);
    return err;
}

void AudioEngine::shutdown()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

juce::String AudioEngine::getSupportedFormatsWildcard() const
{
    return formatManager.getWildcardForAllFormats();
}

bool AudioEngine::isSupportedFile (const juce::File& f) const
{
    return f.existsAsFile() && formatManager.findFormatForFileExtension (f.getFileExtension()) != nullptr;
}

void AudioEngine::runOnMessageThread (std::function<void()> fn)
{
    auto a = alive;
    juce::MessageManager::callAsync ([a, fn]
    {
        if (a->load()) fn();
    });
}

// =============================================================================
//  Callback de audio
// =============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int block = std::max (16, device->getCurrentBufferSizeSamples());

    scratchL.assign ((size_t) block, 0.0f);
    scratchR.assign ((size_t) block, 0.0f);
    currentSampleRate.store (sr);
    project.prepare (sr, block);
    transport.prepare (sr, block);

    // Si la frecuencia cambio, remuestrear la biblioteca en segundo plano.
    // Mientras tanto el proyecto igual suena (interpola al vuelo).
    bool needsResample = false;
    for (auto& d : library.getAll())
        if (std::abs (d->getSampleRate() - sr) > 0.01) { needsResample = true; break; }

    if (needsResample)
    {
        ++busyJobs;
        pool.addJob ([this, sr]
        {
            auto changes = library.resampleAll (sr);
            project.replaceSources (changes);
            --busyJobs;
        });
    }
}

void AudioEngine::audioDeviceStopped()
{
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    const int cap = (int) scratchL.size();
    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min (cap, numSamples - done);
        float* L = scratchL.data();
        float* R = scratchR.data();
        transport.process (project, L, R, n);

        if (numOutputChannels >= 2)
        {
            if (outputChannelData[0] != nullptr) std::memcpy (outputChannelData[0] + done, L, sizeof (float) * (size_t) n);
            if (outputChannelData[1] != nullptr) std::memcpy (outputChannelData[1] + done, R, sizeof (float) * (size_t) n);
        }
        else if (numOutputChannels == 1 && outputChannelData[0] != nullptr)
        {
            for (int i = 0; i < n; ++i) outputChannelData[0][done + i] = 0.5f * (L[i] + R[i]);
        }
        done += n;
    }

    for (int ch = 2; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            std::memset (outputChannelData[ch], 0, sizeof (float) * (size_t) numSamples);
}

// =============================================================================
//  Importar
// =============================================================================
std::shared_ptr<AudioData> AudioEngine::loadAudioFile (const juce::File& file, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "No se pudo leer \"" + file.getFileName() + "\" (formato no soportado o archivo danado).";
        return nullptr;
    }

    const juce::int64 len = reader->lengthInSamples;
    const double sr = reader->sampleRate;
    if (len <= 0 || sr <= 0.0)
    {
        error = "\"" + file.getFileName() + "\" esta vacio.";
        return nullptr;
    }
    if ((double) len / sr > 3.0 * 3600.0 || len > (juce::int64) std::numeric_limits<int>::max())
    {
        error = "\"" + file.getFileName() + "\" es demasiado largo (maximo 3 horas).";
        return nullptr;
    }

    const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    juce::AudioBuffer<float> buffer (channels, (int) len);
    buffer.clear();
    reader->read (&buffer, 0, (int) len, 0, true, channels > 1);

    std::vector<float> l (buffer.getReadPointer (0), buffer.getReadPointer (0) + len);
    std::vector<float> r;
    if (channels > 1) r.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + len);
    else              r = l;

    return std::make_shared<AudioData> (file.getFileNameWithoutExtension().toStdString(),
                                        file.getFullPathName().toStdString(),
                                        sr, std::move (l), std::move (r));
}

void AudioEngine::importFilesAsync (const juce::Array<juce::File>& files,
                                    std::function<void (AudioDataPtr, juce::String)> onEach)
{
    for (auto& file : files)
    {
        ++busyJobs;
        pool.addJob ([this, file, onEach]
        {
            juce::String err;
            AudioDataPtr added;
            if (auto data = loadAudioFile (file, err))
                added = library.add (data, currentSampleRate.load());

            runOnMessageThread ([onEach, added, err] { if (onEach) onEach (added, err); });
            --busyJobs;
        });
    }
}

// =============================================================================
//  Exportar
// =============================================================================
bool AudioEngine::writeWav (const juce::File& file, const float* left, const float* right,
                            int64_t numSamples, double sampleRate, int bitDepth, juce::String& error)
{
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr || stream->failedToOpen())
    {
        error = "No se pudo crear " + file.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, 2,
                                                                          bitDepth, {}, 0));
    if (writer == nullptr)
    {
        error = "No se pudo crear el escritor WAV.";
        return false;
    }
    stream.release();   // ahora el writer es dueno del stream

    constexpr int kChunk = 65536;
    for (int64_t pos = 0; pos < numSamples; pos += kChunk)
    {
        const int n = (int) std::min<int64_t> (kChunk, numSamples - pos);
        const float* chans[2] = { left + pos, right + pos };
        if (! writer->writeFromFloatArrays (chans, 2, n))
        {
            error = "Error al escribir " + file.getFullPathName();
            return false;
        }
    }
    return true;
}

void AudioEngine::exportMixAsync (const juce::File& destination, ExportOptions opt,
                                  std::function<void (float)> onProgress,
                                  std::function<void (bool, juce::String)> onDone)
{
    cancelFlag.store (false);
    ++busyJobs;

    pool.addJob ([this, destination, opt, onProgress, onDone]
    {
        auto finish = [this, onDone] (bool ok, juce::String msg)
        {
            runOnMessageThread ([onDone, ok, msg] { if (onDone) onDone (ok, msg); });
            --busyJobs;
        };

        const double sr = opt.sampleRate > 0.0 ? opt.sampleRate : currentSampleRate.load();
        const double start = std::max (0.0, opt.start);
        const double end = opt.end >= 0.0 ? opt.end : project.getLength() + std::max (0.0, opt.tailSeconds);
        if (end <= start + 0.001)
        {
            finish (false, "No hay nada que exportar (el proyecto esta vacio).");
            return;
        }

        const bool twoPasses = opt.normalize;
        auto report = [this, onProgress] (float p)
        {
            runOnMessageThread ([onProgress, p] { if (onProgress) onProgress (p); });
        };

        // Pasada 1 (solo si se normaliza): buscar el pico
        float gain = 1.0f;
        if (twoPasses)
        {
            const float peak = OfflineRenderer::findPeak (project, sr, start, end,
                                                          [&report] (float p) { report (p * 0.5f); }, &cancelFlag);
            if (cancelFlag.load()) { finish (false, "Exportacion cancelada."); return; }
            if (peak > 1.0e-9f) gain = dsp::dbToGain (-0.1f) / peak;
        }

        // Abrir el archivo
        const int bits = (opt.bitDepth == 16 || opt.bitDepth == 32) ? opt.bitDepth : 24;
        destination.deleteFile();
        std::unique_ptr<juce::FileOutputStream> stream (destination.createOutputStream());
        if (stream == nullptr || stream->failedToOpen())
        {
            finish (false, "No se pudo crear " + destination.getFullPathName());
            return;
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sr, 2, bits, {}, 0));
        if (writer == nullptr)
        {
            finish (false, "No se pudo crear el archivo WAV.");
            return;
        }
        stream.release();

        std::vector<float> bl, br;
        const bool clip = bits != 32;
        const bool ok = OfflineRenderer::render (project, sr, start, end,
            [&] (const float* l, const float* r, int n)
            {
                bl.assign (l, l + n);
                br.assign (r, r + n);
                for (int i = 0; i < n; ++i)
                {
                    bl[(size_t) i] *= gain; br[(size_t) i] *= gain;
                    if (clip)
                    {
                        bl[(size_t) i] = juce::jlimit (-1.0f, 1.0f, bl[(size_t) i]);
                        br[(size_t) i] = juce::jlimit (-1.0f, 1.0f, br[(size_t) i]);
                    }
                }
                const float* chans[2] = { bl.data(), br.data() };
                return writer->writeFromFloatArrays (chans, 2, n);
            },
            [&] (float p) { report (twoPasses ? 0.5f + p * 0.5f : p); },
            &cancelFlag);

        writer.reset();   // cierra el archivo

        if (! ok)
        {
            destination.deleteFile();
            finish (false, cancelFlag.load() ? "Exportacion cancelada." : "Error al escribir el archivo.");
            return;
        }

        const double secs = end - start;
        finish (true, "Exportado: " + destination.getFileName() + " ("
                          + juce::String (secs, 1) + " s, " + juce::String (bits) + "-bit, "
                          + juce::String ((int) sr) + " Hz)");
    });
}

void AudioEngine::renderToLibraryAsync (double start, double end, juce::String name,
                                        std::function<void (AudioDataPtr, juce::String)> onDone)
{
    cancelFlag.store (false);
    ++busyJobs;

    pool.addJob ([this, start, end, name, onDone]
    {
        const double sr = currentSampleRate.load();
        std::vector<float> l, r;
        juce::String err;
        AudioDataPtr added;

        if (end <= start + 0.001)
            err = "Selecciona un rango valido para renderizar.";
        else
        {
            const auto expected = (size_t) std::llround ((end - start) * sr);
            l.reserve (expected);
            r.reserve (expected);
            const bool ok = OfflineRenderer::render (project, sr, start, end,
                [&] (const float* bl, const float* br, int n)
                {
                    l.insert (l.end(), bl, bl + n);
                    r.insert (r.end(), br, br + n);
                    return true;
                }, {}, &cancelFlag);

            if (! ok) err = "Render cancelado.";
            else
                added = library.add (std::make_shared<AudioData> (name.toStdString(), std::string(), sr,
                                                                  std::move (l), std::move (r)), sr);
        }

        runOnMessageThread ([onDone, added, err] { if (onDone) onDone (added, err); });
        --busyJobs;
    });
}

} // namespace daw
