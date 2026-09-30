#include "Node.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace daw {

// =============================================================================
//  Parameter
// =============================================================================
Parameter::Parameter (std::string i, std::string n, float mn, float mx, float def,
                      std::string u, bool lg, int dec)
    : id (std::move (i)), name (std::move (n)), unit (std::move (u)),
      minValue (mn), maxValue (mx), defaultValue (def),
      logarithmic (lg && mn > 0.0f), decimals (dec)
{
    set (def);
}

Parameter::Parameter (std::string i, std::string n, std::vector<std::string> ch, int defIndex, bool isToggle)
    : id (std::move (i)), name (std::move (n)),
      minValue (0.0f), maxValue ((float) std::max<size_t> (1, ch.size()) - 1.0f),
      defaultValue ((float) defIndex), decimals (0), choices (std::move (ch)), toggle (isToggle)
{
    set ((float) defIndex);
}

void Parameter::set (float v)
{
    if (! std::isfinite (v)) v = defaultValue;
    v = std::clamp (v, minValue, maxValue);
    if (isChoice()) v = std::round (v);
    value.store (v, std::memory_order_relaxed);
}

float Parameter::toNormalized (float v) const
{
    if (maxValue <= minValue) return 0.0f;
    v = std::clamp (v, minValue, maxValue);
    if (logarithmic)
        return std::log (v / minValue) / std::log (maxValue / minValue);
    return (v - minValue) / (maxValue - minValue);
}

float Parameter::fromNormalized (float n) const
{
    n = std::clamp (n, 0.0f, 1.0f);
    if (logarithmic)
        return minValue * std::pow (maxValue / minValue, n);
    return minValue + n * (maxValue - minValue);
}

std::string Parameter::formatValue (float v) const
{
    if (isChoice())
    {
        const int idx = std::clamp ((int) std::lround (v), 0, (int) choices.size() - 1);
        return choices[(size_t) idx];
    }

    char buf[64];
    if (unit == "Hz" && v >= 1000.0f)
        std::snprintf (buf, sizeof (buf), "%.2f kHz", v / 1000.0f);
    else if (unit == "%")
        std::snprintf (buf, sizeof (buf), "%.0f %%", v * 100.0f);
    else if (unit.empty())
        std::snprintf (buf, sizeof (buf), "%.*f", decimals, v);
    else
        std::snprintf (buf, sizeof (buf), "%.*f %s", decimals, v, unit.c_str());
    return buf;
}

// =============================================================================
//  Node
// =============================================================================
Node::Node (std::string type, std::string displayName)
    : name (std::move (displayName)), typeId (std::move (type))
{
}

std::string Node::getInputName (int index) const
{
    return getNumInputs() > 1 ? "In " + std::to_string (index + 1) : "In";
}

Parameter* Node::getParameter (const std::string& paramId) const
{
    for (auto& p : params)
        if (p->id == paramId)
            return p.get();
    return nullptr;
}

Parameter* Node::addParameter (const std::string& pid, const std::string& pname, float minV, float maxV,
                               float def, const std::string& unit, bool logarithmic, int decimals)
{
    params.push_back (std::make_unique<Parameter> (pid, pname, minV, maxV, def, unit, logarithmic, decimals));
    return params.back().get();
}

Parameter* Node::addChoice (const std::string& pid, const std::string& pname,
                            std::vector<std::string> choices, int defaultIndex)
{
    params.push_back (std::make_unique<Parameter> (pid, pname, std::move (choices), defaultIndex, false));
    return params.back().get();
}

Parameter* Node::addToggle (const std::string& pid, const std::string& pname, bool def)
{
    params.push_back (std::make_unique<Parameter> (pid, pname, std::vector<std::string> { "No", "Si" },
                                                   def ? 1 : 0, true));
    return params.back().get();
}

void Node::prepare (double sr, int maxBlock)
{
    sampleRate   = sr > 0.0 ? sr : 44100.0;
    maxBlockSize = std::max (1, maxBlock);
    dryL.assign ((size_t) maxBlockSize, 0.0f);
    dryR.assign ((size_t) maxBlockSize, 0.0f);
    maskBuf.assign ((size_t) maxBlockSize, 1.0f);
    onPrepare();
    onReset();
    bypassGain = bypassed.load() ? 0.0f : 1.0f;
    wasFullyBypassed = bypassed.load();
    resetPending.store (false);
}

void Node::processMulti (const ProcessContext& ctx, AudioBuf* inputs, int numInputs, AudioBuf& out)
{
    const size_t bytes = sizeof (float) * (size_t) out.numSamples;
    if (numInputs > 0)
    {
        std::memcpy (out.l, inputs[0].l, bytes);
        std::memcpy (out.r, inputs[0].r, bytes);
    }
    else
    {
        std::memset (out.l, 0, bytes);
        std::memset (out.r, 0, bytes);
    }
    process (ctx, out);
}

void Node::computeRegionMask (const ProcessContext& ctx, float* mask, int n) const
{
    const double start = regionStart.load();
    const double end   = regionEnd.load();
    const double sr    = ctx.sampleRate > 0.0 ? ctx.sampleRate : sampleRate;

    if (end <= start)
    {
        std::fill (mask, mask + n, 0.0f);
        return;
    }

    const double fade = std::min ((double) regionFadeMs.load() * 0.001, (end - start) * 0.5);

    for (int i = 0; i < n; ++i)
    {
        const double t = (double) (ctx.timelineSample + i) / sr;
        float m;
        if (t < start || t >= end)           m = 0.0f;
        else if (fade > 0.0 && t < start + fade) m = (float) ((t - start) / fade);
        else if (fade > 0.0 && t > end - fade)   m = (float) ((end - t) / fade);
        else                                     m = 1.0f;
        mask[i] = m;
    }
}

void Node::processNode (const ProcessContext& ctx, AudioBuf* inputs, int numInputs, AudioBuf& out)
{
    const int n = std::min (out.numSamples, maxBlockSize);
    out.numSamples = n;

    if (resetPending.exchange (false))
        onReset();

    // Senal seca = suma de las entradas
    float* dl = dryL.data();
    float* dr = dryR.data();
    std::fill (dl, dl + n, 0.0f);
    std::fill (dr, dr + n, 0.0f);
    for (int k = 0; k < numInputs; ++k)
        for (int i = 0; i < n; ++i) { dl[i] += inputs[k].l[i]; dr[i] += inputs[k].r[i]; }

    const bool  isBypassed = bypassed.load();
    const float target     = isBypassed ? 0.0f : 1.0f;
    const bool  useRegion  = regionEnabled.load() && supportsRegion();

    auto updatePeak = [&]
    {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
            pk = std::max (pk, std::max (std::abs (out.l[i]), std::abs (out.r[i])));
        outputPeak.store (pk, std::memory_order_relaxed);
    };

    // 1) Totalmente en bypass: pasar la senal seca y no gastar CPU
    if (isBypassed && bypassGain <= 0.0f)
    {
        std::memcpy (out.l, dl, sizeof (float) * (size_t) n);
        std::memcpy (out.r, dr, sizeof (float) * (size_t) n);
        wasFullyBypassed = true;
        updatePeak();
        return;
    }

    if (wasFullyBypassed)
    {
        onReset();   // el estado interno quedo viejo mientras estaba apagado
        wasFullyBypassed = false;
    }

    // 2) Caso normal: sin region y sin transicion de bypass
    if (! useRegion && ! isBypassed && bypassGain >= 1.0f)
    {
        processMulti (ctx, inputs, numInputs, out);
        updatePeak();
        return;
    }

    // 3) Caso general: region y/o fundido de bypass
    float* mask = maskBuf.data();
    const auto mode = (RegionMode) regionMode.load();

    if (useRegion)
        computeRegionMask (ctx, mask, n);
    else
        std::fill (mask, mask + n, 1.0f);

    if (useRegion && mode == RegionMode::Tail)
        for (int k = 0; k < numInputs; ++k)
            for (int i = 0; i < n; ++i) { inputs[k].l[i] *= mask[i]; inputs[k].r[i] *= mask[i]; }

    processMulti (ctx, inputs, numInputs, out);

    const float step = (float) (1.0 / (0.01 * sampleRate));   // fundido de bypass de 10 ms

    for (int i = 0; i < n; ++i)
    {
        float wl = out.l[i], wr = out.r[i];

        if (useRegion)
        {
            const float m = mask[i];
            if (mode == RegionMode::Tail)
            {
                wl = dl[i] * (1.0f - m) + wl;
                wr = dr[i] * (1.0f - m) + wr;
            }
            else
            {
                wl = dl[i] + (wl - dl[i]) * m;
                wr = dr[i] + (wr - dr[i]) * m;
            }
        }

        if (bypassGain < target)      bypassGain = std::min (target, bypassGain + step);
        else if (bypassGain > target) bypassGain = std::max (target, bypassGain - step);

        out.l[i] = dl[i] + (wl - dl[i]) * bypassGain;
        out.r[i] = dr[i] + (wr - dr[i]) * bypassGain;
    }

    updatePeak();
}

} // namespace daw
