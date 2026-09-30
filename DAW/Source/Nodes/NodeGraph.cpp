#include "NodeGraph.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace daw {

// =============================================================================
//  Plan de render (inmutable en estructura; solo el hilo de audio usa sus buffers)
// =============================================================================
struct StereoBuffer
{
    std::vector<float> l, r;
    void setSize (int n) { l.assign ((size_t) n, 0.0f); r.assign ((size_t) n, 0.0f); }
};

struct RenderStep
{
    std::shared_ptr<Node> node;
    bool isInput = false;
    std::vector<std::vector<int>> portSources;   // indices de pasos que alimentan cada puerto
    std::vector<StereoBuffer> ports;
    std::vector<AudioBuf> portBufs;
    StereoBuffer out;
};

struct RenderPlan
{
    std::vector<RenderStep> steps;
    std::vector<int> outputSources;
    std::shared_ptr<Node> outputNode;
    int maxBlock = 512;
};

// =============================================================================
NodeGraph::NodeGraph()
{
    std::lock_guard<std::mutex> lock (mutex);
    createIONodesLocked();
    rebuildLocked();
}

NodeGraph::~NodeGraph() = default;

void NodeGraph::createIONodesLocked()
{
    nodes.clear();
    connections.clear();

    std::shared_ptr<Node> in (NodeFactory::get().create ("input"));
    in->id = kInputNodeId; in->posX = 30.0f; in->posY = 120.0f;
    std::shared_ptr<Node> out (NodeFactory::get().create ("output"));
    out->id = kOutputNodeId; out->posX = 560.0f; out->posY = 120.0f;

    in->prepare (sampleRate, maxBlockSize);
    out->prepare (sampleRate, maxBlockSize);
    nodes.push_back (in);
    nodes.push_back (out);
    connections.push_back ({ kInputNodeId, kOutputNodeId, 0 });
    nextId = 3;
}

// -----------------------------------------------------------------------------
void NodeGraph::prepare (double sr, int maxBlock)
{
    std::lock_guard<std::mutex> lock (mutex);
    sampleRate = sr > 0.0 ? sr : 44100.0;
    maxBlockSize = std::max (16, maxBlock);
    for (auto& n : nodes)
        n->prepare (sampleRate, maxBlockSize);
    rebuildLocked();
}

void NodeGraph::reset()
{
    std::lock_guard<std::mutex> lock (mutex);
    for (auto& n : nodes)
        n->requestReset();
}

// -----------------------------------------------------------------------------
void NodeGraph::process (const ProcessContext& ctx, float* left, float* right, int numSamples)
{
    auto p = std::atomic_load (&plan);
    if (p == nullptr || numSamples <= 0)
        return;

    int offset = 0;
    while (offset < numSamples)
    {
        const int n = std::min (p->maxBlock, numSamples - offset);
        const size_t bytes = sizeof (float) * (size_t) n;

        ProcessContext sub = ctx;
        sub.timelineSample += offset;

        for (auto& s : p->steps)
        {
            if (s.isInput)
            {
                std::memcpy (s.out.l.data(), left + offset, bytes);
                std::memcpy (s.out.r.data(), right + offset, bytes);
                float pk = 0.0f;
                for (int i = 0; i < n; ++i)
                    pk = std::max (pk, std::max (std::abs (s.out.l[(size_t) i]), std::abs (s.out.r[(size_t) i])));
                s.node->outputPeak.store (pk, std::memory_order_relaxed);
                continue;
            }

            for (size_t k = 0; k < s.portSources.size(); ++k)
            {
                float* pl = s.ports[k].l.data();
                float* pr = s.ports[k].r.data();
                std::memset (pl, 0, bytes);
                std::memset (pr, 0, bytes);
                for (int src : s.portSources[k])
                {
                    const float* sl = p->steps[(size_t) src].out.l.data();
                    const float* sr = p->steps[(size_t) src].out.r.data();
                    for (int i = 0; i < n; ++i) { pl[i] += sl[i]; pr[i] += sr[i]; }
                }
                s.portBufs[k].numSamples = n;
            }

            AudioBuf ob { s.out.l.data(), s.out.r.data(), n };
            s.node->processNode (sub, s.portBufs.data(), (int) s.portBufs.size(), ob);
        }

        // Salida = suma de lo que entra al nodo Salida
        float* L = left + offset;
        float* R = right + offset;
        std::memset (L, 0, bytes);
        std::memset (R, 0, bytes);
        for (int src : p->outputSources)
        {
            const float* sl = p->steps[(size_t) src].out.l.data();
            const float* sr = p->steps[(size_t) src].out.r.data();
            for (int i = 0; i < n; ++i) { L[i] += sl[i]; R[i] += sr[i]; }
        }

        if (p->outputNode != nullptr)
        {
            float pk = 0.0f;
            for (int i = 0; i < n; ++i) pk = std::max (pk, std::max (std::abs (L[i]), std::abs (R[i])));
            p->outputNode->outputPeak.store (pk, std::memory_order_relaxed);
        }

        offset += n;
    }
}

// =============================================================================
//  Edicion
// =============================================================================
std::shared_ptr<Node> NodeGraph::findLocked (uint32_t nodeId) const
{
    for (auto& n : nodes)
        if (n->id == nodeId)
            return n;
    return nullptr;
}

uint32_t NodeGraph::addNodeLocked (const std::string& typeId, float x, float y)
{
    if (typeId == "input" || typeId == "output")
        return 0;

    std::shared_ptr<Node> node (NodeFactory::get().create (typeId));
    if (node == nullptr)
        return 0;

    node->id = nextId++;
    node->posX = x;
    node->posY = y;
    node->prepare (sampleRate, maxBlockSize);
    nodes.push_back (node);
    return node->id;
}

uint32_t NodeGraph::addNode (const std::string& typeId, float x, float y)
{
    std::lock_guard<std::mutex> lock (mutex);
    const uint32_t id = addNodeLocked (typeId, x, y);
    if (id != 0) rebuildLocked();
    return id;
}

uint32_t NodeGraph::insertNodeBefore (uint32_t destId, const std::string& typeId, float x, float y)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto dest = findLocked (destId);
    if (dest == nullptr || dest->getNumInputs() == 0)
        return 0;

    const uint32_t id = addNodeLocked (typeId, x, y);
    if (id == 0) return 0;

    for (auto& c : connections)
        if (c.dest == destId && c.destPort == 0)
        {
            c.dest = id;
            c.destPort = 0;
        }
    connections.push_back ({ id, destId, 0 });

    // quitar duplicados que pudieran quedar
    std::vector<Connection> unique;
    for (auto& c : connections)
        if (std::find (unique.begin(), unique.end(), c) == unique.end())
            unique.push_back (c);
    connections.swap (unique);

    rebuildLocked();
    return id;
}

uint32_t NodeGraph::insertNodeAfter (uint32_t sourceId, const std::string& typeId, float x, float y)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto src = findLocked (sourceId);
    if (src == nullptr || ! src->hasOutput())
        return 0;

    const uint32_t id = addNodeLocked (typeId, x, y);
    if (id == 0) return 0;

    for (auto& c : connections)
        if (c.source == sourceId)
            c.source = id;
    connections.push_back ({ sourceId, id, 0 });

    rebuildLocked();
    return id;
}

bool NodeGraph::removeNode (uint32_t nodeId)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto node = findLocked (nodeId);
    if (node == nullptr || ! node->isRemovable())
        return false;

    // Reconectar: fuentes del puerto 0 -> destinos del nodo borrado
    std::vector<uint32_t> sources;
    std::vector<Connection> outgoing;
    for (auto& c : connections)
    {
        if (c.dest == nodeId && c.destPort == 0) sources.push_back (c.source);
        if (c.source == nodeId)                  outgoing.push_back (c);
    }

    connections.erase (std::remove_if (connections.begin(), connections.end(),
                                       [nodeId] (const Connection& c) { return c.source == nodeId || c.dest == nodeId; }),
                       connections.end());
    nodes.erase (std::remove (nodes.begin(), nodes.end(), node), nodes.end());

    for (auto s : sources)
        for (auto& o : outgoing)
            if (canConnectLocked (s, o.dest, o.destPort))
                connections.push_back ({ s, o.dest, o.destPort });

    rebuildLocked();
    return true;
}

uint32_t NodeGraph::duplicateNode (uint32_t nodeId)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto src = findLocked (nodeId);
    if (src == nullptr || ! src->isRemovable())
        return 0;

    const uint32_t id = addNodeLocked (src->getTypeId(), src->posX + 30.0f, src->posY + 30.0f);
    auto dup = findLocked (id);
    if (dup == nullptr) return 0;

    dup->name = src->name;
    for (auto& p : src->getParameters())
        if (auto* q = dup->getParameter (p->id))
            q->set (p->get());
    dup->bypassed.store (src->bypassed.load());
    dup->regionEnabled.store (src->regionEnabled.load());
    dup->regionStart.store (src->regionStart.load());
    dup->regionEnd.store (src->regionEnd.load());
    dup->regionFadeMs.store (src->regionFadeMs.load());
    dup->regionMode.store (src->regionMode.load());

    rebuildLocked();
    return id;
}

// Existe un camino from -> ... -> to ?
bool NodeGraph::reachesLocked (uint32_t from, uint32_t to) const
{
    std::vector<uint32_t> stack { from };
    std::unordered_set<uint32_t> seen;
    while (! stack.empty())
    {
        const uint32_t cur = stack.back();
        stack.pop_back();
        if (cur == to) return true;
        if (! seen.insert (cur).second) continue;
        for (auto& c : connections)
            if (c.source == cur)
                stack.push_back (c.dest);
    }
    return false;
}

bool NodeGraph::canConnectLocked (uint32_t source, uint32_t dest, int destPort) const
{
    if (source == dest) return false;
    auto s = findLocked (source);
    auto d = findLocked (dest);
    if (s == nullptr || d == nullptr) return false;
    if (! s->hasOutput()) return false;
    if (destPort < 0 || destPort >= d->getNumInputs()) return false;
    if (std::find (connections.begin(), connections.end(), Connection { source, dest, destPort }) != connections.end())
        return false;
    if (reachesLocked (dest, source)) return false;   // formaria un ciclo
    return true;
}

bool NodeGraph::canConnect (uint32_t source, uint32_t dest, int destPort) const
{
    std::lock_guard<std::mutex> lock (mutex);
    return canConnectLocked (source, dest, destPort);
}

bool NodeGraph::connect (uint32_t source, uint32_t dest, int destPort)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (! canConnectLocked (source, dest, destPort))
        return false;
    connections.push_back ({ source, dest, destPort });
    rebuildLocked();
    return true;
}

bool NodeGraph::disconnect (uint32_t source, uint32_t dest, int destPort)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto it = std::find (connections.begin(), connections.end(), Connection { source, dest, destPort });
    if (it == connections.end())
        return false;
    connections.erase (it);
    rebuildLocked();
    return true;
}

void NodeGraph::disconnectNode (uint32_t nodeId)
{
    std::lock_guard<std::mutex> lock (mutex);
    connections.erase (std::remove_if (connections.begin(), connections.end(),
                                       [nodeId] (const Connection& c) { return c.source == nodeId || c.dest == nodeId; }),
                       connections.end());
    rebuildLocked();
}

void NodeGraph::clear()
{
    std::lock_guard<std::mutex> lock (mutex);
    createIONodesLocked();
    rebuildLocked();
}

// =============================================================================
//  Consultas
// =============================================================================
std::shared_ptr<Node> NodeGraph::getNode (uint32_t nodeId) const
{
    std::lock_guard<std::mutex> lock (mutex);
    return findLocked (nodeId);
}

std::vector<std::shared_ptr<Node>> NodeGraph::getNodes() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return nodes;
}

std::vector<Connection> NodeGraph::getConnections() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return connections;
}

std::vector<uint32_t> NodeGraph::getProcessingOrder() const
{
    auto p = std::atomic_load (&plan);
    std::vector<uint32_t> order;
    if (p != nullptr)
        for (auto& s : p->steps)
            order.push_back (s.node->getId());
    return order;
}

// =============================================================================
//  Compilar el plan de render
// =============================================================================
void NodeGraph::rebuildLocked()
{
    auto newPlan = std::make_shared<RenderPlan>();
    newPlan->maxBlock = maxBlockSize;
    newPlan->outputNode = findLocked (kOutputNodeId);

    // Orden topologico: DFS en reversa desde la Salida (solo nodos que llegan a ella)
    std::vector<uint32_t> order;
    std::unordered_set<uint32_t> visited;
    std::function<void (uint32_t)> visit = [&] (uint32_t id)
    {
        if (! visited.insert (id).second) return;
        for (auto& c : connections)
            if (c.dest == id)
                visit (c.source);
        order.push_back (id);
    };
    for (auto& c : connections)
        if (c.dest == kOutputNodeId)
            visit (c.source);

    std::unordered_map<uint32_t, int> stepIndex;
    for (auto id : order)
    {
        auto node = findLocked (id);
        if (node == nullptr) continue;

        RenderStep s;
        s.node = node;
        s.isInput = (id == kInputNodeId);
        const int nIn = s.isInput ? 0 : node->getNumInputs();
        s.portSources.resize ((size_t) nIn);
        s.ports.resize ((size_t) nIn);
        for (int k = 0; k < nIn; ++k)
        {
            s.ports[(size_t) k].setSize (maxBlockSize);
            for (auto& c : connections)
                if (c.dest == id && c.destPort == k)
                {
                    auto it = stepIndex.find (c.source);
                    if (it != stepIndex.end())
                        s.portSources[(size_t) k].push_back (it->second);
                }
        }
        s.out.setSize (maxBlockSize);
        stepIndex[id] = (int) newPlan->steps.size();
        newPlan->steps.push_back (std::move (s));
    }

    // Los punteros de los AudioBuf se fijan despues de mover los pasos
    for (auto& s : newPlan->steps)
    {
        s.portBufs.clear();
        for (auto& b : s.ports)
            s.portBufs.push_back ({ b.l.data(), b.r.data(), maxBlockSize });
    }

    for (auto& c : connections)
        if (c.dest == kOutputNodeId)
        {
            auto it = stepIndex.find (c.source);
            if (it != stepIndex.end())
                newPlan->outputSources.push_back (it->second);
        }

    // Liberar planes viejos que el hilo de audio ya no usa
    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                     [] (const std::shared_ptr<RenderPlan>& g) { return g.use_count() <= 1; }),
                     graveyard.end());

    auto old = std::atomic_exchange (&plan, newPlan);
    if (old != nullptr)
        graveyard.push_back (old);

    ++version;
}

// =============================================================================
//  Persistencia (formato de texto, independiente del idioma del sistema)
// =============================================================================
std::string NodeGraph::toString() const
{
    std::lock_guard<std::mutex> lock (mutex);
    std::ostringstream os;
    os.imbue (std::locale::classic());
    os << std::setprecision (9);
    os << "DAWGRAPH 1\n";
    for (auto& n : nodes)
    {
        os << "NODE " << n->id << ' ' << n->getTypeId() << ' ' << n->posX << ' ' << n->posY << ' '
           << (n->bypassed.load() ? 1 : 0) << ' ' << (n->regionEnabled.load() ? 1 : 0) << ' '
           << n->regionStart.load() << ' ' << n->regionEnd.load() << ' ' << n->regionFadeMs.load() << ' '
           << n->regionMode.load() << ' ' << std::quoted (n->name) << '\n';
        for (auto& p : n->getParameters())
            os << "PARAM " << n->id << ' ' << p->id << ' ' << p->get() << '\n';
    }
    for (auto& c : connections)
        os << "CONN " << c.source << ' ' << c.dest << ' ' << c.destPort << '\n';
    return os.str();
}

bool NodeGraph::fromString (const std::string& text)
{
    std::istringstream is (text);
    is.imbue (std::locale::classic());

    std::string header; int ver = 0;
    is >> header >> ver;
    if (header != "DAWGRAPH")
        return false;

    std::lock_guard<std::mutex> lock (mutex);
    createIONodesLocked();
    connections.clear();

    std::vector<Connection> pending;
    uint32_t maxId = 2;
    std::string line;
    std::getline (is, line);

    while (std::getline (is, line))
    {
        std::istringstream ls (line);
        ls.imbue (std::locale::classic());
        std::string kind;
        ls >> kind;

        if (kind == "NODE")
        {
            uint32_t id; std::string type; float x, y; int byp, reg, mode; double rs, re; float fade; std::string nm;
            if (! (ls >> id >> type >> x >> y >> byp >> reg >> rs >> re >> fade >> mode >> std::quoted (nm)))
                continue;

            std::shared_ptr<Node> node;
            if (type == "input")       node = findLocked (kInputNodeId);
            else if (type == "output") node = findLocked (kOutputNodeId);
            else
            {
                if (id <= 2 || findLocked (id) != nullptr) continue;
                node.reset (NodeFactory::get().create (type).release());
                if (node == nullptr) continue;
                node->id = id;
                node->prepare (sampleRate, maxBlockSize);
                nodes.push_back (node);
            }
            node->posX = x; node->posY = y;
            node->name = nm;
            node->bypassed.store (byp != 0);
            node->regionEnabled.store (reg != 0);
            node->regionStart.store (rs);
            node->regionEnd.store (re);
            node->regionFadeMs.store (fade);
            node->regionMode.store (std::clamp (mode, 0, 1));
            maxId = std::max (maxId, node->id);
        }
        else if (kind == "PARAM")
        {
            uint32_t id; std::string pid; float v;
            if (! (ls >> id >> pid >> v)) continue;
            if (auto node = findLocked (id))
                if (auto* p = node->getParameter (pid))
                    p->set (v);
        }
        else if (kind == "CONN")
        {
            Connection c;
            if (ls >> c.source >> c.dest >> c.destPort)
                pending.push_back (c);
        }
    }

    for (auto& c : pending)
        if (canConnectLocked (c.source, c.dest, c.destPort))
            connections.push_back (c);

    for (auto& n : nodes)
        n->prepare (sampleRate, maxBlockSize);   // estado limpio con los parametros cargados

    nextId = maxId + 1;
    rebuildLocked();
    return true;
}

} // namespace daw
