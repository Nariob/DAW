// =============================================================================
//  NodeGraphTest.cpp  -  Pruebas del sistema de nodos (no necesita JUCE)
//  Compilar:  g++ -std=c++17 -O2 -I../Source/Nodes NodeGraphTest.cpp ../Source/Nodes/*.cpp -o test
// =============================================================================
#include "../Source/Nodes/NodeGraph.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace daw;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf ("  FALLO: %s\n", msg); ++failures; } } while (0)

static float rms (const std::vector<float>& v, size_t from = 0, size_t to = 0)
{
    if (to == 0) to = v.size();
    double s = 0; for (size_t i = from; i < to; ++i) s += (double) v[i] * v[i];
    return (float) std::sqrt (s / (double) (to - from));
}

static bool allFinite (const std::vector<float>& v, float limit)
{
    for (float x : v) if (! std::isfinite (x) || std::abs (x) > limit) return false;
    return true;
}

// Procesa 'seconds' de senal con bloques de tamano variable
static void render (NodeGraph& g, std::vector<float>& L, std::vector<float>& R, double sr)
{
    std::mt19937 rng (1);
    std::uniform_int_distribution<int> blk (1, 700);
    size_t pos = 0;
    while (pos < L.size())
    {
        const int n = (int) std::min<size_t> ((size_t) blk (rng), L.size() - pos);
        ProcessContext ctx; ctx.sampleRate = sr; ctx.timelineSample = (int64_t) pos; ctx.isPlaying = true;
        g.process (ctx, L.data() + pos, R.data() + pos, n);
        pos += (size_t) n;
    }
}

static void makeSignal (std::vector<float>& L, std::vector<float>& R, double sr)
{
    std::mt19937 rng (42);
    std::uniform_real_distribution<float> noise (-0.3f, 0.3f);
    for (size_t i = 0; i < L.size(); ++i)
    {
        const float s = 0.4f * (float) std::sin (2.0 * 3.14159265 * 220.0 * (double) i / sr);
        L[i] = s + noise (rng);
        R[i] = s * 0.8f + noise (rng);
    }
}

int main()
{
    const double sr = 48000.0;
    const size_t len = (size_t) (sr * 3.0);

    // ---------------------------------------------------------------- 1
    std::printf ("[1] Grafo vacio = passthrough\n");
    {
        NodeGraph g; g.prepare (sr, 512);
        std::vector<float> L (len), R (len); makeSignal (L, R, sr);
        auto L0 = L;
        render (g, L, R, sr);
        CHECK (L == L0, "Entrada->Salida debe ser identico");
    }

    // ---------------------------------------------------------------- 2
    std::printf ("[2] Cada tipo de nodo, con parametros aleatorios\n");
    for (auto& info : NodeFactory::get().getTypes())
    {
        if (! info.userCreatable) continue;
        for (int trial = 0; trial < 6; ++trial)
        {
            NodeGraph g; g.prepare (sr, 512);
            const uint32_t id = g.insertNodeBefore (NodeGraph::kOutputNodeId, info.typeId);
            CHECK (id != 0, "no se pudo crear nodo");
            auto node = g.getNode (id);
            std::mt19937 rng ((unsigned) trial * 77u + 3u);
            std::uniform_real_distribution<float> u (0.0f, 1.0f);
            if (trial > 0)
                for (auto& p : node->getParameters()) p->setNormalized (u (rng));

            std::vector<float> L (len), R (len); makeSignal (L, R, sr);
            render (g, L, R, sr);
            char msg[256];
            std::snprintf (msg, sizeof (msg), "%s (prueba %d) produjo NaN/Inf o nivel absurdo", info.typeId.c_str(), trial);
            CHECK (allFinite (L, 200.0f) && allFinite (R, 200.0f), msg);
            if (trial == 0)
                std::printf ("    %-12s ok  (RMS salida %.3f)\n", info.typeId.c_str(), rms (L));
        }
    }

    // ---------------------------------------------------------------- 3
    std::printf ("[3] Ganancia -6 dB\n");
    {
        NodeGraph g; g.prepare (sr, 512);
        auto id = g.insertNodeBefore (NodeGraph::kOutputNodeId, "gain");
        g.getNode (id)->getParameter ("gain")->set (-6.0f);
        std::vector<float> L (len), R (len); makeSignal (L, R, sr);
        auto L0 = L;
        g.getNode (id)->prepare (sr, 512);
        render (g, L, R, sr);
        const float ratio = rms (L, 4800) / rms (L0, 4800);
        std::printf ("    ratio = %.4f (esperado 0.501)\n", ratio);
        CHECK (std::abs (ratio - 0.501f) < 0.01f, "ganancia incorrecta");
    }

    // ---------------------------------------------------------------- 4
    std::printf ("[4] Region: la ganancia solo afecta 1.0s - 2.0s\n");
    {
        NodeGraph g; g.prepare (sr, 512);
        auto id = g.insertNodeBefore (NodeGraph::kOutputNodeId, "gain");
        auto n = g.getNode (id);
        n->getParameter ("gain")->set (-60.0f);
        n->prepare (sr, 512);
        n->regionEnabled = true; n->regionStart = 1.0; n->regionEnd = 2.0; n->regionMode = (int) RegionMode::Blend;
        std::vector<float> L (len), R (len); makeSignal (L, R, sr);
        auto L0 = L;
        render (g, L, R, sr);
        const float before = rms (L, 0, 44000) / rms (L0, 0, 44000);
        const float inside = rms (L, 50000, 94000) / rms (L0, 50000, 94000);
        const float after  = rms (L, 100000, len) / rms (L0, 100000, len);
        std::printf ("    antes %.3f  dentro %.4f  despues %.3f\n", before, inside, after);
        CHECK (std::abs (before - 1.0f) < 0.001f, "antes de la region debe ser igual");
        CHECK (inside < 0.01f, "dentro de la region debe estar atenuado");
        CHECK (std::abs (after - 1.0f) < 0.001f, "despues de la region debe ser igual");
    }

    // ---------------------------------------------------------------- 5
    std::printf ("[5] Region modo Cola: la reverb sigue sonando despues\n");
    {
        NodeGraph g; g.prepare (sr, 512);
        auto id = g.insertNodeBefore (NodeGraph::kOutputNodeId, "reverb");
        auto n = g.getNode (id);
        n->getParameter ("mix")->set (1.0f);
        n->getParameter ("size")->set (0.9f);
        n->regionEnabled = true; n->regionStart = 0.0; n->regionEnd = 1.0; n->regionMode = (int) RegionMode::Tail;
        std::vector<float> L (len, 0.0f), R (len, 0.0f);
        for (size_t i = 0; i < (size_t) sr; ++i) { L[i] = R[i] = 0.3f * (float) std::sin (0.05 * (double) i); }
        render (g, L, R, sr);
        const float tail = rms (L, (size_t) (sr * 1.05), (size_t) (sr * 1.5));
        std::printf ("    RMS de la cola: %.4f\n", tail);
        CHECK (tail > 0.001f, "la cola deberia existir");
    }

    // ---------------------------------------------------------------- 6
    std::printf ("[6] Bypass = passthrough\n");
    {
        NodeGraph g; g.prepare (sr, 512);
        auto id = g.insertNodeBefore (NodeGraph::kOutputNodeId, "distortion");
        g.getNode (id)->bypassed = true;
        g.getNode (id)->prepare (sr, 512);
        std::vector<float> L (len), R (len); makeSignal (L, R, sr);
        auto L0 = L;
        render (g, L, R, sr);
        CHECK (L == L0, "bypass deberia dejar la senal intacta");
    }

    // ---------------------------------------------------------------- 7
    std::printf ("[7] Ciclos rechazados y cadena\n");
    {
        NodeGraph g;
        auto a = g.insertNodeBefore (NodeGraph::kOutputNodeId, "gain");
        auto b = g.insertNodeBefore (NodeGraph::kOutputNodeId, "eq3");
        auto c = g.insertNodeBefore (NodeGraph::kOutputNodeId, "delay");
        auto order = g.getProcessingOrder();
        CHECK (order.size() == 4, "orden deberia tener Entrada + 3 nodos");
        CHECK (order.size() == 4 && order[0] == 1 && order[1] == a && order[2] == b && order[3] == c, "orden incorrecto");
        CHECK (! g.connect (c, a, 0), "c->a formaria un ciclo");
        CHECK (! g.connect (a, a, 0), "auto-conexion");
        CHECK (g.removeNode (b), "borrar b");
        order = g.getProcessingOrder();
        CHECK (order.size() == 3 && order[1] == a && order[2] == c, "al borrar b, a debe quedar conectado a c");
        CHECK (! g.removeNode (NodeGraph::kOutputNodeId), "no se puede borrar la salida");
    }

    // ---------------------------------------------------------------- 8
    std::printf ("[8] Procesamiento paralelo con Mezclador\n");
    {
        NodeGraph g; g.prepare (sr, 512);
        g.disconnect (NodeGraph::kInputNodeId, NodeGraph::kOutputNodeId, 0);
        auto m  = g.addNode ("mixer");
        auto g1 = g.addNode ("gain");
        auto g2 = g.addNode ("gain");
        CHECK (g.connect (1, g1) && g.connect (1, g2), "conectar entrada");
        CHECK (g.connect (g1, m, 0) && g.connect (g2, m, 1), "conectar mezclador");
        CHECK (g.connect (m, 2, 0), "conectar salida");
        std::vector<float> L (len), R (len); makeSignal (L, R, sr);
        auto L0 = L;
        render (g, L, R, sr);
        const float ratio = rms (L) / rms (L0);
        std::printf ("    ratio = %.3f (esperado 2.0)\n", ratio);
        CHECK (std::abs (ratio - 2.0f) < 0.01f, "dos ramas unitarias deberian sumar x2");
    }

    // ---------------------------------------------------------------- 9
    std::printf ("[9] Guardar / cargar\n");
    {
        NodeGraph g;
        auto a = g.insertNodeBefore (NodeGraph::kOutputNodeId, "compressor");
        auto b = g.insertNodeBefore (NodeGraph::kOutputNodeId, "reverb");
        g.getNode (a)->getParameter ("ratio")->set (8.5f);
        g.getNode (a)->name = "Mi compresor con espacios";
        g.getNode (b)->regionEnabled = true;
        g.getNode (b)->regionStart = 1.25;
        const std::string s = g.toString();

        NodeGraph h;
        CHECK (h.fromString (s), "fromString");
        CHECK (h.toString() == s, "ida y vuelta deberia ser identico");
        CHECK (h.getNode (a) && h.getNode (a)->name == "Mi compresor con espacios", "nombre");
        CHECK (h.getNode (b) && std::abs (h.getNode (b)->regionStart.load() - 1.25) < 1e-9, "region");
        auto nb = h.insertNodeBefore (NodeGraph::kOutputNodeId, "gain");
        CHECK (nb > b, "los ids nuevos no deben chocar");
    }

    // ---------------------------------------------------------------- 10
    std::printf ("[10] Limitador respeta el techo\n");
    {
        NodeGraph g; g.prepare (sr, 512);
        auto id = g.insertNodeBefore (NodeGraph::kOutputNodeId, "limiter");
        g.getNode (id)->getParameter ("input")->set (18.0f);
        g.getNode (id)->getParameter ("ceiling")->set (-1.0f);
        std::vector<float> L (len), R (len); makeSignal (L, R, sr);
        render (g, L, R, sr);
        float pk = 0; for (float x : L) pk = std::max (pk, std::abs (x));
        std::printf ("    pico = %.4f (techo 0.891)\n", pk);
        CHECK (pk <= 0.8913f, "el limitador paso el techo");
    }

    std::printf ("\n%s (%d fallos)\n", failures == 0 ? "TODO OK" : "HAY FALLOS", failures);
    return failures == 0 ? 0 : 1;
}
