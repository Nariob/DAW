// =============================================================================
//  EngineTest.cpp  -  Pruebas del transporte (play/stop/loop/metronomo) y del
//  render offline. No necesita JUCE.
//  Compilar (desde DAW/Tests):
//    g++ -std=c++17 -O2 EngineTest.cpp ../Source/Engine/Transport.cpp ../Source/Model/*.cpp ../Source/Nodes/*.cpp -o enginetest
// =============================================================================
#include "../Source/Engine/Transport.h"
#include "../Source/Model/OfflineRenderer.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace daw;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf ("  FALLO: %s\n", msg); ++failures; } } while (0)

static const double SR = 48000.0;

static std::shared_ptr<AudioData> makeRamp (double seconds)
{
    const size_t n = (size_t) (seconds * SR);
    std::vector<float> l (n);
    for (size_t i = 0; i < n; ++i) l[i] = (float) i / (float) n;
    return std::make_shared<AudioData> ("rampa", "", SR, l, l);
}

// Simula el callback de la tarjeta de sonido
static std::vector<float> runTransport (Transport& t, Project& p, double seconds, int block = 480)
{
    const size_t n = (size_t) (seconds * SR);
    std::vector<float> L (n), R (n);
    for (size_t pos = 0; pos < n; pos += (size_t) block)
    {
        const int b = (int) std::min<size_t> ((size_t) block, n - pos);
        t.process (p, L.data() + pos, R.data() + pos, b);
    }
    return L;
}

int main()
{
    ClipLibrary lib;
    Project p (lib);
    p.prepare (SR, 480);
    Transport t;
    t.prepare (SR, 480);

    auto src = lib.add (makeRamp (4.0), SR);
    const uint32_t tr = p.addTrack();
    p.addClip (tr, src, 0.0);

    // ---------------------------------------------------------------- 1
    std::printf ("[1] Play / pausa / stop\n");
    {
        auto L = runTransport (t, p, 0.1);
        CHECK (L[100] == 0.0f, "detenido = silencio");

        t.setPosition (1.0);
        t.play();
        L = runTransport (t, p, 0.5);
        CHECK (std::abs (L[0] - src->getLeft()[48000]) < 1e-6f, "empieza en la posicion indicada");
        CHECK (std::abs (t.getPosition() - 1.5) < 1e-6, "avanza 0.5 s");

        t.pause();
        runTransport (t, p, 0.1);
        CHECK (std::abs (t.getPosition() - 1.5) < 1e-6, "pausa se queda en su lugar");

        t.play();
        runTransport (t, p, 0.2);
        t.stop();
        runTransport (t, p, 0.01);
        CHECK (std::abs (t.getPosition() - 1.5) < 1e-6, "stop vuelve a donde empezo el play");
        t.stop();
        CHECK (t.getPosition() == 0.0, "segundo stop va al inicio");
    }

    // ---------------------------------------------------------------- 2
    std::printf ("[2] Loop 0.5 s - 1.0 s\n");
    {
        t.loopStart = 0.5; t.loopEnd = 1.0; t.loopEnabled = true;
        t.setPosition (0.0);
        t.play();
        auto L = runTransport (t, p, 2.0, 333);   // bloque que no coincide con el loop
        t.pause();
        t.loopEnabled = false;

        float err = 0.0f;
        for (size_t i = 0; i < L.size(); ++i)
        {
            size_t expectedIdx = i;
            if (i >= 48000) expectedIdx = 24000 + (i - 48000) % 24000;
            err = std::max (err, std::abs (L[i] - src->getLeft()[expectedIdx]));
        }
        std::printf ("    error max %.2g\n", err);
        CHECK (err < 1e-6f, "el loop deberia repetir exactamente el tramo");
    }

    // ---------------------------------------------------------------- 3
    std::printf ("[3] Metronomo a 120 BPM\n");
    {
        ClipLibrary lib2;
        Project empty (lib2);
        empty.prepare (SR, 480);
        empty.bpm = 120.0;
        Transport m;
        m.prepare (SR, 480);
        m.metronomeEnabled = true;
        m.play();
        auto L = runTransport (m, empty, 2.0);

        int clicks = 0;
        bool inClick = false;
        int silentRun = 0;
        std::vector<size_t> onsets;
        for (size_t i = 0; i < L.size(); ++i)
        {
            if (std::abs (L[i]) > 1e-4f)
            {
                if (! inClick) { ++clicks; onsets.push_back (i); inClick = true; }
                silentRun = 0;
            }
            else if (++silentRun > 200) inClick = false;
        }
        std::printf ("    clicks: %d en", clicks);
        for (auto o : onsets) std::printf (" %.3fs", (double) o / SR);
        std::printf ("\n");
        CHECK (clicks == 4, "deberian ser 4 clicks en 2 segundos");
    }

    // ---------------------------------------------------------------- 4
    std::printf ("[4] Render offline = reproduccion en tiempo real\n");
    {
        auto trk = p.getTrack (tr);
        trk->fx.insertNodeBefore (NodeGraph::kOutputNodeId, "reverb");
        trk->fx.insertNodeBefore (NodeGraph::kOutputNodeId, "compressor");
        p.masterFx.insertNodeBefore (NodeGraph::kOutputNodeId, "eq3");

        t.setPosition (0.0);
        t.play();
        auto live = runTransport (t, p, 3.0, 480);
        t.pause();

        std::vector<float> off;
        OfflineRenderer::render (p, SR, 0.0, 3.0, [&off] (const float* l, const float*, int n)
        {
            off.insert (off.end(), l, l + n);
            return true;
        });

        float err = 0.0f;
        for (size_t i = 0; i < live.size() && i < off.size(); ++i) err = std::max (err, std::abs (live[i] - off[i]));
        std::printf ("    muestras %zu vs %zu, error max %.2g\n", live.size(), off.size(), err);
        CHECK (off.size() == live.size(), "largo del render offline");
        CHECK (err < 1e-5f, "offline deberia sonar igual");

        const float pk = OfflineRenderer::findPeak (p, SR, 0.0, 3.0);
        float livePk = 0; for (float x : live) livePk = std::max (livePk, std::abs (x));
        CHECK (std::abs (pk - livePk) < 1e-5f, "pico offline");
    }

    // ---------------------------------------------------------------- 5
    std::printf ("[5] Cancelar render offline\n");
    {
        std::atomic<bool> cancel { false };
        int blocks = 0;
        const bool ok = OfflineRenderer::render (p, SR, 0.0, 3.0, [&] (const float*, const float*, int)
        {
            if (++blocks == 10) cancel = true;
            return true;
        }, {}, &cancel);
        CHECK (! ok && blocks == 10, "deberia cancelar");
    }

    // ---------------------------------------------------------------- 6
    std::printf ("[6] Cambio de frecuencia conserva la posicion en segundos\n");
    {
        t.setPosition (2.5);
        runTransport (t, p, 0.01);
        t.prepare (44100.0, 512);
        CHECK (std::abs (t.getPosition() - 2.5) < 1e-4, "posicion tras cambiar a 44.1 kHz");
        t.prepare (SR, 480);
    }

    std::printf ("\n%s (%d fallos)\n", failures == 0 ? "TODO OK" : "HAY FALLOS", failures);
    return failures == 0 ? 0 : 1;
}
