#pragma once
// =============================================================================
//  AudioEngine.h  -  Conecta el proyecto con la tarjeta de sonido (JUCE),
//  carga archivos de audio y exporta la mezcla.
//
//  Formatos de entrada: WAV, AIFF, FLAC, OGG, MP3 (+ los del sistema operativo).
//  Exportar: WAV 16 / 24 / 32-bit float, con normalizacion opcional.
//  Todo lo pesado (cargar, exportar, renderizar) corre en hilos de fondo y
//  avisa a la UI por el hilo de mensajes.
// =============================================================================
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Model/Project.h"
#include "Transport.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace daw {

class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    AudioEngine (Project& project, ClipLibrary& library, Transport& transport);
    ~AudioEngine() override;

    // Abre la tarjeta de sonido por defecto. Devuelve "" si todo salio bien.
    juce::String initialise();
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager()  { return deviceManager; }
    juce::AudioFormatManager& getFormatManager()  { return formatManager; }
    double getSampleRate() const                  { return currentSampleRate.load(); }
    double getCpuUsage() const                    { return deviceManager.getCpuUsage(); }
    juce::String getSupportedFormatsWildcard() const;   // "*.wav;*.mp3;..."
    bool isSupportedFile (const juce::File& f) const;

    // --- Importar ----------------------------------------------------------------------
    // Carga sincronica (la usa ProjectFile). Devuelve nullptr y llena 'error' si falla.
    std::shared_ptr<AudioData> loadAudioFile (const juce::File& file, juce::String& error);
    // Carga en segundo plano y agrega a la biblioteca. onEach se llama (en el hilo de UI)
    // por cada archivo: con el audio agregado, o con nullptr y el error.
    void importFilesAsync (const juce::Array<juce::File>& files,
                           std::function<void (AudioDataPtr added, juce::String error)> onEach);

    // --- Exportar ----------------------------------------------------------------------
    struct ExportOptions
    {
        double start = 0.0;
        double end = -1.0;            // < 0 = hasta el final del ultimo clip
        double tailSeconds = 2.0;     // para que terminen reverb/delay
        int bitDepth = 24;            // 16, 24 o 32 (float)
        double sampleRate = 0.0;      // 0 = la actual
        bool normalize = false;       // pico a -0.1 dBFS
    };

    void exportMixAsync (const juce::File& destination, ExportOptions options,
                         std::function<void (float progress)> onProgress,
                         std::function<void (bool ok, juce::String message)> onDone);

    // Renderiza un tramo de la mezcla y lo guarda como un clip nuevo en la biblioteca
    void renderToLibraryAsync (double start, double end, juce::String name,
                               std::function<void (AudioDataPtr added, juce::String error)> onDone);

    void cancelBackgroundWork()   { cancelFlag.store (true); }
    bool isBusy() const           { return busyJobs.load() > 0; }

    // Escribe un WAV (lo usa ProjectFile para guardar los audios del proyecto)
    static bool writeWav (const juce::File& file, const float* left, const float* right,
                          int64_t numSamples, double sampleRate, int bitDepth, juce::String& error);

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    void runOnMessageThread (std::function<void()> fn);

    Project& project;
    ClipLibrary& library;
    Transport& transport;

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::ThreadPool pool { 2 };

    std::vector<float> scratchL, scratchR;
    std::atomic<double> currentSampleRate { 48000.0 };
    std::atomic<int> busyJobs { 0 };
    std::atomic<bool> cancelFlag { false };
    std::shared_ptr<std::atomic<bool>> alive;   // evita callbacks tras destruir el motor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace daw
