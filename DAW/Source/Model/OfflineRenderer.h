#pragma once
// =============================================================================
//  OfflineRenderer.h  -  Renderiza un tramo del proyecto mas rapido que tiempo
//  real, sobre una copia independiente (no interfiere con la reproduccion).
//  Lo usan "Exportar WAV" y "Renderizar a clip".
// =============================================================================
#include "Project.h"

#include <atomic>
#include <functional>

namespace daw {

struct OfflineRenderer
{
    // sink recibe bloques (izq, der, n). Si devuelve false, se aborta.
    using Sink = std::function<bool (const float* left, const float* right, int numSamples)>;

    // Devuelve false si se cancelo o si el sink fallo.
    static bool render (const Project& source, double sampleRate, double startSeconds, double endSeconds,
                        const Sink& sink,
                        const std::function<void (float progress)>& progress = {},
                        const std::atomic<bool>* cancel = nullptr);

    // Pico maximo absoluto del tramo (para normalizar)
    static float findPeak (const Project& source, double sampleRate, double startSeconds, double endSeconds,
                           const std::function<void (float)>& progress = {},
                           const std::atomic<bool>* cancel = nullptr);
};

} // namespace daw
