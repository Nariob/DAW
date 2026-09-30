#pragma once
// =============================================================================
//  DSPUtils.h  -  Bloques basicos de procesamiento de senal (C++17 puro)
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace daw::dsp {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

inline float dbToGain (float db)  { return db <= -100.0f ? 0.0f : std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)   { return g > 1.0e-6f ? 20.0f * std::log10 (g) : -120.0f; }
inline float flushDenormal (float x) { return std::abs (x) < 1.0e-15f ? 0.0f : x; }

// Coeficiente de un filtro de un polo para una constante de tiempo en ms
inline float msToCoef (float ms, double sampleRate)
{
    if (ms <= 0.0f) return 0.0f;
    return (float) std::exp (-1.0 / (ms * 0.001 * sampleRate));
}

// -----------------------------------------------------------------------------
//  Rampa lineal para evitar clicks al cambiar parametros
// -----------------------------------------------------------------------------
class SmoothedValue
{
public:
    void reset (double sampleRate, double rampSeconds)
    {
        rampLength = std::max (1, (int) (sampleRate * rampSeconds));
        countdown = 0;
        current = target;
    }
    void setCurrentAndTarget (float v) { current = target = v; countdown = 0; }
    void setTarget (float v)
    {
        if (v == target) return;
        target = v;
        countdown = rampLength;
        step = (target - current) / (float) rampLength;
    }
    float next()
    {
        if (countdown <= 0) return target;
        --countdown;
        current += step;
        if (countdown == 0) current = target;
        return current;
    }
    float getTarget() const   { return target; }
    bool isSmoothing() const  { return countdown > 0; }

private:
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int rampLength = 1, countdown = 0;
};

// -----------------------------------------------------------------------------
//  Biquad (Transposed Direct Form II) + disenos RBJ "Audio EQ Cookbook"
// -----------------------------------------------------------------------------
struct BiquadCoeffs { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

namespace biquad {

struct Pre { double cosw, sinw, alpha, A; };

inline Pre pre (double sr, double freq, double q, double gainDb = 0.0)
{
    freq = std::clamp (freq, 10.0, sr * 0.49);
    q    = std::max (q, 0.025);
    const double w0 = 2.0 * 3.14159265358979323846 * freq / sr;
    return { std::cos (w0), std::sin (w0), std::sin (w0) / (2.0 * q), std::pow (10.0, gainDb / 40.0) };
}

inline BiquadCoeffs norm (double b0, double b1, double b2, double a0, double a1, double a2)
{
    return { (float) (b0 / a0), (float) (b1 / a0), (float) (b2 / a0), (float) (a1 / a0), (float) (a2 / a0) };
}

inline BiquadCoeffs lowPass (double sr, double f, double q)
{
    auto p = pre (sr, f, q);
    return norm ((1 - p.cosw) / 2, 1 - p.cosw, (1 - p.cosw) / 2, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
}
inline BiquadCoeffs highPass (double sr, double f, double q)
{
    auto p = pre (sr, f, q);
    return norm ((1 + p.cosw) / 2, -(1 + p.cosw), (1 + p.cosw) / 2, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
}
inline BiquadCoeffs bandPass (double sr, double f, double q)
{
    auto p = pre (sr, f, q);
    return norm (p.alpha, 0, -p.alpha, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
}
inline BiquadCoeffs notch (double sr, double f, double q)
{
    auto p = pre (sr, f, q);
    return norm (1, -2 * p.cosw, 1, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
}
inline BiquadCoeffs peak (double sr, double f, double q, double gainDb)
{
    auto p = pre (sr, f, q, gainDb);
    return norm (1 + p.alpha * p.A, -2 * p.cosw, 1 - p.alpha * p.A,
                 1 + p.alpha / p.A, -2 * p.cosw, 1 - p.alpha / p.A);
}
inline BiquadCoeffs lowShelf (double sr, double f, double q, double gainDb)
{
    auto p = pre (sr, f, q, gainDb);
    const double A = p.A, c = p.cosw, s = 2 * std::sqrt (A) * p.alpha;
    return norm (A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                 (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
}
inline BiquadCoeffs highShelf (double sr, double f, double q, double gainDb)
{
    auto p = pre (sr, f, q, gainDb);
    const double A = p.A, c = p.cosw, s = 2 * std::sqrt (A) * p.alpha;
    return norm (A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                 (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
}
} // namespace biquad

struct Biquad
{
    BiquadCoeffs c;
    float z1 = 0.0f, z2 = 0.0f;

    void setCoeffs (const BiquadCoeffs& nc) { c = nc; }
    void reset() { z1 = z2 = 0.0f; }
    inline float process (float x)
    {
        const float y = c.b0 * x + z1;
        z1 = flushDenormal (c.b1 * x - c.a1 * y + z2);
        z2 = flushDenormal (c.b2 * x - c.a2 * y);
        return y;
    }
};

// -----------------------------------------------------------------------------
//  Filtros de un polo
// -----------------------------------------------------------------------------
struct OnePoleLP
{
    float a = 1.0f, y = 0.0f;
    void setCutoff (double freq, double sr)
    {
        freq = std::clamp (freq, 1.0, sr * 0.49);
        a = (float) (1.0 - std::exp (-2.0 * 3.14159265358979323846 * freq / sr));
    }
    void reset() { y = 0.0f; }
    inline float process (float x) { y = flushDenormal (y + (x - y) * a); return y; }
};

struct DCBlocker
{
    float x1 = 0.0f, y1 = 0.0f, r = 0.995f;
    void reset() { x1 = y1 = 0.0f; }
    inline float process (float x)
    {
        const float y = x - x1 + r * y1;
        x1 = x; y1 = flushDenormal (y);
        return y;
    }
};

// -----------------------------------------------------------------------------
//  Linea de retardo circular con interpolacion lineal.
//  Uso: primero read(d) (d >= 1 muestras de retardo), luego push(x).
// -----------------------------------------------------------------------------
class DelayLine
{
public:
    void setMaxDelaySamples (int maxDelay)
    {
        int size = 1;
        while (size < maxDelay + 4) size <<= 1;
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        writePos = 0;
    }
    void clear() { std::fill (buffer.begin(), buffer.end(), 0.0f); writePos = 0; }
    int getMaxDelay() const { return (int) buffer.size() - 4; }

    inline float read (float delaySamples) const
    {
        const float d = std::clamp (delaySamples, 1.0f, (float) (buffer.size() - 3));
        const float pos = (float) writePos - d;
        const float fl = std::floor (pos);
        const float frac = pos - fl;
        const int i0 = ((int) fl) & mask;
        const int i1 = (i0 + 1) & mask;
        return buffer[(size_t) i0] + frac * (buffer[(size_t) i1] - buffer[(size_t) i0]);
    }
    inline void push (float x)
    {
        buffer[(size_t) writePos] = flushDenormal (x);
        writePos = (writePos + 1) & mask;
    }

private:
    std::vector<float> buffer { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    int mask = 7, writePos = 0;
};

// -----------------------------------------------------------------------------
//  LFO (0 = seno, 1 = triangulo, 2 = cuadrada). Salida en [-1, 1]
// -----------------------------------------------------------------------------
struct LFO
{
    double phase = 0.0, inc = 0.0;

    void setRate (double hz, double sr) { inc = hz / sr; }
    void reset (double p = 0.0) { phase = p; }

    static float shape (double ph, int s)
    {
        switch (s)
        {
            case 1:  return ph < 0.5 ? (float) (4.0 * ph - 1.0) : (float) (3.0 - 4.0 * ph);
            case 2:  return ph < 0.5 ? 1.0f : -1.0f;
            default: return (float) std::sin (2.0 * 3.14159265358979323846 * ph);
        }
    }
    float valueAt (double offset, int s) const
    {
        double p = phase + offset;
        p -= std::floor (p);
        return shape (p, s);
    }
    void advance (int n = 1)
    {
        phase += inc * n;
        phase -= std::floor (phase);
    }
};

} // namespace daw::dsp
