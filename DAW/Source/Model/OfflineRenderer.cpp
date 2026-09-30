#include "OfflineRenderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace daw {

bool OfflineRenderer::render (const Project& source, double sr, double start, double end,
                              const Sink& sink, const std::function<void (float)>& progress,
                              const std::atomic<bool>* cancel)
{
    if (sr <= 0.0 || end <= start) return false;

    constexpr int kBlock = 1024;
    auto copy = source.createOfflineCopy (sr, kBlock);

    const int64_t s0 = (int64_t) std::llround (start * sr);
    const int64_t s1 = (int64_t) std::llround (end * sr);
    const double total = (double) (s1 - s0);

    std::vector<float> L (kBlock), R (kBlock);
    int64_t pos = s0;
    float lastReported = -1.0f;

    while (pos < s1)
    {
        if (cancel != nullptr && cancel->load()) return false;

        const int n = (int) std::min<int64_t> (kBlock, s1 - pos);
        ProcessContext ctx;
        ctx.sampleRate = sr;
        ctx.timelineSample = pos;
        ctx.isPlaying = true;
        ctx.bpm = copy->bpm.load();
        copy->render (ctx, L.data(), R.data(), n);

        if (! sink (L.data(), R.data(), n)) return false;
        pos += n;

        if (progress)
        {
            const float p = (float) ((double) (pos - s0) / total);
            if (p - lastReported >= 0.01f || pos >= s1) { progress (p); lastReported = p; }
        }
    }
    return true;
}

float OfflineRenderer::findPeak (const Project& source, double sr, double start, double end,
                                 const std::function<void (float)>& progress,
                                 const std::atomic<bool>* cancel)
{
    float peak = 0.0f;
    render (source, sr, start, end, [&peak] (const float* l, const float* r, int n)
    {
        for (int i = 0; i < n; ++i) peak = std::max (peak, std::max (std::abs (l[i]), std::abs (r[i])));
        return true;
    }, progress, cancel);
    return peak;
}

} // namespace daw
