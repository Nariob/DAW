#pragma once
// =============================================================================
//  AudioData.h  -  Un audio cargado en memoria (estereo, float).
//  Lo comparten la biblioteca de clips y todos los clips que lo usan
//  (std::shared_ptr<const AudioData>), asi que nunca se copian los samples.
//  Incluye un resumen de picos para dibujar la forma de onda rapido.
// =============================================================================
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {

class AudioData
{
public:
    static constexpr int kPeakBlock = 256;   // muestras por punto del resumen de forma de onda

    AudioData (std::string name, std::string filePath, double sampleRate,
               std::vector<float> left, std::vector<float> right);

    uint32_t id = 0;            // lo asigna ClipLibrary
    std::string name;
    std::string filePath;       // archivo original ("" si se creo dentro del programa)

    double getSampleRate() const   { return sampleRate; }
    int64_t getNumSamples() const  { return (int64_t) left.size(); }
    double getLengthSeconds() const { return sampleRate > 0.0 ? (double) left.size() / sampleRate : 0.0; }

    const float* getLeft() const   { return left.data(); }
    const float* getRight() const  { return right.data(); }

    // Resumen de forma de onda (min/max de ambos canales por bloque)
    const std::vector<float>& getPeakMin() const { return peakMin; }
    const std::vector<float>& getPeakMax() const { return peakMax; }
    // min/max real entre dos muestras (usa el resumen si el rango es grande)
    void getMinMax (int64_t startSample, int64_t endSample, float& mn, float& mx) const;

    // Copia remuestreada a otra frecuencia (sinc con ventana, alta calidad)
    std::shared_ptr<AudioData> resampled (double newSampleRate) const;
    // Copia de un tramo [start, end) en muestras; opcionalmente al reves
    std::shared_ptr<AudioData> extract (int64_t startSample, int64_t endSample, bool reversed,
                                        const std::string& newName) const;

private:
    void computePeaks();

    double sampleRate;
    std::vector<float> left, right;
    std::vector<float> peakMin, peakMax;
};

using AudioDataPtr = std::shared_ptr<const AudioData>;

// Remuestreo de un canal (sinc con ventana Blackman)
std::vector<float> resampleChannel (const std::vector<float>& in, double inRate, double outRate);

} // namespace daw
