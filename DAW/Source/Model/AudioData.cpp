#include "AudioData.h"

#include <algorithm>
#include <cmath>

namespace daw {

AudioData::AudioData (std::string n, std::string path, double sr,
                      std::vector<float> l, std::vector<float> r)
    : name (std::move (n)), filePath (std::move (path)),
      sampleRate (sr > 0.0 ? sr : 44100.0), left (std::move (l)), right (std::move (r))
{
    if (right.size() != left.size())
    {
        // Mono o tamanos distintos: el derecho copia al izquierdo
        if (right.empty()) right = left;
        right.resize (left.size(), 0.0f);
    }
    computePeaks();
}

void AudioData::computePeaks()
{
    const size_t blocks = (left.size() + kPeakBlock - 1) / kPeakBlock;
    peakMin.assign (blocks, 0.0f);
    peakMax.assign (blocks, 0.0f);
    for (size_t b = 0; b < blocks; ++b)
    {
        const size_t s = b * kPeakBlock;
        const size_t e = std::min (left.size(), s + kPeakBlock);
        float mn = 0.0f, mx = 0.0f;
        for (size_t i = s; i < e; ++i)
        {
            mn = std::min (mn, std::min (left[i], right[i]));
            mx = std::max (mx, std::max (left[i], right[i]));
        }
        peakMin[b] = mn;
        peakMax[b] = mx;
    }
}

void AudioData::getMinMax (int64_t s, int64_t e, float& mn, float& mx) const
{
    mn = mx = 0.0f;
    const int64_t n = getNumSamples();
    s = std::clamp<int64_t> (s, 0, n);
    e = std::clamp<int64_t> (e, 0, n);
    if (e <= s) return;

    if (e - s >= kPeakBlock * 2)
    {
        const size_t b0 = (size_t) (s / kPeakBlock);
        const size_t b1 = std::min (peakMin.size(), (size_t) ((e + kPeakBlock - 1) / kPeakBlock));
        for (size_t b = b0; b < b1; ++b) { mn = std::min (mn, peakMin[b]); mx = std::max (mx, peakMax[b]); }
        return;
    }
    for (int64_t i = s; i < e; ++i)
    {
        mn = std::min (mn, std::min (left[(size_t) i], right[(size_t) i]));
        mx = std::max (mx, std::max (left[(size_t) i], right[(size_t) i]));
    }
}

std::vector<float> resampleChannel (const std::vector<float>& in, double inRate, double outRate)
{
    if (in.empty() || inRate <= 0.0 || outRate <= 0.0 || std::abs (inRate - outRate) < 1.0e-6)
        return in;

    const double ratio  = outRate / inRate;
    const double cutoff = std::min (1.0, ratio) * 0.97;   // filtro anti-aliasing al bajar la frecuencia
    const int half = 16;
    const double scale = std::max (1.0, 1.0 / ratio);     // ancho del kernel al bajar frecuencia
    const int span = (int) std::ceil (half * scale);
    const size_t outLen = (size_t) std::llround ((double) in.size() * ratio);
    const double pi = 3.14159265358979323846;

    std::vector<float> out (outLen, 0.0f);
    const int64_t inLen = (int64_t) in.size();

    for (size_t j = 0; j < outLen; ++j)
    {
        const double x = (double) j / ratio;
        const int64_t c = (int64_t) std::floor (x);
        double acc = 0.0, wsum = 0.0;
        for (int64_t k = c - span + 1; k <= c + span; ++k)
        {
            const double t = x - (double) k;
            const double a = t / (double) span;
            if (a <= -1.0 || a >= 1.0) continue;
            const double sincArg = pi * cutoff * t;
            const double sinc = std::abs (sincArg) < 1.0e-9 ? 1.0 : std::sin (sincArg) / sincArg;
            const double win = 0.42 + 0.5 * std::cos (pi * a) + 0.08 * std::cos (2.0 * pi * a);
            const double w = sinc * win;
            wsum += w;
            if (k >= 0 && k < inLen) acc += w * in[(size_t) k];
        }
        out[j] = wsum != 0.0 ? (float) (acc / wsum) : 0.0f;
    }
    return out;
}

std::shared_ptr<AudioData> AudioData::resampled (double newRate) const
{
    return std::make_shared<AudioData> (name, filePath, newRate,
                                        resampleChannel (left, sampleRate, newRate),
                                        resampleChannel (right, sampleRate, newRate));
}

std::shared_ptr<AudioData> AudioData::extract (int64_t s, int64_t e, bool reversed, const std::string& newName) const
{
    const int64_t n = getNumSamples();
    s = std::clamp<int64_t> (s, 0, n);
    e = std::clamp<int64_t> (e, 0, n);
    if (e < s) std::swap (s, e);

    std::vector<float> l (left.begin() + s, left.begin() + e);
    std::vector<float> r (right.begin() + s, right.begin() + e);
    if (reversed) { std::reverse (l.begin(), l.end()); std::reverse (r.begin(), r.end()); }
    return std::make_shared<AudioData> (newName, std::string(), sampleRate, std::move (l), std::move (r));
}

} // namespace daw
