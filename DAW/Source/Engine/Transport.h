#pragma once
// =============================================================================
//  Transport.h  -  Reproduccion: play / pausa / stop, posicion, loop y metronomo.
//  C++ puro. process() se llama desde el hilo de audio; lo demas desde la UI.
//
//  - stop(): detiene y vuelve a donde empezo el play. Un segundo stop() va a 0.
//  - pause(): detiene y se queda donde esta.
// =============================================================================
#include "../Model/Project.h"

#include <atomic>
#include <cstdint>

namespace daw {

class Transport
{
public:
    void prepare (double sampleRate, int maxBlockSize);

    void play();
    void pause();
    void stop();
    void togglePlay() { if (isPlaying()) pause(); else play(); }
    bool isPlaying() const { return playing.load(); }

    void    setPosition (double seconds);
    double  getPosition() const;                 // segundos
    int64_t getPositionSamples() const;
    double  getSampleRate() const { return sampleRate.load(); }

    // Loop (segundos)
    std::atomic<bool>   loopEnabled { false };
    std::atomic<double> loopStart   { 0.0 };
    std::atomic<double> loopEnd     { 4.0 };

    // Metronomo
    std::atomic<bool>  metronomeEnabled  { false };
    std::atomic<float> metronomeVolumeDb { -6.0f };
    std::atomic<int>   beatsPerBar       { 4 };

    // Hilo de audio
    void process (Project& project, float* left, float* right, int numSamples);

private:
    void addMetronome (const Project& project, float* left, float* right, int n, int64_t pos);

    std::atomic<double>  sampleRate { 48000.0 };
    std::atomic<int64_t> position { 0 };
    std::atomic<int64_t> pendingSeek { -1 };
    std::atomic<int64_t> playStart { 0 };
    std::atomic<bool>    playing { false };
    std::atomic<bool>    startRequested { false };

    // Estado del click (solo hilo de audio)
    int    clickRemaining = 0, clickLength = 1;
    double clickPhase = 0.0, clickInc = 0.0;
    float  clickAmp = 0.0f;
};

} // namespace daw
