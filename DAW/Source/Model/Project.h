#pragma once
// =============================================================================
//  Project.h  -  Modelo del proyecto: pistas, clips, master, mezcla, edicion,
//                deshacer/rehacer y guardado en texto.
//
//  Tiempos del timeline en SEGUNDOS (double). Asi el proyecto no depende de la
//  frecuencia de la tarjeta de sonido.
//
//  Hilos:
//   - Todas las funciones de edicion se llaman desde el hilo de UI.
//   - render() se llama desde el hilo de audio.
//   - Un mutex protege la lista de pistas/clips; las secciones criticas son
//     muy cortas (los samples nunca se copian: se comparten con shared_ptr).
//   - Volumen, paneo, mute y solo son atomicos: se cambian sin bloquear.
// =============================================================================
#include "AudioData.h"
#include "ClipLibrary.h"
#include "../Nodes/NodeGraph.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace daw {

// -----------------------------------------------------------------------------
struct Clip
{
    uint32_t id = 0;
    AudioDataPtr source;
    std::string name;

    double start  = 0.0;   // posicion en el timeline (s)
    double offset = 0.0;   // desde donde se lee el audio fuente (s)
    double length = 0.0;   // duracion en el timeline (s)

    float  gainDb  = 0.0f;
    double fadeIn  = 0.0;  // s
    double fadeOut = 0.0;  // s
    bool   muted    = false;
    bool   reversed = false;
    uint32_t colour = 0;   // 0 = usar el color de la pista (formato 0xAARRGGBB)

    double end() const { return start + length; }
    double getSourceLength() const { return source ? source->getLengthSeconds() : 0.0; }
};

// -----------------------------------------------------------------------------
class Track
{
public:
    uint32_t    getId() const { return id; }
    std::string name;
    uint32_t    colour = 0xff4a90e2;
    float       height = 96.0f;   // alto en el timeline (solo UI)

    std::atomic<float> volumeDb { 0.0f };   // -60 .. +12
    std::atomic<float> pan      { 0.0f };   // -1 .. +1
    std::atomic<bool>  muted    { false };
    std::atomic<bool>  solo     { false };

    NodeGraph fx;                          // efectos (nodos) de la pista

    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };   // medidores

private:
    friend class Project;
    void prepare (double sampleRate, int maxBlock);

    uint32_t id = 0;
    std::vector<Clip> clips;

    std::vector<float> bufL, bufR;
    float curGainL = 1.0f, curGainR = 1.0f;
    float accPeakL = 0.0f, accPeakR = 0.0f;
};

// -----------------------------------------------------------------------------
class Project
{
public:
    static constexpr double kMinClipLength = 0.005;   // 5 ms

    explicit Project (ClipLibrary& library);

    ClipLibrary& getLibrary() { return library; }

    // --- Audio ----------------------------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);
    double getSampleRate() const { return sampleRate; }
    void render (const ProcessContext& ctx, float* left, float* right, int numSamples);
    void resetEffects();   // limpia colas de todos los efectos

    // --- Pistas ---------------------------------------------------------------------
    uint32_t addTrack (const std::string& name = {}, int index = -1);
    bool     removeTrack (uint32_t trackId);
    bool     moveTrack (uint32_t trackId, int newIndex);
    uint32_t duplicateTrack (uint32_t trackId);
    bool     renameTrack (uint32_t trackId, const std::string& name);
    bool     setTrackColour (uint32_t trackId, uint32_t colour);

    std::shared_ptr<Track> getTrack (uint32_t trackId) const;
    std::vector<std::shared_ptr<Track>> getTracks() const;
    int    getTrackIndex (uint32_t trackId) const;
    size_t getNumTracks() const;

    // --- Clips ----------------------------------------------------------------------
    uint32_t addClip (uint32_t trackId, AudioDataPtr source, double start);
    bool     removeClip (uint32_t clipId);
    bool     moveClip (uint32_t clipId, uint32_t newTrackId, double newStart);
    uint32_t splitClip (uint32_t clipId, double time);        // devuelve el id de la parte derecha
    bool     trimClipStart (uint32_t clipId, double newStart);
    bool     trimClipEnd (uint32_t clipId, double newEnd);
    uint32_t duplicateClip (uint32_t clipId);                  // la copia queda justo despues
    bool     updateClip (const Clip& clip);                    // reemplaza nombre/ganancia/fades/mute/reverse/color

    std::optional<Clip> getClip (uint32_t clipId) const;
    std::vector<Clip>   getClips (uint32_t trackId) const;
    uint32_t findTrackOfClip (uint32_t clipId) const;

    // --- Operaciones por rango (trackId = 0 -> todas las pistas) -------------------
    int  splitAt (double time, uint32_t trackId = 0);
    bool deleteRange (double t0, double t1, uint32_t trackId = 0, bool ripple = false);
    bool insertSilence (double time, double duration, uint32_t trackId = 0);

    double getLength() const;             // fin del ultimo clip (s)
    bool   isSourceUsed (uint32_t sourceId) const;
    void   replaceSources (const std::vector<std::pair<AudioDataPtr, AudioDataPtr>>& changes);

    // --- Master ----------------------------------------------------------------------
    NodeGraph masterFx;
    std::atomic<float>  masterVolumeDb { 0.0f };
    std::atomic<float>  masterPeakL { 0.0f }, masterPeakR { 0.0f };
    std::atomic<double> bpm { 120.0 };

    // --- Cambios / guardado ---------------------------------------------------------
    uint32_t getVersion() const { return version.load(); }
    std::string toString() const;
    bool fromString (const std::string& text);
    void clear();   // proyecto nuevo (vacio)

    // Copia independiente (pistas, clips y efectos propios) para renderizar
    // offline sin tocar lo que esta sonando. Comparte los samples (no los copia).
    std::unique_ptr<Project> createOfflineCopy (double sampleRate, int maxBlockSize) const;

    // --- Deshacer / rehacer ----------------------------------------------------------
    // La UI llama checkpoint("Mover clip") ANTES de un cambio (o al empezar un arrastre).
    void checkpoint (const std::string& label);
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;
    std::string getUndoLabel() const;
    std::string getRedoLabel() const;
    void clearHistory();

private:
    struct HistoryEntry { std::string state, label; };

    std::shared_ptr<Track> findTrackLocked (uint32_t trackId) const;
    Clip* findClipLocked (uint32_t clipId, Track** owner = nullptr) const;
    std::shared_ptr<Track> createTrackLocked (const std::string& name);
    uint32_t splitClipLocked (uint32_t clipId, double time);
    void rememberSource (const AudioDataPtr& src);
    AudioDataPtr lookupSource (uint32_t sourceId) const;
    std::vector<std::shared_ptr<Track>> tracksForRangeLocked (uint32_t trackId) const;
    void touch() { ++version; }

    ClipLibrary& library;

    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Track>> tracks;
    uint32_t nextTrackId = 1;
    uint32_t nextClipId  = 1;

    std::unordered_map<uint32_t, AudioDataPtr> knownSources;   // para deshacer aunque se borre de la biblioteca

    double sampleRate = 44100.0;
    int    maxBlockSize = 512;
    std::vector<float> mixL, mixR;
    float masterGainCur = 1.0f;

    mutable std::mutex historyMutex;
    std::vector<HistoryEntry> undoStack, redoStack;

    std::atomic<uint32_t> version { 0 };
};

} // namespace daw
