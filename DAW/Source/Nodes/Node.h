#pragma once
// =============================================================================
//  Node.h  -  Clase base de todos los nodos (modificadores de audio)
//
//  - Parametros thread-safe (std::atomic) editables desde la UI mientras suena.
//  - Bypass con fundido (sin clicks).
//  - REGION: cada nodo puede actuar solo sobre un tramo del timeline
//    (inicio/fin en segundos) con fundido en los bordes. Asi se puede
//    "cambiar solo cierta parte del audio".
//      * Modo "Cola"   : solo entra al efecto el audio de la region, pero la
//                        cola (reverb, delay...) sigue sonando despues.
//      * Modo "Mezcla" : fuera de la region se escucha la senal original tal cual.
// =============================================================================
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {

struct ProcessContext
{
    double  sampleRate     = 44100.0;
    int64_t timelineSample = 0;     // posicion en el timeline de la primera muestra del bloque
    bool    isPlaying      = false;
    double  bpm            = 120.0;
};

// Vista de un buffer estereo no intercalado
struct AudioBuf
{
    float* l = nullptr;
    float* r = nullptr;
    int numSamples = 0;
};

// -----------------------------------------------------------------------------
class Parameter
{
public:
    // Parametro continuo
    Parameter (std::string id, std::string name, float minValue, float maxValue, float defaultValue,
               std::string unit = {}, bool logarithmic = false, int decimals = 2);
    // Parametro de opciones (menu)
    Parameter (std::string id, std::string name, std::vector<std::string> choices, int defaultIndex, bool isToggle = false);

    std::string id, name, unit;
    float minValue, maxValue, defaultValue;
    bool logarithmic = false;
    int decimals = 2;
    std::vector<std::string> choices;
    bool toggle = false;

    float get() const           { return value.load (std::memory_order_relaxed); }
    int   getIndex() const      { return (int) std::lround (get()); }
    bool  getBool() const       { return get() >= 0.5f; }
    void  set (float v);
    void  resetToDefault()      { set (defaultValue); }

    bool isChoice() const       { return ! choices.empty(); }
    bool isToggle() const       { return toggle; }

    float toNormalized (float v) const;
    float fromNormalized (float n) const;
    float getNormalized() const { return toNormalized (get()); }
    void  setNormalized (float n) { set (fromNormalized (n)); }

    std::string formatValue (float v) const;
    std::string getValueText() const { return formatValue (get()); }

private:
    std::atomic<float> value { 0.0f };
};

// -----------------------------------------------------------------------------
enum class RegionMode : int { Tail = 0, Blend = 1 };

class Node
{
public:
    Node (std::string typeId, std::string displayName);
    virtual ~Node() = default;

    Node (const Node&) = delete;
    Node& operator= (const Node&) = delete;

    const std::string& getTypeId() const { return typeId; }
    uint32_t getId() const               { return id; }

    std::string name;              // editable por el usuario (solo hilo de UI)
    float posX = 0.0f, posY = 0.0f; // posicion en el editor de nodos (solo UI)

    // --- Topologia -----------------------------------------------------------
    virtual int  getNumInputs() const     { return 1; }
    virtual bool hasOutput() const        { return true; }
    virtual bool isRemovable() const      { return true; }
    virtual bool supportsRegion() const   { return true; }
    virtual std::string getInputName (int index) const;

    // --- Ciclo de vida ------------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);
    void requestReset() { resetPending.store (true); }

    // Llamado por el grafo en el hilo de audio. Aplica bypass y region.
    void processNode (const ProcessContext& ctx, AudioBuf* inputs, int numInputs, AudioBuf& out);

    // --- Parametros ---------------------------------------------------------
    const std::vector<std::unique_ptr<Parameter>>& getParameters() const { return params; }
    Parameter* getParameter (const std::string& paramId) const;

    // --- Medidor opcional (p.ej. reduccion de ganancia del compresor) --------
    virtual bool        hasMeter() const        { return false; }
    virtual std::string getMeterLabel() const   { return {}; }
    virtual float       getMeterValue() const   { return 0.0f; }
    virtual float       getMeterMin() const     { return -24.0f; }
    virtual float       getMeterMax() const     { return 0.0f; }

    // --- Estado compartido con la UI ------------------------------------------
    std::atomic<bool>   bypassed      { false };
    std::atomic<bool>   regionEnabled { false };
    std::atomic<double> regionStart   { 0.0 };   // segundos
    std::atomic<double> regionEnd     { 4.0 };   // segundos
    std::atomic<float>  regionFadeMs  { 10.0f };
    std::atomic<int>    regionMode    { (int) RegionMode::Tail };
    std::atomic<float>  outputPeak    { 0.0f };  // pico del ultimo bloque (para medidores)

protected:
    Parameter* addParameter (const std::string& paramId, const std::string& paramName, float minV, float maxV,
                             float def, const std::string& unit = {}, bool logarithmic = false, int decimals = 2);
    Parameter* addChoice (const std::string& paramId, const std::string& paramName,
                          std::vector<std::string> choices, int defaultIndex);
    Parameter* addToggle (const std::string& paramId, const std::string& paramName, bool def);

    virtual void onPrepare() {}
    virtual void onReset() {}

    // Por defecto: copia la entrada 0 a la salida y llama a process() in-place.
    virtual void processMulti (const ProcessContext& ctx, AudioBuf* inputs, int numInputs, AudioBuf& out);
    virtual void process (const ProcessContext&, AudioBuf&) {}

    double sampleRate = 44100.0;
    int maxBlockSize = 512;

private:
    friend class NodeGraph;

    void computeRegionMask (const ProcessContext& ctx, float* mask, int n) const;

    std::string typeId;
    uint32_t id = 0;
    std::vector<std::unique_ptr<Parameter>> params;

    std::vector<float> dryL, dryR, maskBuf;
    float bypassGain = 1.0f;
    bool  wasFullyBypassed = false;
    std::atomic<bool> resetPending { false };
};

} // namespace daw
