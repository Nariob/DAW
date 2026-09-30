#pragma once
// =============================================================================
//  NodeGraph.h  -  Grafo de nodos de audio
//
//  - Siempre tiene un nodo "Entrada" (id 1) y uno "Salida" (id 2).
//  - Cada nodo puede recibir varias conexiones (se suman) y enviar su salida
//    a varios nodos -> permite cadenas en serie y procesamiento en paralelo.
//  - Se rechazan conexiones que formen ciclos.
//  - Edicion (hilo de UI) y procesamiento (hilo de audio) son seguros:
//    cada cambio compila un "plan de render" nuevo que se publica atomicamente.
//  - Se puede guardar/cargar como texto (para el archivo de proyecto).
// =============================================================================
#include "Node.h"
#include "NodeFactory.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace daw {

struct Connection
{
    uint32_t source = 0;
    uint32_t dest   = 0;
    int destPort    = 0;

    bool operator== (const Connection& o) const
    {
        return source == o.source && dest == o.dest && destPort == o.destPort;
    }
};

struct RenderPlan;

class NodeGraph
{
public:
    static constexpr uint32_t kInputNodeId  = 1;
    static constexpr uint32_t kOutputNodeId = 2;

    NodeGraph();
    ~NodeGraph();

    NodeGraph (const NodeGraph&) = delete;
    NodeGraph& operator= (const NodeGraph&) = delete;

    // --- Audio -----------------------------------------------------------------
    // Llamar con el audio detenido (prepareToPlay).
    void prepare (double sampleRate, int maxBlockSize);
    // Procesa in-place. Seguro de llamar desde el hilo de audio.
    void process (const ProcessContext& ctx, float* left, float* right, int numSamples);
    // Limpia colas de reverb/delay, etc. (p.ej. al saltar en el timeline)
    void reset();

    // --- Edicion (hilo de UI) -------------------------------------------------------
    uint32_t addNode (const std::string& typeId, float x = 0.0f, float y = 0.0f);
    // Inserta un nodo nuevo justo antes de 'destId' (entre su entrada y el).
    // insertBefore(kOutputNodeId, ...) = "agregar al final de la cadena".
    uint32_t insertNodeBefore (uint32_t destId, const std::string& typeId, float x = 0.0f, float y = 0.0f);
    // Inserta un nodo nuevo justo despues de 'sourceId'.
    uint32_t insertNodeAfter (uint32_t sourceId, const std::string& typeId, float x = 0.0f, float y = 0.0f);
    // Borra el nodo y reconecta sus entradas con sus salidas (no rompe la cadena).
    bool removeNode (uint32_t nodeId);
    uint32_t duplicateNode (uint32_t nodeId);

    bool canConnect (uint32_t source, uint32_t dest, int destPort) const;
    bool connect (uint32_t source, uint32_t dest, int destPort = 0);
    bool disconnect (uint32_t source, uint32_t dest, int destPort = 0);
    void disconnectNode (uint32_t nodeId);
    void clear();   // deja solo Entrada -> Salida

    // --- Consultas -----------------------------------------------------------------
    std::shared_ptr<Node> getNode (uint32_t nodeId) const;
    std::vector<std::shared_ptr<Node>> getNodes() const;
    std::vector<Connection> getConnections() const;
    // Nodos en orden de procesamiento (solo los que llegan a la Salida)
    std::vector<uint32_t> getProcessingOrder() const;
    // Aumenta con cada cambio de estructura (la UI lo usa para refrescarse)
    uint32_t getVersion() const { return version.load(); }
    double getSampleRate() const { return sampleRate; }

    // --- Persistencia ---------------------------------------------------------------
    std::string toString() const;
    bool fromString (const std::string& text);
    void copyFrom (const NodeGraph& other) { fromString (other.toString()); }

private:
    std::shared_ptr<Node> findLocked (uint32_t nodeId) const;
    uint32_t addNodeLocked (const std::string& typeId, float x, float y);
    bool canConnectLocked (uint32_t source, uint32_t dest, int destPort) const;
    bool reachesLocked (uint32_t from, uint32_t to) const;
    void createIONodesLocked();
    void rebuildLocked();

    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Node>> nodes;
    std::vector<Connection> connections;
    uint32_t nextId = 3;

    double sampleRate = 44100.0;
    int maxBlockSize = 512;

    std::shared_ptr<RenderPlan> plan;
    std::vector<std::shared_ptr<RenderPlan>> graveyard;   // planes viejos: se liberan en el hilo de UI
    std::atomic<uint32_t> version { 0 };
};

} // namespace daw
