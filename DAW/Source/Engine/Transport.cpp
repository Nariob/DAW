#include "Transport.h"
#include "../Nodes/DSPUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace daw {

void Transport::prepare (double sr, int)
{
    const double old = sampleRate.load();
    if (sr <= 0.0) sr = 48000.0;
    if (old > 0.0 && std::abs (old - sr) > 0.01)
    {
        const double ratio = sr / old;
        position.store ((int64_t) std::llround ((double) position.load() * ratio));
        playStart.store ((int64_t) std::llround ((double) playStart.load() * ratio));
        const int64_t seek = pendingSeek.load();
        if (seek >= 0) pendingSeek.store ((int64_t) std::llround ((double) seek * ratio));
    }
    sampleRate.store (sr);
    clickRemaining = 0;
}

void Transport::play()
{
    if (playing.load()) return;
    playStart.store (getPositionSamples());
    startRequested.store (true);
    playing.store (true);
}

void Transport::pause()
{
    playing.store (false);
}

void Transport::stop()
{
    if (playing.exchange (false))
        pendingSeek.store (playStart.load());   // volver a donde empezo
    else
        pendingSeek.store (0);                  // segundo stop: al inicio
}

void Transport::setPosition (double seconds)
{
    const int64_t s = (int64_t) std::llround (std::max (0.0, seconds) * sampleRate.load());
    pendingSeek.store (s);
    if (! playing.load()) playStart.store (s);
}

int64_t Transport::getPositionSamples() const
{
    const int64_t seek = pendingSeek.load();
    return seek >= 0 ? seek : position.load();
}

double Transport::getPosition() const
{
    return (double) getPositionSamples() / sampleRate.load();
}

void Transport::process (Project& project, float* left, float* right, int numSamples)
{
    const int64_t seek = pendingSeek.exchange (-1);
    if (seek >= 0)
    {
        position.store (seek);
        clickRemaining = 0;
    }

    if (! playing.load())
    {
        std::memset (left, 0, sizeof (float) * (size_t) numSamples);
        std::memset (right, 0, sizeof (float) * (size_t) numSamples);
        return;
    }

    if (startRequested.exchange (false))
        project.resetEffects();   // sin colas viejas al empezar a reproducir

    const double sr = sampleRate.load();
    int64_t pos = position.load();

    const bool loopOn = loopEnabled.load();
    const int64_t ls = (int64_t) std::llround (loopStart.load() * sr);
    const int64_t le = (int64_t) std::llround (loopEnd.load() * sr);
    const bool validLoop = loopOn && le > ls;

    int done = 0;
    while (done < numSamples)
    {
        int seg = numSamples - done;
        bool wrap = false;
        if (validLoop && pos < le && le - pos <= seg)
        {
            seg = (int) (le - pos);
            wrap = true;
        }

        if (seg > 0)
        {
            ProcessContext ctx;
            ctx.sampleRate = sr;
            ctx.timelineSample = pos;
            ctx.isPlaying = true;
            ctx.bpm = project.bpm.load();
            project.render (ctx, left + done, right + done, seg);

            if (metronomeEnabled.load())
                addMetronome (project, left + done, right + done, seg, pos);

            pos += seg;
            done += seg;
        }

        if (wrap) pos = ls;
    }

    position.store (pos);
}

void Transport::addMetronome (const Project& project, float* left, float* right, int n, int64_t pos)
{
    const double sr  = sampleRate.load();
    const double bpm = std::clamp (project.bpm.load(), 20.0, 400.0);
    const double spb = 60.0 / bpm * sr;
    const int bpb = std::max (1, beatsPerBar.load());
    const float gain = dsp::dbToGain (metronomeVolumeDb.load());

    for (int i = 0; i < n; ++i)
    {
        const int64_t p = pos + i;
        const int64_t b  = (int64_t) std::floor ((double) p / spb);
        const int64_t bp = p == 0 ? -1 : (int64_t) std::floor ((double) (p - 1) / spb);

        if (b != bp)
        {
            const bool accent = (b % bpb) == 0;
            clickLength = std::max (1, (int) (0.035 * sr));
            clickRemaining = clickLength;
            clickPhase = 0.0;
            clickInc = 2.0 * 3.14159265358979323846 * (accent ? 1600.0 : 1000.0) / sr;
            clickAmp = accent ? 0.6f : 0.4f;
        }

        if (clickRemaining > 0)
        {
            float env = (float) clickRemaining / (float) clickLength;
            env *= env;
            const float s = (float) std::sin (clickPhase) * clickAmp * env * gain;
            clickPhase += clickInc;
            --clickRemaining;
            left[i] += s;
            right[i] += s;
        }
    }
}

} // namespace daw
