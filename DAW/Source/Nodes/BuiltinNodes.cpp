// =============================================================================
//  BuiltinNodes.cpp  -  Implementacion de todos los nodos de efectos.
//
//  Utilidad : Ganancia, Paneo, Imagen estereo, Mezclador (4 entradas)
//  Filtros  : EQ 3 bandas, Filtro (LP/HP/BP/Notch con LFO = auto-filtro)
//  Dinamica : Compresor, Noise Gate, Limitador (con lookahead)
//  Tiempo   : Delay (ping-pong), Reverb (Freeverb), Chorus, Flanger, Phaser
//  Color    : Distorsion (4 curvas), Bitcrusher, Tremolo / Auto-pan
//  Tono     : Pitch shifter
// =============================================================================
#include "NodeFactory.h"
#include "DSPUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace daw {
namespace {

using namespace dsp;

// =============================================================================
//  Entrada / Salida del grafo (el NodeGraph las maneja directamente)
// =============================================================================
class InputNode : public Node
{
public:
    InputNode() : Node ("input", "Entrada") {}
    int  getNumInputs() const override   { return 0; }
    bool isRemovable() const override    { return false; }
    bool supportsRegion() const override { return false; }
};

class OutputNode : public Node
{
public:
    OutputNode() : Node ("output", "Salida") {}
    bool hasOutput() const override      { return false; }
    bool isRemovable() const override    { return false; }
    bool supportsRegion() const override { return false; }
};

// =============================================================================
//  Ganancia
// =============================================================================
class GainNode : public Node
{
public:
    GainNode() : Node ("gain", "Ganancia")
    {
        gain   = addParameter ("gain", "Ganancia", -60.0f, 24.0f, 0.0f, "dB", false, 1);
        invert = addToggle ("invert", "Invertir fase", false);
    }

protected:
    void onPrepare() override { smooth.reset (sampleRate, 0.02); }
    void onReset() override   { smooth.setCurrentAndTarget (targetGain()); }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        smooth.setTarget (targetGain());
        for (int i = 0; i < io.numSamples; ++i)
        {
            const float g = smooth.next();
            io.l[i] *= g; io.r[i] *= g;
        }
    }

private:
    float targetGain() const { return dbToGain (gain->get()) * (invert->getBool() ? -1.0f : 1.0f); }
    Parameter* gain;
    Parameter* invert;
    SmoothedValue smooth;
};

// =============================================================================
//  Paneo (balance de potencia constante)
// =============================================================================
class PanNode : public Node
{
public:
    PanNode() : Node ("pan", "Paneo")
    {
        pan = addParameter ("pan", "Paneo (I/D)", -1.0f, 1.0f, 0.0f, "", false, 2);
    }

protected:
    void onPrepare() override { sl.reset (sampleRate, 0.02); sr.reset (sampleRate, 0.02); }
    void onReset() override   { float l, r; gains (l, r); sl.setCurrentAndTarget (l); sr.setCurrentAndTarget (r); }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        float l, r; gains (l, r);
        sl.setTarget (l); sr.setTarget (r);
        for (int i = 0; i < io.numSamples; ++i) { io.l[i] *= sl.next(); io.r[i] *= sr.next(); }
    }

private:
    void gains (float& l, float& r) const
    {
        const float angle = (pan->get() + 1.0f) * kPi * 0.25f;
        l = std::min (1.0f, std::sqrt (2.0f) * std::cos (angle));
        r = std::min (1.0f, std::sqrt (2.0f) * std::sin (angle));
    }
    Parameter* pan;
    SmoothedValue sl, sr;
};

// =============================================================================
//  Imagen estereo / utilidad
// =============================================================================
class StereoNode : public Node
{
public:
    StereoNode() : Node ("stereo", "Imagen estereo")
    {
        width   = addParameter ("width", "Ancho", 0.0f, 2.0f, 1.0f, "%", false, 0);
        invL    = addToggle ("invl", "Invertir izq.", false);
        invR    = addToggle ("invr", "Invertir der.", false);
        swapLR  = addToggle ("swap", "Intercambiar I/D", false);
        outGain = addParameter ("out", "Salida", -24.0f, 12.0f, 0.0f, "dB", false, 1);
    }

protected:
    void onPrepare() override { sw.reset (sampleRate, 0.03); sg.reset (sampleRate, 0.03); }
    void onReset() override   { sw.setCurrentAndTarget (width->get()); sg.setCurrentAndTarget (dbToGain (outGain->get())); }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        sw.setTarget (width->get());
        sg.setTarget (dbToGain (outGain->get()));
        const float pl = invL->getBool() ? -1.0f : 1.0f;
        const float pr = invR->getBool() ? -1.0f : 1.0f;
        const bool swap = swapLR->getBool();

        for (int i = 0; i < io.numSamples; ++i)
        {
            float l = io.l[i] * pl, r = io.r[i] * pr;
            if (swap) std::swap (l, r);
            const float m = 0.5f * (l + r);
            const float s = 0.5f * (l - r) * sw.next();
            const float g = sg.next();
            io.l[i] = (m + s) * g;
            io.r[i] = (m - s) * g;
        }
    }

private:
    Parameter *width, *invL, *invR, *swapLR, *outGain;
    SmoothedValue sw, sg;
};

// =============================================================================
//  Mezclador de 4 entradas (para procesamiento en paralelo en el grafo)
// =============================================================================
class MixerNode : public Node
{
public:
    MixerNode() : Node ("mixer", "Mezclador")
    {
        for (int k = 0; k < 4; ++k)
            g[k] = addParameter ("in" + std::to_string (k + 1), "Entrada " + std::to_string (k + 1),
                                 -60.0f, 12.0f, 0.0f, "dB", false, 1);
    }
    int getNumInputs() const override { return 4; }

protected:
    void onPrepare() override { for (auto& s : sm) s.reset (sampleRate, 0.02); }
    void onReset() override   { for (int k = 0; k < 4; ++k) sm[k].setCurrentAndTarget (dbToGain (g[k]->get())); }

    void processMulti (const ProcessContext&, AudioBuf* inputs, int numInputs, AudioBuf& out) override
    {
        const int n = out.numSamples;
        std::fill (out.l, out.l + n, 0.0f);
        std::fill (out.r, out.r + n, 0.0f);
        for (int k = 0; k < 4; ++k)
        {
            sm[k].setTarget (dbToGain (g[k]->get()));
            if (k >= numInputs) { for (int i = 0; i < n; ++i) sm[k].next(); continue; }
            for (int i = 0; i < n; ++i)
            {
                const float gg = sm[k].next();
                out.l[i] += inputs[k].l[i] * gg;
                out.r[i] += inputs[k].r[i] * gg;
            }
        }
    }

private:
    Parameter* g[4];
    SmoothedValue sm[4];
};

// =============================================================================
//  EQ de 3 bandas (shelf grave, campana media, shelf agudo)
// =============================================================================
class EQ3Node : public Node
{
public:
    EQ3Node() : Node ("eq3", "EQ 3 bandas")
    {
        lowF  = addParameter ("lowf",  "Graves frec.",  20.0f, 1000.0f, 100.0f, "Hz", true, 0);
        lowG  = addParameter ("lowg",  "Graves",       -18.0f, 18.0f,   0.0f,  "dB", false, 1);
        midF  = addParameter ("midf",  "Medios frec.", 150.0f, 8000.0f, 1000.0f, "Hz", true, 0);
        midG  = addParameter ("midg",  "Medios",       -18.0f, 18.0f,   0.0f,  "dB", false, 1);
        midQ  = addParameter ("midq",  "Medios Q",      0.1f,  10.0f,   1.0f,  "",   true, 2);
        highF = addParameter ("highf", "Agudos frec.", 1000.0f, 20000.0f, 8000.0f, "Hz", true, 0);
        highG = addParameter ("highg", "Agudos",       -18.0f, 18.0f,   0.0f,  "dB", false, 1);
        outG  = addParameter ("out",   "Salida",       -24.0f, 12.0f,   0.0f,  "dB", false, 1);
    }

protected:
    void onPrepare() override { so.reset (sampleRate, 0.02); cache.fill (-1.0e9f); }
    void onReset() override
    {
        for (auto& ch : f) for (auto& b : ch) b.reset();
        so.setCurrentAndTarget (dbToGain (outG->get()));
        cache.fill (-1.0e9f);
        update();
    }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        update();
        so.setTarget (dbToGain (outG->get()));
        for (int i = 0; i < io.numSamples; ++i)
        {
            float l = io.l[i], r = io.r[i];
            for (int b = 0; b < 3; ++b) { l = f[0][b].process (l); r = f[1][b].process (r); }
            const float g = so.next();
            io.l[i] = l * g; io.r[i] = r * g;
        }
    }

private:
    void update()
    {
        const std::array<float, 7> v { lowF->get(), lowG->get(), midF->get(), midG->get(),
                                       midQ->get(), highF->get(), highG->get() };
        if (v == cache) return;
        cache = v;
        const auto c0 = biquad::lowShelf  (sampleRate, v[0], 0.707, v[1]);
        const auto c1 = biquad::peak      (sampleRate, v[2], v[4], v[3]);
        const auto c2 = biquad::highShelf (sampleRate, v[5], 0.707, v[6]);
        for (auto& ch : f) { ch[0].setCoeffs (c0); ch[1].setCoeffs (c1); ch[2].setCoeffs (c2); }
    }

    Parameter *lowF, *lowG, *midF, *midG, *midQ, *highF, *highG, *outG;
    Biquad f[2][3];
    std::array<float, 7> cache {};
    SmoothedValue so;
};

// =============================================================================
//  Filtro multimodo con LFO (auto-filtro / wah)
// =============================================================================
class FilterNode : public Node
{
public:
    FilterNode() : Node ("filter", "Filtro")
    {
        type     = addChoice ("type", "Tipo", { "Pasa bajos", "Pasa altos", "Pasa banda", "Notch" }, 0);
        cutoff   = addParameter ("cutoff", "Corte", 20.0f, 20000.0f, 1000.0f, "Hz", true, 0);
        q        = addParameter ("q", "Resonancia (Q)", 0.1f, 18.0f, 0.707f, "", true, 2);
        slope    = addChoice ("slope", "Pendiente", { "12 dB/oct", "24 dB/oct" }, 0);
        lfoRate  = addParameter ("lforate", "LFO velocidad", 0.01f, 20.0f, 1.0f, "Hz", true, 2);
        lfoDepth = addParameter ("lfodepth", "LFO profundidad", 0.0f, 4.0f, 0.0f, "oct", false, 2);
    }

protected:
    void onReset() override
    {
        for (auto& ch : f) for (auto& b : ch) b.reset();
        lfo.reset();
        current = cutoff->get();
    }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        constexpr int chunk = 16;
        const int t = type->getIndex();
        const bool steep = slope->getIndex() == 1;
        lfo.setRate (lfoRate->get(), sampleRate);
        const float depth = lfoDepth->get();

        for (int start = 0; start < io.numSamples; start += chunk)
        {
            const int len = std::min (chunk, io.numSamples - start);

            float target = cutoff->get();
            if (depth > 0.0f) target *= std::pow (2.0f, depth * lfo.valueAt (0.0, 0));
            target = std::clamp (target, 20.0f, (float) (sampleRate * 0.45));
            // suavizado en dominio logaritmico
            current = current * std::pow (target / current, 0.35f);

            const auto c1 = design (t, current, q->get());
            const auto c2 = design (t, current, 0.707f);
            for (int ch = 0; ch < 2; ++ch) { f[ch][0].setCoeffs (c1); f[ch][1].setCoeffs (c2); }

            for (int i = start; i < start + len; ++i)
            {
                float l = f[0][0].process (io.l[i]);
                float r = f[1][0].process (io.r[i]);
                if (steep) { l = f[0][1].process (l); r = f[1][1].process (r); }
                io.l[i] = l; io.r[i] = r;
            }
            lfo.advance (len);
        }
    }

private:
    BiquadCoeffs design (int t, float fc, float qq) const
    {
        switch (t)
        {
            case 1:  return biquad::highPass (sampleRate, fc, qq);
            case 2:  return biquad::bandPass (sampleRate, fc, qq);
            case 3:  return biquad::notch    (sampleRate, fc, qq);
            default: return biquad::lowPass  (sampleRate, fc, qq);
        }
    }

    Parameter *type, *cutoff, *q, *slope, *lfoRate, *lfoDepth;
    Biquad f[2][2];
    LFO lfo;
    float current = 1000.0f;
};

// =============================================================================
//  Compresor (estereo enlazado, rodilla suave, mezcla paralela)
// =============================================================================
class CompressorNode : public Node
{
public:
    CompressorNode() : Node ("compressor", "Compresor")
    {
        threshold = addParameter ("threshold", "Umbral", -60.0f, 0.0f, -18.0f, "dB", false, 1);
        ratio     = addParameter ("ratio", "Ratio", 1.0f, 20.0f, 4.0f, ":1", true, 1);
        attack    = addParameter ("attack", "Ataque", 0.1f, 200.0f, 10.0f, "ms", true, 1);
        release   = addParameter ("release", "Relajacion", 5.0f, 2000.0f, 120.0f, "ms", true, 0);
        knee      = addParameter ("knee", "Rodilla", 0.0f, 24.0f, 6.0f, "dB", false, 1);
        makeup    = addParameter ("makeup", "Ganancia", 0.0f, 24.0f, 0.0f, "dB", false, 1);
        mix       = addParameter ("mix", "Mezcla", 0.0f, 1.0f, 1.0f, "%", false, 0);
    }

    bool hasMeter() const override             { return true; }
    std::string getMeterLabel() const override { return "Reduccion"; }
    float getMeterValue() const override       { return grMeter.load(); }

protected:
    void onReset() override { env = 0.0f; grMeter.store (0.0f); }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        const float T = threshold->get(), R = ratio->get(), W = knee->get();
        const float aC = msToCoef (attack->get(), sampleRate);
        const float rC = msToCoef (release->get(), sampleRate);
        const float mk = dbToGain (makeup->get());
        const float wet = mix->get();
        float minGr = 0.0f;

        for (int i = 0; i < io.numSamples; ++i)
        {
            const float x  = gainToDb (std::max (std::abs (io.l[i]), std::abs (io.r[i])));
            const float ov = x - T;
            float y;
            if (2.0f * ov < -W)                 y = x;
            else if (W > 0.0f && 2.0f * std::abs (ov) <= W)
                y = x + (1.0f / R - 1.0f) * (ov + W * 0.5f) * (ov + W * 0.5f) / (2.0f * W);
            else                                 y = T + ov / R;

            const float gr = y - x;   // <= 0
            env = gr < env ? aC * env + (1.0f - aC) * gr : rC * env + (1.0f - rC) * gr;
            minGr = std::min (minGr, env);

            const float g = dbToGain (env) * mk;
            io.l[i] = io.l[i] * (1.0f - wet) + io.l[i] * g * wet;
            io.r[i] = io.r[i] * (1.0f - wet) + io.r[i] * g * wet;
        }
        grMeter.store (minGr);
    }

private:
    Parameter *threshold, *ratio, *attack, *release, *knee, *makeup, *mix;
    float env = 0.0f;
    std::atomic<float> grMeter { 0.0f };
};

// =============================================================================
//  Noise gate
// =============================================================================
class GateNode : public Node
{
public:
    GateNode() : Node ("gate", "Noise Gate")
    {
        threshold = addParameter ("threshold", "Umbral", -80.0f, 0.0f, -40.0f, "dB", false, 1);
        range     = addParameter ("range", "Atenuacion", -80.0f, 0.0f, -80.0f, "dB", false, 1);
        attack    = addParameter ("attack", "Ataque", 0.1f, 50.0f, 1.0f, "ms", true, 1);
        hold      = addParameter ("hold", "Mantener", 0.0f, 500.0f, 50.0f, "ms", false, 0);
        release   = addParameter ("release", "Relajacion", 5.0f, 2000.0f, 150.0f, "ms", true, 0);
    }

    bool hasMeter() const override             { return true; }
    std::string getMeterLabel() const override { return "Atenuacion"; }
    float getMeterValue() const override       { return meter.load(); }
    float getMeterMin() const override         { return -80.0f; }

protected:
    void onReset() override { gain = 0.0f; level = 0.0f; holdCount = 0; }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        const float thr    = dbToGain (threshold->get());
        const float closed = dbToGain (range->get());
        const float aC = msToCoef (attack->get(), sampleRate);
        const float rC = msToCoef (release->get(), sampleRate);
        const float detC = msToCoef (5.0f, sampleRate);
        const int holdSamples = (int) (hold->get() * 0.001 * sampleRate);
        float minG = 1.0f;

        for (int i = 0; i < io.numSamples; ++i)
        {
            const float pk = std::max (std::abs (io.l[i]), std::abs (io.r[i]));
            level = pk > level ? pk : detC * level + (1.0f - detC) * pk;

            float target;
            if (level >= thr)          { target = 1.0f; holdCount = holdSamples; }
            else if (holdCount > 0)    { target = 1.0f; --holdCount; }
            else                       target = closed;

            gain = target > gain ? aC * gain + (1.0f - aC) * target : rC * gain + (1.0f - rC) * target;
            minG = std::min (minG, gain);
            io.l[i] *= gain; io.r[i] *= gain;
        }
        meter.store (gainToDb (minG));
    }

private:
    Parameter *threshold, *range, *attack, *hold, *release;
    float gain = 0.0f, level = 0.0f;
    int holdCount = 0;
    std::atomic<float> meter { 0.0f };
};

// =============================================================================
//  Limitador con lookahead
// =============================================================================
class LimiterNode : public Node
{
public:
    LimiterNode() : Node ("limiter", "Limitador")
    {
        inGain    = addParameter ("input", "Entrada", 0.0f, 24.0f, 0.0f, "dB", false, 1);
        ceiling   = addParameter ("ceiling", "Techo", -24.0f, 0.0f, -0.3f, "dB", false, 1);
        release   = addParameter ("release", "Relajacion", 1.0f, 1000.0f, 80.0f, "ms", true, 0);
        lookahead = addParameter ("lookahead", "Anticipacion", 0.0f, 10.0f, 3.0f, "ms", false, 1);
    }

    bool hasMeter() const override             { return true; }
    std::string getMeterLabel() const override { return "Reduccion"; }
    float getMeterValue() const override       { return meter.load(); }

protected:
    void onPrepare() override
    {
        const int maxLa = (int) (0.011 * sampleRate) + 2;
        dl.setMaxDelaySamples (maxLa);
        dr.setMaxDelaySamples (maxLa);
    }
    void onReset() override { dl.clear(); dr.clear(); g = 1.0f; held = 1.0f; holdCount = 0; }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        const float ig  = dbToGain (inGain->get());
        const float ceil = dbToGain (ceiling->get());
        const int la = std::max (1, (int) (lookahead->get() * 0.001 * sampleRate));
        const float aC = msToCoef (std::max (0.05f, lookahead->get() / 3.0f), sampleRate);
        const float rC = msToCoef (release->get(), sampleRate);
        float minG = 1.0f;

        for (int i = 0; i < io.numSamples; ++i)
        {
            const float l = io.l[i] * ig, r = io.r[i] * ig;
            const float pk = std::max (std::abs (l), std::abs (r));
            const float t  = pk > ceil ? ceil / pk : 1.0f;

            if (t <= held)           { held = t; holdCount = la; }
            else if (holdCount > 0)  --holdCount;
            else                     held = t;

            g = held < g ? aC * g + (1.0f - aC) * held : rC * g + (1.0f - rC) * held;
            minG = std::min (minG, g);

            const float ol = dl.read ((float) la);
            const float orr = dr.read ((float) la);
            dl.push (l); dr.push (r);

            io.l[i] = std::clamp (ol * g, -ceil, ceil);
            io.r[i] = std::clamp (orr * g, -ceil, ceil);
        }
        meter.store (gainToDb (minG));
    }

private:
    Parameter *inGain, *ceiling, *release, *lookahead;
    DelayLine dl, dr;
    float g = 1.0f, held = 1.0f;
    int holdCount = 0;
    std::atomic<float> meter { 0.0f };
};

// =============================================================================
//  Delay estereo / ping-pong con filtro en la realimentacion
// =============================================================================
class DelayNode : public Node
{
public:
    DelayNode() : Node ("delay", "Delay")
    {
        time     = addParameter ("time", "Tiempo", 1.0f, 2000.0f, 350.0f, "ms", true, 0);
        feedback = addParameter ("feedback", "Realimentacion", 0.0f, 0.95f, 0.4f, "%", false, 0);
        tone     = addParameter ("tone", "Tono", 500.0f, 20000.0f, 8000.0f, "Hz", true, 0);
        pingpong = addToggle ("pingpong", "Ping-pong", false);
        mix      = addParameter ("mix", "Mezcla", 0.0f, 1.0f, 0.3f, "%", false, 0);
    }

protected:
    void onPrepare() override
    {
        const int maxD = (int) (2.1 * sampleRate);
        dl.setMaxDelaySamples (maxD); dr.setMaxDelaySamples (maxD);
        st.reset (sampleRate, 0.08);
    }
    void onReset() override
    {
        dl.clear(); dr.clear(); lpL.reset(); lpR.reset();
        st.setCurrentAndTarget ((float) (time->get() * 0.001 * sampleRate));
    }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        st.setTarget ((float) (time->get() * 0.001 * sampleRate));
        lpL.setCutoff (tone->get(), sampleRate);
        lpR.setCutoff (tone->get(), sampleRate);
        const float fb = feedback->get(), wet = mix->get();
        const bool pp = pingpong->getBool();

        for (int i = 0; i < io.numSamples; ++i)
        {
            const float d = st.next();
            const float yl = lpL.process (dl.read (d));
            const float yr = lpR.process (dr.read (d));
            const float xl = io.l[i], xr = io.r[i];

            if (pp) { dl.push ((xl + xr) * 0.5f + fb * yr); dr.push (fb * yl); }
            else    { dl.push (xl + fb * yl);               dr.push (xr + fb * yr); }

            io.l[i] = xl * (1.0f - wet) + yl * wet;
            io.r[i] = xr * (1.0f - wet) + yr * wet;
        }
    }

private:
    Parameter *time, *feedback, *tone, *pingpong, *mix;
    DelayLine dl, dr;
    OnePoleLP lpL, lpR;
    SmoothedValue st;
};

// =============================================================================
//  Reverb (algoritmo Freeverb: 8 combs + 4 allpass por canal) con pre-delay
// =============================================================================
class ReverbNode : public Node
{
public:
    ReverbNode() : Node ("reverb", "Reverb")
    {
        size     = addParameter ("size", "Tamano", 0.0f, 1.0f, 0.6f, "%", false, 0);
        damping  = addParameter ("damping", "Amortiguacion", 0.0f, 1.0f, 0.5f, "%", false, 0);
        width    = addParameter ("width", "Ancho", 0.0f, 1.0f, 1.0f, "%", false, 0);
        predelay = addParameter ("predelay", "Pre-delay", 0.0f, 200.0f, 10.0f, "ms", false, 0);
        freeze   = addToggle ("freeze", "Congelar", false);
        mix      = addParameter ("mix", "Mezcla", 0.0f, 1.0f, 0.3f, "%", false, 0);
    }

protected:
    struct Comb
    {
        std::vector<float> buf; size_t idx = 0; float store = 0.0f;
        void setSize (int n) { buf.assign ((size_t) std::max (1, n), 0.0f); idx = 0; store = 0.0f; }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); store = 0.0f; }
        inline float process (float in, float fb, float d1, float d2)
        {
            const float out = buf[idx];
            store = flushDenormal (out * d2 + store * d1);
            buf[idx] = flushDenormal (in + store * fb);
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };
    struct Allpass
    {
        std::vector<float> buf; size_t idx = 0;
        void setSize (int n) { buf.assign ((size_t) std::max (1, n), 0.0f); idx = 0; }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
        inline float process (float in)
        {
            const float b = buf[idx];
            buf[idx] = flushDenormal (in + b * 0.5f);
            if (++idx >= buf.size()) idx = 0;
            return b - in;
        }
    };

    void onPrepare() override
    {
        static const int combT[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int apT[4]   = { 556, 441, 341, 225 };
        const double sc = sampleRate / 44100.0;
        for (int c = 0; c < 8; ++c)
        {
            combL[c].setSize ((int) (combT[c] * sc));
            combR[c].setSize ((int) ((combT[c] + 23) * sc));
        }
        for (int a = 0; a < 4; ++a)
        {
            apL[a].setSize ((int) (apT[a] * sc));
            apR[a].setSize ((int) ((apT[a] + 23) * sc));
        }
        pre.setMaxDelaySamples ((int) (0.21 * sampleRate));
        preR.setMaxDelaySamples ((int) (0.21 * sampleRate));
    }
    void onReset() override
    {
        for (auto& c : combL) c.clear();
        for (auto& c : combR) c.clear();
        for (auto& a : apL) a.clear();
        for (auto& a : apR) a.clear();
        pre.clear(); preR.clear();
    }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        const bool frz = freeze->getBool();
        const float fb = frz ? 1.0f : size->get() * 0.28f + 0.7f;
        const float d  = frz ? 0.0f : damping->get() * 0.4f;
        const float d1 = d, d2 = 1.0f - d;
        const float inGain = frz ? 0.0f : 0.015f;
        const float w = width->get();
        const float wet = mix->get();
        const float wet1 = 3.0f * (w * 0.5f + 0.5f), wet2 = 3.0f * ((1.0f - w) * 0.5f);
        const float pd = (float) (predelay->get() * 0.001 * sampleRate);

        for (int i = 0; i < io.numSamples; ++i)
        {
            const float xl = io.l[i], xr = io.r[i];
            float inL = xl, inR = xr;
            if (pd >= 1.0f) { inL = pre.read (pd); inR = preR.read (pd); }
            pre.push (xl); preR.push (xr);

            const float input = (inL + inR) * inGain;
            float accL = 0.0f, accR = 0.0f;
            for (int c = 0; c < 8; ++c)
            {
                accL += combL[c].process (input, fb, d1, d2);
                accR += combR[c].process (input, fb, d1, d2);
            }
            for (int a = 0; a < 4; ++a) { accL = apL[a].process (accL); accR = apR[a].process (accR); }

            const float outL = accL * wet1 + accR * wet2;
            const float outR = accR * wet1 + accL * wet2;
            io.l[i] = xl * (1.0f - wet) + outL * wet;
            io.r[i] = xr * (1.0f - wet) + outR * wet;
        }
    }

private:
    Parameter *size, *damping, *width, *predelay, *freeze, *mix;
    Comb combL[8], combR[8];
    Allpass apL[4], apR[4];
    DelayLine pre, preR;
};

// =============================================================================
//  Delay modulado: sirve para Chorus y Flanger
// =============================================================================
class ModDelayNode : public Node
{
public:
    ModDelayNode (const char* typeId, const char* display, float defRate, float defDepth, float defDelay,
                  float defFb, float minFb, float maxDelay)
        : Node (typeId, display)
    {
        rate     = addParameter ("rate", "Velocidad", 0.01f, 10.0f, defRate, "Hz", true, 2);
        depth    = addParameter ("depth", "Profundidad", 0.0f, 10.0f, defDepth, "ms", false, 2);
        delay    = addParameter ("delay", "Retardo", 0.1f, maxDelay, defDelay, "ms", false, 1);
        feedback = addParameter ("feedback", "Realimentacion", minFb, 0.95f, defFb, "", false, 2);
        stereo   = addParameter ("stereo", "Fase estereo", 0.0f, 180.0f, 90.0f, "deg", false, 0);
        mix      = addParameter ("mix", "Mezcla", 0.0f, 1.0f, 0.5f, "%", false, 0);
    }

protected:
    void onPrepare() override
    {
        const int maxD = (int) (0.06 * sampleRate);
        dl.setMaxDelaySamples (maxD); dr.setMaxDelaySamples (maxD);
    }
    void onReset() override { dl.clear(); dr.clear(); lfo.reset(); }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        lfo.setRate (rate->get(), sampleRate);
        const float msToS = (float) (sampleRate * 0.001);
        const float base = delay->get(), dep = depth->get(), fb = feedback->get(), wet = mix->get();
        const double off = stereo->get() / 360.0;

        for (int i = 0; i < io.numSamples; ++i)
        {
            const float ml = 0.5f + 0.5f * lfo.valueAt (0.0, 0);
            const float mr = 0.5f + 0.5f * lfo.valueAt (off, 0);
            lfo.advance();

            const float yl = dl.read ((base + dep * ml) * msToS);
            const float yr = dr.read ((base + dep * mr) * msToS);
            const float xl = io.l[i], xr = io.r[i];
            dl.push (xl + fb * yl);
            dr.push (xr + fb * yr);

            io.l[i] = xl * (1.0f - wet) + yl * wet;
            io.r[i] = xr * (1.0f - wet) + yr * wet;
        }
    }

private:
    Parameter *rate, *depth, *delay, *feedback, *stereo, *mix;
    DelayLine dl, dr;
    LFO lfo;
};

// =============================================================================
//  Phaser (cadena de allpass de primer orden barrida por LFO)
// =============================================================================
class PhaserNode : public Node
{
public:
    PhaserNode() : Node ("phaser", "Phaser")
    {
        stages   = addChoice ("stages", "Etapas", { "4", "6", "8", "12" }, 1);
        rate     = addParameter ("rate", "Velocidad", 0.01f, 10.0f, 0.5f, "Hz", true, 2);
        depth    = addParameter ("depth", "Profundidad", 0.0f, 1.0f, 0.7f, "%", false, 0);
        center   = addParameter ("center", "Frecuencia", 100.0f, 4000.0f, 800.0f, "Hz", true, 0);
        feedback = addParameter ("feedback", "Realimentacion", -0.95f, 0.95f, 0.5f, "", false, 2);
        mix      = addParameter ("mix", "Mezcla", 0.0f, 1.0f, 0.5f, "%", false, 0);
    }

protected:
    void onReset() override
    {
        for (auto& ch : z) ch.fill (0.0f);
        last[0] = last[1] = 0.0f;
        lfo.reset();
    }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        static const int stageCounts[4] = { 4, 6, 8, 12 };
        const int ns = stageCounts[std::clamp (stages->getIndex(), 0, 3)];
        lfo.setRate (rate->get(), sampleRate);
        const float fc = center->get(), dep = depth->get(), fb = feedback->get(), wet = mix->get();
        float a[2] = { 0.0f, 0.0f };

        for (int i = 0; i < io.numSamples; ++i)
        {
            if ((i & 7) == 0)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float m = lfo.valueAt (ch == 0 ? 0.0 : 0.25, 0);
                    const double f = std::clamp ((double) fc * std::pow (2.0, dep * 3.0 * m), 20.0, sampleRate * 0.45);
                    const double t = std::tan (3.14159265358979 * f / sampleRate);
                    a[ch] = (float) ((t - 1.0) / (t + 1.0));
                }
            }
            lfo.advance();

            float* io_[2] = { io.l, io.r };
            for (int ch = 0; ch < 2; ++ch)
            {
                const float x = io_[ch][i];
                float s = x + fb * last[ch];
                for (int k = 0; k < ns; ++k)
                {
                    const float y = a[ch] * s + z[ch][(size_t) k];
                    z[ch][(size_t) k] = flushDenormal (s - a[ch] * y);
                    s = y;
                }
                last[ch] = std::clamp (s, -4.0f, 4.0f);
                io_[ch][i] = x * (1.0f - wet) + s * wet;
            }
        }
    }

private:
    Parameter *stages, *rate, *depth, *center, *feedback, *mix;
    std::array<float, 12> z[2] {};
    float last[2] { 0.0f, 0.0f };
    LFO lfo;
};

// =============================================================================
//  Distorsion
// =============================================================================
class DistortionNode : public Node
{
public:
    DistortionNode() : Node ("distortion", "Distorsion")
    {
        type  = addChoice ("type", "Tipo", { "Suave (tanh)", "Dura (clip)", "Plegado (fold)", "Asimetrica" }, 0);
        drive = addParameter ("drive", "Drive", 0.0f, 48.0f, 12.0f, "dB", false, 1);
        tone  = addParameter ("tone", "Tono", 500.0f, 20000.0f, 12000.0f, "Hz", true, 0);
        out   = addParameter ("out", "Salida", -36.0f, 12.0f, -6.0f, "dB", false, 1);
        mix   = addParameter ("mix", "Mezcla", 0.0f, 1.0f, 1.0f, "%", false, 0);
    }

protected:
    void onPrepare() override { sd.reset (sampleRate, 0.02); }
    void onReset() override
    {
        lpL.reset(); lpR.reset(); dcL.reset(); dcR.reset();
        sd.setCurrentAndTarget (dbToGain (drive->get()));
    }

    static inline float shape (float v, int t)
    {
        switch (t)
        {
            case 1: return std::clamp (v, -1.0f, 1.0f);
            case 2:
            {
                const float p = 0.25f * v + 0.25f;
                return 4.0f * std::abs (p - std::round (p)) - 1.0f;
            }
            case 3: return v >= 0.0f ? std::tanh (v) : 0.5f * std::tanh (2.0f * v) + 0.1f * v;
            default: return std::tanh (v);
        }
    }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        const int t = type->getIndex();
        sd.setTarget (dbToGain (drive->get()));
        lpL.setCutoff (tone->get(), sampleRate);
        lpR.setCutoff (tone->get(), sampleRate);
        const float og = dbToGain (out->get()), wet = mix->get();

        for (int i = 0; i < io.numSamples; ++i)
        {
            const float d = sd.next();
            const float xl = io.l[i], xr = io.r[i];
            const float yl = dcL.process (lpL.process (shape (xl * d, t))) * og;
            const float yr = dcR.process (lpR.process (shape (xr * d, t))) * og;
            io.l[i] = xl * (1.0f - wet) + yl * wet;
            io.r[i] = xr * (1.0f - wet) + yr * wet;
        }
    }

private:
    Parameter *type, *drive, *tone, *out, *mix;
    OnePoleLP lpL, lpR;
    DCBlocker dcL, dcR;
    SmoothedValue sd;
};

// =============================================================================
//  Bitcrusher
// =============================================================================
class BitcrusherNode : public Node
{
public:
    BitcrusherNode() : Node ("bitcrusher", "Bitcrusher")
    {
        bits       = addParameter ("bits", "Bits", 1.0f, 16.0f, 8.0f, "bit", false, 0);
        downsample = addParameter ("downsample", "Reduccion", 1.0f, 64.0f, 4.0f, "x", true, 0);
        mix        = addParameter ("mix", "Mezcla", 0.0f, 1.0f, 1.0f, "%", false, 0);
    }

protected:
    void onReset() override { counter = 0; holdL = holdR = 0.0f; }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        const float levels = std::pow (2.0f, std::round (bits->get()) - 1.0f);
        const int factor = std::max (1, (int) std::lround (downsample->get()));
        const float wet = mix->get();

        for (int i = 0; i < io.numSamples; ++i)
        {
            if (counter <= 0)
            {
                holdL = std::round (io.l[i] * levels) / levels;
                holdR = std::round (io.r[i] * levels) / levels;
                counter = factor;
            }
            --counter;
            io.l[i] = io.l[i] * (1.0f - wet) + holdL * wet;
            io.r[i] = io.r[i] * (1.0f - wet) + holdR * wet;
        }
    }

private:
    Parameter *bits, *downsample, *mix;
    int counter = 0;
    float holdL = 0.0f, holdR = 0.0f;
};

// =============================================================================
//  Tremolo / Auto-pan (fase estereo 180 = auto-pan)
// =============================================================================
class TremoloNode : public Node
{
public:
    TremoloNode() : Node ("tremolo", "Tremolo / Auto-pan")
    {
        rate   = addParameter ("rate", "Velocidad", 0.1f, 20.0f, 5.0f, "Hz", true, 2);
        depth  = addParameter ("depth", "Profundidad", 0.0f, 1.0f, 0.5f, "%", false, 0);
        shape  = addChoice ("shape", "Forma", { "Seno", "Triangulo", "Cuadrada" }, 0);
        stereo = addParameter ("stereo", "Fase estereo", 0.0f, 180.0f, 0.0f, "deg", false, 0);
    }

protected:
    void onReset() override { lfo.reset(); gl = gr = 1.0f; }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        lfo.setRate (rate->get(), sampleRate);
        const float dep = depth->get();
        const int s = shape->getIndex();
        const double off = stereo->get() / 360.0;
        const float smooth = 1.0f - msToCoef (2.0f, sampleRate);

        for (int i = 0; i < io.numSamples; ++i)
        {
            const float tl = 1.0f - dep * (0.5f - 0.5f * lfo.valueAt (0.0, s));
            const float tr = 1.0f - dep * (0.5f - 0.5f * lfo.valueAt (off, s));
            lfo.advance();
            gl += (tl - gl) * smooth;
            gr += (tr - gr) * smooth;
            io.l[i] *= gl; io.r[i] *= gr;
        }
    }

private:
    Parameter *rate, *depth, *shape, *stereo;
    LFO lfo;
    float gl = 1.0f, gr = 1.0f;
};

// =============================================================================
//  Pitch shifter (dos lecturas de delay cruzadas con ventana)
// =============================================================================
class PitchShiftNode : public Node
{
public:
    PitchShiftNode() : Node ("pitch", "Pitch Shift")
    {
        semitones = addParameter ("semitones", "Semitonos", -12.0f, 12.0f, 0.0f, "st", false, 0);
        cents     = addParameter ("cents", "Cents", -100.0f, 100.0f, 0.0f, "ct", false, 0);
        window    = addParameter ("window", "Ventana", 20.0f, 200.0f, 60.0f, "ms", false, 0);
        mix       = addParameter ("mix", "Mezcla", 0.0f, 1.0f, 1.0f, "%", false, 0);
    }

protected:
    void onPrepare() override
    {
        const int maxD = (int) (0.25 * sampleRate);
        dl.setMaxDelaySamples (maxD); dr.setMaxDelaySamples (maxD);
    }
    void onReset() override { dl.clear(); dr.clear(); phase = 0.0; }

    void process (const ProcessContext&, AudioBuf& io) override
    {
        const double ratio = std::pow (2.0, (semitones->get() + cents->get() * 0.01) / 12.0);
        const double win = window->get() * 0.001 * sampleRate;
        const double inc = (1.0 - ratio) / win;
        const float wet = mix->get();

        for (int i = 0; i < io.numSamples; ++i)
        {
            double p2 = phase + 0.5; p2 -= std::floor (p2);
            const float d1 = (float) (1.0 + phase * win);
            const float d2 = (float) (1.0 + p2 * win);
            const float g1 = (float) std::sin (3.14159265358979 * phase);
            const float g2 = (float) std::sin (3.14159265358979 * p2);

            const float yl = dl.read (d1) * g1 + dl.read (d2) * g2;
            const float yr = dr.read (d1) * g1 + dr.read (d2) * g2;
            const float xl = io.l[i], xr = io.r[i];
            dl.push (xl); dr.push (xr);

            phase += inc;
            phase -= std::floor (phase);

            io.l[i] = xl * (1.0f - wet) + yl * wet;
            io.r[i] = xr * (1.0f - wet) + yr * wet;
        }
    }

private:
    Parameter *semitones, *cents, *window, *mix;
    DelayLine dl, dr;
    double phase = 0.0;
};

template <typename T>
std::function<std::unique_ptr<Node>()> maker()
{
    return [] { return std::unique_ptr<Node> (new T()); };
}

} // anonymous namespace

// =============================================================================
void registerBuiltinNodes (NodeFactory& f)
{
    f.registerType ({ "input",  "Entrada", "Sistema", "Audio que entra al grafo", maker<InputNode>(),  false });
    f.registerType ({ "output", "Salida",  "Sistema", "Audio que sale del grafo", maker<OutputNode>(), false });

    f.registerType ({ "gain",   "Ganancia",       "Utilidad", "Sube o baja el volumen, invierte fase", maker<GainNode>() });
    f.registerType ({ "pan",    "Paneo",          "Utilidad", "Balance izquierda/derecha", maker<PanNode>() });
    f.registerType ({ "stereo", "Imagen estereo", "Utilidad", "Ancho estereo, mono, invertir/intercambiar canales", maker<StereoNode>() });
    f.registerType ({ "mixer",  "Mezclador",      "Utilidad", "Suma hasta 4 senales (procesamiento paralelo)", maker<MixerNode>() });

    f.registerType ({ "eq3",    "EQ 3 bandas", "Filtros", "Graves, medios y agudos", maker<EQ3Node>() });
    f.registerType ({ "filter", "Filtro",      "Filtros", "Pasa bajos/altos/banda/notch con LFO", maker<FilterNode>() });

    f.registerType ({ "compressor", "Compresor",  "Dinamica", "Controla el rango dinamico", maker<CompressorNode>() });
    f.registerType ({ "gate",       "Noise Gate", "Dinamica", "Silencia el audio bajo el umbral", maker<GateNode>() });
    f.registerType ({ "limiter",    "Limitador",  "Dinamica", "Evita que la senal pase el techo", maker<LimiterNode>() });

    f.registerType ({ "delay",  "Delay",  "Tiempo", "Eco con realimentacion y ping-pong", maker<DelayNode>() });
    f.registerType ({ "reverb", "Reverb", "Tiempo", "Reverberacion (Freeverb)", maker<ReverbNode>() });
    f.registerType ({ "chorus", "Chorus", "Modulacion", "Engrosa el sonido con copias moduladas",
                      [] { return std::unique_ptr<Node> (new ModDelayNode ("chorus", "Chorus", 0.8f, 3.0f, 15.0f, 0.0f, 0.0f, 40.0f)); } });
    f.registerType ({ "flanger", "Flanger", "Modulacion", "Barrido tipo 'avion'",
                      [] { return std::unique_ptr<Node> (new ModDelayNode ("flanger", "Flanger", 0.25f, 2.0f, 1.0f, 0.7f, -0.95f, 10.0f)); } });
    f.registerType ({ "phaser",  "Phaser",  "Modulacion", "Barrido de fase", maker<PhaserNode>() });
    f.registerType ({ "tremolo", "Tremolo / Auto-pan", "Modulacion", "Modula el volumen o el paneo", maker<TremoloNode>() });

    f.registerType ({ "distortion", "Distorsion", "Color", "Saturacion y distorsion", maker<DistortionNode>() });
    f.registerType ({ "bitcrusher", "Bitcrusher", "Color", "Reduce bits y frecuencia de muestreo", maker<BitcrusherNode>() });

    f.registerType ({ "pitch", "Pitch Shift", "Tono", "Cambia la afinacion sin cambiar la duracion", maker<PitchShiftNode>() });
}

} // namespace daw
