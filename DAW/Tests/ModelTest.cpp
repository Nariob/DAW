// =============================================================================
//  ModelTest.cpp  -  Pruebas del modelo (proyecto, pistas, clips, edicion, undo)
//  Compilar (desde DAW/Tests):
//    g++ -std=c++17 -O2 ModelTest.cpp ../Source/Model/*.cpp ../Source/Nodes/*.cpp -o modeltest
// =============================================================================
#include "../Source/Model/Project.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace daw;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf ("  FALLO: %s\n", msg); ++failures; } } while (0)

static const double SR = 48000.0;

// Audio de prueba: rampa (cada muestra distinta) para detectar cualquier salto
static std::shared_ptr<AudioData> makeRamp (double seconds, const char* name)
{
    const size_t n = (size_t) (seconds * SR);
    std::vector<float> l (n), r (n);
    for (size_t i = 0; i < n; ++i) { l[i] = (float) i / (float) n; r[i] = -l[i]; }
    return std::make_shared<AudioData> (name, "", SR, l, r);
}

static std::vector<float> renderAll (Project& p, double seconds, std::vector<float>* rightOut = nullptr)
{
    const size_t n = (size_t) (seconds * SR);
    std::vector<float> L (n), R (n);
    size_t pos = 0;
    int blk = 1;
    while (pos < n)
    {
        const int b = (int) std::min<size_t> ((size_t) ((blk * 97) % 900 + 1), n - pos);
        ProcessContext ctx; ctx.sampleRate = SR; ctx.timelineSample = (int64_t) pos; ctx.isPlaying = true;
        p.render (ctx, L.data() + pos, R.data() + pos, b);
        pos += (size_t) b; ++blk;
    }
    if (rightOut) *rightOut = R;
    return L;
}

static float maxDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    float m = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max (m, std::abs (a[i] - b[i]));
    return m;
}

int main()
{
    // ---------------------------------------------------------------- 1
    std::printf ("[1] Remuestreo conserva la frecuencia\n");
    {
        const size_t n = 44100;
        std::vector<float> s (n);
        for (size_t i = 0; i < n; ++i) s[i] = (float) std::sin (2 * 3.14159265358979 * 1000.0 * (double) i / 44100.0);
        auto out = resampleChannel (s, 44100.0, 48000.0);
        double err = 0;
        for (size_t i = 2000; i < 46000; ++i)
            err = std::max (err, std::abs (out[i] - std::sin (2 * 3.14159265358979 * 1000.0 * (double) i / 48000.0)));
        std::printf ("    largo %zu (esperado 48000), error max %.5f\n", out.size(), err);
        CHECK (out.size() == 48000, "largo remuestreado");
        CHECK (err < 0.01, "error de remuestreo");
    }

    ClipLibrary lib;
    Project p (lib);
    p.prepare (SR, 256);

    // ---------------------------------------------------------------- 2
    std::printf ("[2] Biblioteca + clip suena identico al archivo\n");
    auto src = lib.add (makeRamp (2.0, "rampa"), SR);
    CHECK (src && src->id == 1, "id de biblioteca");
    const uint32_t t1 = p.addTrack();
    const uint32_t c1 = p.addClip (t1, src, 0.5);
    {
        auto L = renderAll (p, 3.0);
        const size_t o = (size_t) (0.5 * SR);
        float err = 0;
        for (size_t i = 0; i < (size_t) src->getNumSamples(); ++i) err = std::max (err, std::abs (L[o + i] - src->getLeft()[i]));
        CHECK (err < 1e-6f, "el clip deberia sonar igual al audio");
        CHECK (L[o - 1] == 0.0f && L[o + (size_t) src->getNumSamples()] == 0.0f, "silencio fuera del clip");
    }

    // ---------------------------------------------------------------- 3
    std::printf ("[3] Dividir un clip no cambia el sonido (normal y al reves)\n");
    for (int rev = 0; rev < 2; ++rev)
    {
        auto c = *p.getClip (c1);
        c.reversed = rev == 1;
        p.updateClip (c);
        auto before = renderAll (p, 3.0);
        const uint32_t right = p.splitClip (c1, 1.2345);
        CHECK (right != 0, "split");
        auto after = renderAll (p, 3.0);
        char msg[64]; std::snprintf (msg, sizeof msg, "split audible (reverse=%d), diff %.2g", rev, maxDiff (before, after));
        CHECK (maxDiff (before, after) < 1e-6f, msg);
        // volver a un solo clip para la siguiente vuelta
        p.removeClip (right);
        p.trimClipEnd (c1, 10.0);
        auto cc = *p.getClip (c1);
        CHECK (std::abs (cc.length - 2.0) < 1e-9, "trimEnd deberia recuperar el largo total");
    }
    { auto c = *p.getClip (c1); c.reversed = false; p.updateClip (c); }

    // ---------------------------------------------------------------- 4
    std::printf ("[4] Recortar inicio/fin respeta el audio fuente\n");
    {
        p.trimClipStart (c1, 0.9);
        auto c = *p.getClip (c1);
        CHECK (std::abs (c.start - 0.9) < 1e-9 && std::abs (c.offset - 0.4) < 1e-9 && std::abs (c.length - 1.6) < 1e-9, "trimStart");
        p.trimClipStart (c1, 0.0);   // no puede ir mas alla del inicio del audio
        c = *p.getClip (c1);
        CHECK (std::abs (c.start - 0.5) < 1e-9 && std::abs (c.offset) < 1e-9, "trimStart limitado");
        p.trimClipEnd (c1, 1.5);
        c = *p.getClip (c1);
        CHECK (std::abs (c.length - 1.0) < 1e-9, "trimEnd");
        p.trimClipEnd (c1, 99.0);
        c = *p.getClip (c1);
        CHECK (std::abs (c.end() - 2.5) < 1e-9, "trimEnd limitado");
    }

    // ---------------------------------------------------------------- 5
    std::printf ("[5] Borrar rango con ripple\n");
    {
        const uint32_t t2 = p.addTrack ("Otra");
        const uint32_t c2 = p.addClip (t2, src, 3.0);
        p.deleteRange (1.0, 1.5, 0, true);
        auto clips1 = p.getClips (t1);
        CHECK (clips1.size() == 2, "el clip deberia quedar en 2 partes");
        if (clips1.size() == 2)
        {
            CHECK (std::abs (clips1[0].start - 0.5) < 1e-9 && std::abs (clips1[0].end() - 1.0) < 1e-9, "parte izquierda");
            CHECK (std::abs (clips1[1].start - 1.0) < 1e-9 && std::abs (clips1[1].offset - 1.0) < 1e-9, "parte derecha desplazada");
        }
        CHECK (std::abs (p.getClip (c2)->start - 2.5) < 1e-9, "ripple en otra pista");
        p.removeTrack (t2);
    }

    // ---------------------------------------------------------------- 6
    std::printf ("[6] Deshacer / rehacer\n");
    {
        const std::string s0 = p.toString();
        p.checkpoint ("Mover");
        auto id = p.getClips (t1)[0].id;
        p.moveClip (id, t1, 5.0);
        const std::string s1 = p.toString();
        CHECK (s0 != s1, "el estado deberia cambiar");
        CHECK (p.canUndo() && p.getUndoLabel() == "Mover", "etiqueta de deshacer");
        CHECK (p.undo() && p.toString() == s0, "undo");
        CHECK (p.redo() && p.toString() == s1, "redo");
        p.undo();
    }

    // ---------------------------------------------------------------- 7
    std::printf ("[7] Mute, solo, volumen y paneo\n");
    {
        const uint32_t t3 = p.addTrack();
        p.addClip (t3, src, 0.0);
        auto tr1 = p.getTrack (t1);
        auto tr3 = p.getTrack (t3);

        tr1->muted = true;
        auto L = renderAll (p, 0.4);       // t1 empieza en 0.5 -> solo suena t3
        CHECK (std::abs (L[10000] - src->getLeft()[10000]) < 1e-5f, "t3 suena sola");

        tr3->solo = true; tr1->muted = false;
        L = renderAll (p, 3.0);
        CHECK (std::abs (L[(size_t) (1.8 * SR)] - src->getLeft()[(size_t) (1.8 * SR)]) < 1e-5f, "solo en t3 silencia t1");
        tr3->solo = false;

        tr3->volumeDb = -6.0f; tr1->muted = true;
        L = renderAll (p, 0.4);
        CHECK (std::abs (L[15000] / src->getLeft()[15000] - 0.5012f) < 0.001f, "volumen -6 dB");

        tr3->volumeDb = 0.0f; tr3->pan = -1.0f;
        std::vector<float> R;
        L = renderAll (p, 0.4, &R);
        CHECK (std::abs (R[15000]) < 1e-6f && std::abs (L[15000] - src->getLeft()[15000]) < 1e-5f, "paneo total a la izquierda");
        tr3->pan = 0.0f; tr1->muted = false;
        p.removeTrack (t3);
    }

    // ---------------------------------------------------------------- 8
    std::printf ("[8] Fades\n");
    {
        auto c = p.getClips (t1)[0];
        c.fadeIn = 0.1; c.fadeOut = 0.1;
        p.updateClip (c);
        auto L = renderAll (p, 3.0);
        const size_t s = (size_t) std::llround (c.start * SR);
        const size_t e = (size_t) std::llround (c.end() * SR);
        CHECK (std::abs (L[s]) < 1e-6f, "fade in empieza en 0");
        CHECK (std::abs (L[e - 1]) < 1e-3f, "fade out termina en 0");
        c.fadeIn = c.fadeOut = 0.0; p.updateClip (c);
    }

    // ---------------------------------------------------------------- 9
    std::printf ("[9] Nodo con region en una pista via proyecto\n");
    {
        auto tr = p.getTrack (t1);
        auto gid = tr->fx.insertNodeBefore (NodeGraph::kOutputNodeId, "gain");
        auto g = tr->fx.getNode (gid);
        g->getParameter ("gain")->set (-60.0f);
        g->prepare (SR, 256);
        g->regionEnabled = true; g->regionStart = 0.6; g->regionEnd = 0.8; g->regionMode = 1;
        auto L = renderAll (p, 1.0);
        CHECK (std::abs (L[(size_t) (0.55 * SR)]) > 0.001f, "antes de la region suena");
        CHECK (std::abs (L[(size_t) (0.7 * SR)]) < 0.001f, "dentro de la region atenuado");
        CHECK (std::abs (L[(size_t) (0.9 * SR)]) > 0.001f, "despues de la region suena");
    }

    // ---------------------------------------------------------------- 10
    std::printf ("[10] Guardar / cargar proyecto completo\n");
    {
        p.masterFx.insertNodeBefore (NodeGraph::kOutputNodeId, "limiter");
        p.renameTrack (t1, "Voz \"principal\" con espacios");
        p.getTrack (t1)->volumeDb = -3.5f;
        const std::string s = p.toString();

        Project q (lib);
        q.prepare (SR, 256);
        CHECK (q.fromString (s), "fromString");
        CHECK (q.toString() == s, "ida y vuelta identica");
        auto a = renderAll (p, 3.0), b = renderAll (q, 3.0);
        CHECK (maxDiff (a, b) < 1e-6f, "el proyecto cargado suena igual");
        CHECK (q.addTrack() > t1, "ids nuevos no chocan");
    }

    // ---------------------------------------------------------------- 11
    std::printf ("[11] Duplicar pista y clip, insertar silencio\n");
    {
        const uint32_t d = p.duplicateTrack (t1);
        CHECK (d != 0 && p.getClips (d).size() == p.getClips (t1).size(), "duplicar pista");
        CHECK (p.getTrackIndex (d) == p.getTrackIndex (t1) + 1, "la copia queda debajo");
        const uint32_t firstClip = p.getClips (d)[0].id;
        const uint32_t dc = p.duplicateClip (firstClip);
        CHECK (dc != 0 && std::abs (p.getClip (dc)->start - p.getClip (firstClip)->end()) < 1e-9, "duplicar clip");
        const double before = p.getLength();
        p.insertSilence (0.0, 1.0);
        CHECK (std::abs (p.getLength() - (before + 1.0)) < 1e-9, "insertar silencio");
    }

    // ---------------------------------------------------------------- 12
    std::printf ("[12] Deshacer tras borrar el audio de la biblioteca\n");
    {
        p.checkpoint ("Borrar todo");
        p.deleteRange (0.0, 100.0);
        lib.remove (src->id);
        CHECK (p.getLength() == 0.0, "todo borrado");
        p.undo();
        CHECK (p.getLength() > 0.0, "undo recupera los clips aunque el audio ya no este en la biblioteca");
    }

    std::printf ("\n%s (%d fallos)\n", failures == 0 ? "TODO OK" : "HAY FALLOS", failures);
    return failures == 0 ? 0 : 1;
}
