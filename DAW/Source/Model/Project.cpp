#include "Project.h"
#include "../Nodes/DSPUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>

namespace daw {

namespace {

const uint32_t kPalette[] = { 0xff4a90e2, 0xffe2574a, 0xff50c878, 0xfff5a623, 0xff9b59b6,
                              0xff1abc9c, 0xffe84393, 0xfff1c40f, 0xff00a8ff, 0xffff7f50 };

inline float fadeCurve (double x)   // 0..1 -> 0..1 (curva suave)
{
    x = std::clamp (x, 0.0, 1.0);
    return (float) (x * x * (3.0 - 2.0 * x));
}

void clampFades (Clip& c)
{
    c.fadeIn  = std::clamp (c.fadeIn,  0.0, c.length);
    c.fadeOut = std::clamp (c.fadeOut, 0.0, c.length - c.fadeIn);
}

void sortClips (std::vector<Clip>& clips)
{
    std::stable_sort (clips.begin(), clips.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });
}

} // namespace

// =============================================================================
//  Track
// =============================================================================
void Track::prepare (double sr, int maxBlock)
{
    bufL.assign ((size_t) maxBlock, 0.0f);
    bufR.assign ((size_t) maxBlock, 0.0f);
    fx.prepare (sr, maxBlock);
}

// =============================================================================
//  Project: basicos
// =============================================================================
Project::Project (ClipLibrary& lib) : library (lib)
{
    mixL.assign ((size_t) maxBlockSize, 0.0f);
    mixR.assign ((size_t) maxBlockSize, 0.0f);
    masterFx.prepare (sampleRate, maxBlockSize);
}

void Project::prepare (double sr, int maxBlock)
{
    std::lock_guard<std::mutex> lock (mutex);
    sampleRate   = sr > 0.0 ? sr : 44100.0;
    maxBlockSize = std::max (16, maxBlock);
    mixL.assign ((size_t) maxBlockSize, 0.0f);
    mixR.assign ((size_t) maxBlockSize, 0.0f);
    for (auto& t : tracks)
        t->prepare (sampleRate, maxBlockSize);
    masterFx.prepare (sampleRate, maxBlockSize);
}

void Project::resetEffects()
{
    std::lock_guard<std::mutex> lock (mutex);
    for (auto& t : tracks) t->fx.reset();
    masterFx.reset();
}

std::shared_ptr<Track> Project::findTrackLocked (uint32_t trackId) const
{
    for (auto& t : tracks)
        if (t->id == trackId) return t;
    return nullptr;
}

Clip* Project::findClipLocked (uint32_t clipId, Track** owner) const
{
    for (auto& t : tracks)
        for (auto& c : t->clips)
            if (c.id == clipId)
            {
                if (owner) *owner = t.get();
                return &c;
            }
    return nullptr;
}

void Project::rememberSource (const AudioDataPtr& src)
{
    if (src != nullptr) knownSources[src->id] = src;
}

AudioDataPtr Project::lookupSource (uint32_t sourceId) const
{
    if (auto s = library.get (sourceId)) return s;
    auto it = knownSources.find (sourceId);
    return it != knownSources.end() ? it->second : nullptr;
}

// =============================================================================
//  Mezcla (hilo de audio)
// =============================================================================
void Project::render (const ProcessContext& ctx, float* left, float* right, int numSamples)
{
    std::lock_guard<std::mutex> lock (mutex);
    const double sr = sampleRate;

    bool anySolo = false;
    for (auto& t : tracks) anySolo = anySolo || t->solo.load();

    const float step = (float) (1.0 / (0.02 * sr));   // rampas de 20 ms para volumen/paneo/mute
    auto ramp = [step] (float cur, float target)
    {
        if (cur < target) return std::min (target, cur + step);
        if (cur > target) return std::max (target, cur - step);
        return cur;
    };

    for (auto& t : tracks) { t->accPeakL = 0.0f; t->accPeakR = 0.0f; }
    float mPeakL = 0.0f, mPeakR = 0.0f;

    int offset = 0;
    while (offset < numSamples)
    {
        const int n = std::min (maxBlockSize, numSamples - offset);
        const int64_t blockStart = ctx.timelineSample + offset;
        const int64_t blockEnd   = blockStart + n;

        ProcessContext sub = ctx;
        sub.timelineSample = blockStart;

        std::fill (mixL.begin(), mixL.begin() + n, 0.0f);
        std::fill (mixR.begin(), mixR.begin() + n, 0.0f);

        for (size_t ti = 0; ti < tracks.size(); ++ti)
        {
            Track& t = *tracks[ti];
            float* tl = t.bufL.data();
            float* tr = t.bufR.data();
            std::fill (tl, tl + n, 0.0f);
            std::fill (tr, tr + n, 0.0f);

            // ---- Clips -> buffer de la pista
            for (auto& c : t.clips)
            {
                if (c.muted || c.source == nullptr) continue;

                const int64_t cs = (int64_t) std::llround (c.start * sr);
                const int64_t cl = (int64_t) std::llround (c.length * sr);
                const int64_t s  = std::max (blockStart, cs);
                const int64_t e  = std::min (blockEnd, cs + cl);
                if (e <= s) continue;

                const AudioData& src = *c.source;
                const float* sl = src.getLeft();
                const float* srr = src.getRight();
                const int64_t srcLen = src.getNumSamples();
                const double srcScale = src.getSampleRate() / sr;
                const double offS = c.offset * src.getSampleRate();
                const bool sameRate = std::abs (srcScale - 1.0) < 1.0e-9;
                const float gain = dsp::dbToGain (c.gainDb);
                const double fi = c.fadeIn * sr, fo = c.fadeOut * sr;

                for (int64_t tIdx = s; tIdx < e; ++tIdx)
                {
                    const int64_t k = tIdx - cs;
                    const double pos = c.reversed ? offS + (double) (cl - 1 - k) * srcScale
                                                  : offS + (double) k * srcScale;
                    float vl, vr;
                    if (sameRate)
                    {
                        const int64_t idx = (int64_t) std::llround (pos);
                        if (idx < 0 || idx >= srcLen) continue;
                        vl = sl[idx]; vr = srr[idx];
                    }
                    else
                    {
                        const int64_t i0 = (int64_t) std::floor (pos);
                        if (i0 < 0 || i0 + 1 >= srcLen) continue;
                        const float fr = (float) (pos - (double) i0);
                        vl = sl[i0] + fr * (sl[i0 + 1] - sl[i0]);
                        vr = srr[i0] + fr * (srr[i0 + 1] - srr[i0]);
                    }

                    float env = gain;
                    if (fi > 0.0 && (double) k < fi)              env *= fadeCurve ((double) k / fi);
                    if (fo > 0.0 && (double) (cl - k) < fo)       env *= fadeCurve ((double) (cl - k) / fo);

                    const int i = (int) (tIdx - blockStart);
                    tl[i] += vl * env;
                    tr[i] += vr * env;
                }
            }

            // ---- Efectos (nodos) de la pista
            t.fx.process (sub, tl, tr, n);

            // ---- Volumen, paneo, mute, solo
            const bool audible = ! t.muted.load() && (! anySolo || t.solo.load());
            const float vol = audible ? dsp::dbToGain (t.volumeDb.load()) : 0.0f;
            const float angle = (std::clamp (t.pan.load(), -1.0f, 1.0f) + 1.0f) * dsp::kPi * 0.25f;
            const float gl = vol * std::min (1.0f, std::sqrt (2.0f) * std::cos (angle));
            const float gr = vol * std::min (1.0f, std::sqrt (2.0f) * std::sin (angle));

            float pkL = t.accPeakL, pkR = t.accPeakR;
            for (int i = 0; i < n; ++i)
            {
                t.curGainL = ramp (t.curGainL, gl);
                t.curGainR = ramp (t.curGainR, gr);
                const float l = tl[i] * t.curGainL;
                const float r = tr[i] * t.curGainR;
                pkL = std::max (pkL, std::abs (l));
                pkR = std::max (pkR, std::abs (r));
                mixL[(size_t) i] += l;
                mixR[(size_t) i] += r;
            }
            t.accPeakL = pkL; t.accPeakR = pkR;
        }

        // ---- Master
        masterFx.process (sub, mixL.data(), mixR.data(), n);
        const float mg = dsp::dbToGain (masterVolumeDb.load());
        float* L = left + offset;
        float* R = right + offset;
        for (int i = 0; i < n; ++i)
        {
            masterGainCur = ramp (masterGainCur, mg);
            L[i] = mixL[(size_t) i] * masterGainCur;
            R[i] = mixR[(size_t) i] * masterGainCur;
            mPeakL = std::max (mPeakL, std::abs (L[i]));
            mPeakR = std::max (mPeakR, std::abs (R[i]));
        }

        offset += n;
    }

    for (auto& t : tracks)
    {
        t->peakL.store (t->accPeakL, std::memory_order_relaxed);
        t->peakR.store (t->accPeakR, std::memory_order_relaxed);
    }
    masterPeakL.store (mPeakL, std::memory_order_relaxed);
    masterPeakR.store (mPeakR, std::memory_order_relaxed);
}

// =============================================================================
//  Pistas
// =============================================================================
std::shared_ptr<Track> Project::createTrackLocked (const std::string& name)
{
    auto t = std::make_shared<Track>();
    t->id = nextTrackId++;
    t->name = name.empty() ? "Pista " + std::to_string (t->id) : name;
    t->colour = kPalette[(t->id - 1) % (sizeof (kPalette) / sizeof (kPalette[0]))];
    t->prepare (sampleRate, maxBlockSize);
    return t;
}

uint32_t Project::addTrack (const std::string& name, int index)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto t = createTrackLocked (name);
    if (index < 0 || index > (int) tracks.size()) tracks.push_back (t);
    else tracks.insert (tracks.begin() + index, t);
    touch();
    return t->id;
}

bool Project::removeTrack (uint32_t trackId)
{
    std::shared_ptr<Track> removed;   // se destruye fuera del lock
    {
        std::lock_guard<std::mutex> lock (mutex);
        auto it = std::find_if (tracks.begin(), tracks.end(), [trackId] (auto& t) { return t->id == trackId; });
        if (it == tracks.end()) return false;
        removed = *it;
        tracks.erase (it);
        touch();
    }
    return true;
}

bool Project::moveTrack (uint32_t trackId, int newIndex)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto it = std::find_if (tracks.begin(), tracks.end(), [trackId] (auto& t) { return t->id == trackId; });
    if (it == tracks.end()) return false;
    auto t = *it;
    tracks.erase (it);
    newIndex = std::clamp (newIndex, 0, (int) tracks.size());
    tracks.insert (tracks.begin() + newIndex, t);
    touch();
    return true;
}

uint32_t Project::duplicateTrack (uint32_t trackId)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto src = findTrackLocked (trackId);
    if (src == nullptr) return 0;

    auto t = createTrackLocked (src->name + " (copia)");
    t->colour = src->colour;
    t->height = src->height;
    t->volumeDb.store (src->volumeDb.load());
    t->pan.store (src->pan.load());
    t->muted.store (src->muted.load());
    t->fx.fromString (src->fx.toString());
    for (auto c : src->clips)
    {
        c.id = nextClipId++;
        t->clips.push_back (c);
    }
    auto it = std::find (tracks.begin(), tracks.end(), src);
    tracks.insert (it + 1, t);
    touch();
    return t->id;
}

bool Project::renameTrack (uint32_t trackId, const std::string& name)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto t = findTrackLocked (trackId);
    if (t == nullptr) return false;
    t->name = name;
    touch();
    return true;
}

bool Project::setTrackColour (uint32_t trackId, uint32_t colour)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto t = findTrackLocked (trackId);
    if (t == nullptr) return false;
    t->colour = colour;
    touch();
    return true;
}

std::shared_ptr<Track> Project::getTrack (uint32_t trackId) const
{
    std::lock_guard<std::mutex> lock (mutex);
    return findTrackLocked (trackId);
}

std::vector<std::shared_ptr<Track>> Project::getTracks() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return tracks;
}

int Project::getTrackIndex (uint32_t trackId) const
{
    // Nota: no toma el lock (se usa tambien desde funciones que ya lo tienen)
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i]->id == trackId) return (int) i;
    return -1;
}

size_t Project::getNumTracks() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return tracks.size();
}

// =============================================================================
//  Clips
// =============================================================================
uint32_t Project::addClip (uint32_t trackId, AudioDataPtr source, double start)
{
    if (source == nullptr || source->getNumSamples() == 0) return 0;
    std::lock_guard<std::mutex> lock (mutex);
    auto t = findTrackLocked (trackId);
    if (t == nullptr) return 0;

    Clip c;
    c.id = nextClipId++;
    c.source = source;
    c.name = source->name;
    c.start = std::max (0.0, start);
    c.offset = 0.0;
    c.length = source->getLengthSeconds();
    rememberSource (source);
    t->clips.push_back (c);
    sortClips (t->clips);
    touch();
    return c.id;
}

bool Project::removeClip (uint32_t clipId)
{
    std::lock_guard<std::mutex> lock (mutex);
    for (auto& t : tracks)
    {
        auto it = std::find_if (t->clips.begin(), t->clips.end(), [clipId] (const Clip& c) { return c.id == clipId; });
        if (it != t->clips.end())
        {
            t->clips.erase (it);
            touch();
            return true;
        }
    }
    return false;
}

bool Project::moveClip (uint32_t clipId, uint32_t newTrackId, double newStart)
{
    std::lock_guard<std::mutex> lock (mutex);
    Track* owner = nullptr;
    Clip* c = findClipLocked (clipId, &owner);
    auto dest = findTrackLocked (newTrackId);
    if (c == nullptr || dest == nullptr) return false;

    Clip moved = *c;
    moved.start = std::max (0.0, newStart);
    owner->clips.erase (std::remove_if (owner->clips.begin(), owner->clips.end(),
                                        [clipId] (const Clip& x) { return x.id == clipId; }),
                        owner->clips.end());
    dest->clips.push_back (moved);
    sortClips (dest->clips);
    touch();
    return true;
}

uint32_t Project::splitClipLocked (uint32_t clipId, double time)
{
    Track* owner = nullptr;
    Clip* c = findClipLocked (clipId, &owner);
    if (c == nullptr) return 0;
    if (time <= c->start + kMinClipLength || time >= c->end() - kMinClipLength) return 0;

    Clip leftPart = *c;
    Clip rightPart = *c;
    const double leftLen = time - c->start;

    leftPart.length = leftLen;
    rightPart.start = time;
    rightPart.length = c->length - leftLen;

    if (! c->reversed)
    {
        rightPart.offset = c->offset + leftLen;
    }
    else
    {
        // Al reves: el inicio en el timeline corresponde al final del tramo fuente
        leftPart.offset  = c->offset + c->length - leftLen;
        rightPart.offset = c->offset;
    }

    leftPart.fadeOut = 0.0;
    rightPart.fadeIn = 0.0;
    clampFades (leftPart);
    clampFades (rightPart);
    rightPart.id = nextClipId++;

    *c = leftPart;
    owner->clips.push_back (rightPart);
    sortClips (owner->clips);
    return rightPart.id;
}

uint32_t Project::splitClip (uint32_t clipId, double time)
{
    std::lock_guard<std::mutex> lock (mutex);
    const uint32_t id = splitClipLocked (clipId, time);
    if (id != 0) touch();
    return id;
}

bool Project::trimClipStart (uint32_t clipId, double newStart)
{
    std::lock_guard<std::mutex> lock (mutex);
    Track* owner = nullptr;
    Clip* c = findClipLocked (clipId, &owner);
    if (c == nullptr) return false;

    const double srcLen = c->getSourceLength();
    // cuanto se puede extender hacia la izquierda
    const double available = c->reversed ? srcLen - (c->offset + c->length) : c->offset;
    double minStart = std::max (0.0, c->start - available);
    newStart = std::clamp (newStart, minStart, c->end() - kMinClipLength);

    const double delta = newStart - c->start;   // >0 acorta, <0 alarga
    if (! c->reversed) c->offset += delta;
    c->length -= delta;
    c->start = newStart;
    clampFades (*c);
    sortClips (owner->clips);
    touch();
    return true;
}

bool Project::trimClipEnd (uint32_t clipId, double newEnd)
{
    std::lock_guard<std::mutex> lock (mutex);
    Clip* c = findClipLocked (clipId);
    if (c == nullptr) return false;

    const double srcLen = c->getSourceLength();
    const double available = c->reversed ? c->offset : srcLen - (c->offset + c->length);
    newEnd = std::clamp (newEnd, c->start + kMinClipLength, c->end() + available);

    const double delta = newEnd - c->end();   // >0 alarga
    if (c->reversed) c->offset -= delta;
    c->length += delta;
    c->offset = std::max (0.0, c->offset);
    clampFades (*c);
    touch();
    return true;
}

uint32_t Project::duplicateClip (uint32_t clipId)
{
    std::lock_guard<std::mutex> lock (mutex);
    Track* owner = nullptr;
    Clip* c = findClipLocked (clipId, &owner);
    if (c == nullptr) return 0;
    Clip copy = *c;
    copy.id = nextClipId++;
    copy.start = c->end();
    owner->clips.push_back (copy);
    sortClips (owner->clips);
    touch();
    return copy.id;
}

bool Project::updateClip (const Clip& updated)
{
    std::lock_guard<std::mutex> lock (mutex);
    Clip* c = findClipLocked (updated.id);
    if (c == nullptr) return false;
    c->name     = updated.name;
    c->gainDb   = std::clamp (updated.gainDb, -60.0f, 24.0f);
    c->fadeIn   = updated.fadeIn;
    c->fadeOut  = updated.fadeOut;
    c->muted    = updated.muted;
    c->colour   = updated.colour;
    c->reversed = updated.reversed;   // el mismo tramo, reproducido al reves
    clampFades (*c);
    touch();
    return true;
}

std::optional<Clip> Project::getClip (uint32_t clipId) const
{
    std::lock_guard<std::mutex> lock (mutex);
    if (Clip* c = findClipLocked (clipId)) return *c;
    return std::nullopt;
}

std::vector<Clip> Project::getClips (uint32_t trackId) const
{
    std::lock_guard<std::mutex> lock (mutex);
    auto t = findTrackLocked (trackId);
    return t ? t->clips : std::vector<Clip>();
}

uint32_t Project::findTrackOfClip (uint32_t clipId) const
{
    std::lock_guard<std::mutex> lock (mutex);
    Track* owner = nullptr;
    return findClipLocked (clipId, &owner) ? owner->id : 0;
}

// =============================================================================
//  Operaciones por rango
// =============================================================================
std::vector<std::shared_ptr<Track>> Project::tracksForRangeLocked (uint32_t trackId) const
{
    if (trackId == 0) return tracks;
    if (auto t = findTrackLocked (trackId)) return { t };
    return {};
}

int Project::splitAt (double time, uint32_t trackId)
{
    std::lock_guard<std::mutex> lock (mutex);
    int count = 0;
    for (auto& t : tracksForRangeLocked (trackId))
    {
        std::vector<uint32_t> ids;
        for (auto& c : t->clips)
            if (time > c.start && time < c.end()) ids.push_back (c.id);
        for (auto id : ids)
            if (splitClipLocked (id, time) != 0) ++count;
    }
    if (count > 0) touch();
    return count;
}

bool Project::deleteRange (double t0, double t1, uint32_t trackId, bool ripple)
{
    if (t1 < t0) std::swap (t0, t1);
    if (t1 - t0 <= 0.0) return false;

    std::lock_guard<std::mutex> lock (mutex);
    bool changed = false;
    const double width = t1 - t0;

    for (auto& t : tracksForRangeLocked (trackId))
    {
        std::vector<uint32_t> ids;
        for (auto& c : t->clips) ids.push_back (c.id);
        for (auto id : ids)
        {
            Clip* c = findClipLocked (id);
            if (c == nullptr) continue;
            if (t0 > c->start && t0 < c->end()) splitClipLocked (id, t0);
        }
        ids.clear();
        for (auto& c : t->clips) ids.push_back (c.id);
        for (auto id : ids)
        {
            Clip* c = findClipLocked (id);
            if (c == nullptr) continue;
            if (t1 > c->start && t1 < c->end()) splitClipLocked (id, t1);
        }

        const size_t before = t->clips.size();
        t->clips.erase (std::remove_if (t->clips.begin(), t->clips.end(), [&] (const Clip& c)
                        { return c.start >= t0 - 1.0e-9 && c.end() <= t1 + 1.0e-9; }),
                        t->clips.end());
        changed = changed || t->clips.size() != before;

        if (ripple)
            for (auto& c : t->clips)
                if (c.start >= t1 - 1.0e-9) { c.start -= width; changed = true; }

        sortClips (t->clips);
    }

    if (changed) touch();
    return changed;
}

bool Project::insertSilence (double time, double duration, uint32_t trackId)
{
    if (duration <= 0.0) return false;
    std::lock_guard<std::mutex> lock (mutex);
    bool changed = false;
    for (auto& t : tracksForRangeLocked (trackId))
    {
        std::vector<uint32_t> ids;
        for (auto& c : t->clips)
            if (time > c.start && time < c.end()) ids.push_back (c.id);
        for (auto id : ids) splitClipLocked (id, time);
        for (auto& c : t->clips)
            if (c.start >= time - 1.0e-9) { c.start += duration; changed = true; }
        sortClips (t->clips);
    }
    if (changed) touch();
    return changed;
}

double Project::getLength() const
{
    std::lock_guard<std::mutex> lock (mutex);
    double len = 0.0;
    for (auto& t : tracks)
        for (auto& c : t->clips) len = std::max (len, c.end());
    return len;
}

bool Project::isSourceUsed (uint32_t sourceId) const
{
    std::lock_guard<std::mutex> lock (mutex);
    for (auto& t : tracks)
        for (auto& c : t->clips)
            if (c.source && c.source->id == sourceId) return true;
    return false;
}

void Project::replaceSources (const std::vector<std::pair<AudioDataPtr, AudioDataPtr>>& changes)
{
    std::lock_guard<std::mutex> lock (mutex);
    for (auto& [oldSrc, newSrc] : changes)
    {
        for (auto& t : tracks)
            for (auto& c : t->clips)
                if (c.source == oldSrc) c.source = newSrc;
        rememberSource (newSrc);
    }
    touch();
}

// =============================================================================
//  Guardado en texto
// =============================================================================
std::string Project::toString() const
{
    // Copiar lo necesario con el lock y serializar los grafos fuera de el
    struct TrackCopy { uint32_t id, colour; float height, vol, pan; bool mute, solo; std::string name; std::vector<Clip> clips; const NodeGraph* fx; };
    std::vector<TrackCopy> copy;
    std::vector<std::shared_ptr<Track>> keepAlive;
    uint32_t ntid, ncid;
    {
        std::lock_guard<std::mutex> lock (mutex);
        keepAlive = tracks;
        for (auto& t : tracks)
            copy.push_back ({ t->id, t->colour, t->height, t->volumeDb.load(), t->pan.load(),
                              t->muted.load(), t->solo.load(), t->name, t->clips, &t->fx });
        ntid = nextTrackId; ncid = nextClipId;
    }

    std::ostringstream os;
    os.imbue (std::locale::classic());
    os << std::setprecision (12);
    os << "DAWPROJECT 1\n";
    os << "SETTINGS " << bpm.load() << ' ' << masterVolumeDb.load() << ' ' << ntid << ' ' << ncid << '\n';

    for (auto& t : copy)
    {
        os << "TRACK " << t.id << ' ' << t.colour << ' ' << t.height << ' ' << t.vol << ' ' << t.pan << ' '
           << (t.mute ? 1 : 0) << ' ' << (t.solo ? 1 : 0) << ' ' << std::quoted (t.name) << '\n';
        for (auto& c : t.clips)
            os << "CLIP " << t.id << ' ' << c.id << ' ' << (c.source ? c.source->id : 0) << ' '
               << c.start << ' ' << c.offset << ' ' << c.length << ' ' << c.gainDb << ' '
               << c.fadeIn << ' ' << c.fadeOut << ' ' << (c.muted ? 1 : 0) << ' ' << (c.reversed ? 1 : 0) << ' '
               << c.colour << ' ' << std::quoted (c.name) << '\n';
        const std::string g = t.fx->toString();
        os << "GRAPH " << t.id << ' ' << g.size() << '\n' << g;
    }

    const std::string mg = masterFx.toString();
    os << "MASTERGRAPH " << mg.size() << '\n' << mg;
    os << "END\n";
    return os.str();
}

bool Project::fromString (const std::string& text)
{
    std::istringstream is (text);
    is.imbue (std::locale::classic());

    std::string line;
    if (! std::getline (is, line) || line.rfind ("DAWPROJECT", 0) != 0)
        return false;

    struct TrackData { uint32_t id = 0, colour = 0; float height = 96, vol = 0, pan = 0; bool mute = false, solo = false;
                       std::string name, graph; bool hasGraph = false; std::vector<Clip> clips; };
    std::vector<TrackData> data;
    std::string masterGraph;
    bool hasMaster = false;
    double newBpm = 120.0; float newMaster = 0.0f;
    uint32_t ntid = 1, ncid = 1;

    auto readBlock = [&is] (size_t bytes)
    {
        std::string s (bytes, '\0');
        is.read (&s[0], (std::streamsize) bytes);
        s.resize ((size_t) is.gcount());
        return s;
    };

    while (std::getline (is, line))
    {
        std::istringstream ls (line);
        ls.imbue (std::locale::classic());
        std::string kind;
        ls >> kind;

        if (kind == "SETTINGS")
            ls >> newBpm >> newMaster >> ntid >> ncid;
        else if (kind == "TRACK")
        {
            TrackData td; int m = 0, s = 0;
            if (ls >> td.id >> td.colour >> td.height >> td.vol >> td.pan >> m >> s >> std::quoted (td.name))
            {
                td.mute = m != 0; td.solo = s != 0;
                data.push_back (td);
            }
        }
        else if (kind == "CLIP")
        {
            uint32_t tid, sid; Clip c; int m, r;
            if (! (ls >> tid >> c.id >> sid >> c.start >> c.offset >> c.length >> c.gainDb
                      >> c.fadeIn >> c.fadeOut >> m >> r >> c.colour >> std::quoted (c.name)))
                continue;
            c.muted = m != 0; c.reversed = r != 0;
            {
                std::lock_guard<std::mutex> lock (mutex);
                c.source = lookupSource (sid);
            }
            if (c.source == nullptr) continue;   // audio no disponible
            for (auto& td : data)
                if (td.id == tid) { td.clips.push_back (c); break; }
        }
        else if (kind == "GRAPH")
        {
            uint32_t tid; size_t bytes;
            if (! (ls >> tid >> bytes)) continue;
            const std::string g = readBlock (bytes);
            for (auto& td : data)
                if (td.id == tid) { td.graph = g; td.hasGraph = true; break; }
        }
        else if (kind == "MASTERGRAPH")
        {
            size_t bytes;
            if (! (ls >> bytes)) continue;
            masterGraph = readBlock (bytes);
            hasMaster = true;
        }
        else if (kind == "END")
            break;
    }

    // Aplicar. Las pistas que ya existen se reutilizan (y si su grafo no
    // cambio, sus efectos siguen sonando sin cortes al deshacer).
    std::vector<std::shared_ptr<Track>> oldTracks;
    {
        std::lock_guard<std::mutex> lock (mutex);
        oldTracks = tracks;
    }

    std::vector<std::shared_ptr<Track>> newTracks;
    uint32_t maxTrack = 0, maxClip = 0;

    for (auto& td : data)
    {
        std::shared_ptr<Track> t;
        for (auto& o : oldTracks)
            if (o->id == td.id) { t = o; break; }

        if (t == nullptr)
        {
            t = std::make_shared<Track>();
            t->id = td.id;
            t->prepare (sampleRate, maxBlockSize);
            if (td.hasGraph) t->fx.fromString (td.graph);
        }
        else if (td.hasGraph && t->fx.toString() != td.graph)
        {
            t->fx.fromString (td.graph);
        }

        newTracks.push_back (t);
        maxTrack = std::max (maxTrack, td.id);
        for (auto& c : td.clips) maxClip = std::max (maxClip, c.id);
    }

    if (hasMaster && masterFx.toString() != masterGraph)
        masterFx.fromString (masterGraph);

    {
        std::lock_guard<std::mutex> lock (mutex);
        for (size_t i = 0; i < data.size(); ++i)
        {
            auto& t = newTracks[i];
            auto& td = data[i];
            t->name = td.name;
            t->colour = td.colour;
            t->height = td.height;
            t->volumeDb.store (td.vol);
            t->pan.store (td.pan);
            t->muted.store (td.mute);
            t->solo.store (td.solo);
            t->clips = td.clips;
            sortClips (t->clips);
            for (auto& c : t->clips) rememberSource (c.source);
        }
        tracks = newTracks;
        nextTrackId = std::max (ntid, maxTrack + 1);
        nextClipId  = std::max (ncid, maxClip + 1);
        bpm.store (newBpm);
        masterVolumeDb.store (newMaster);
        touch();
    }
    return true;
}

std::unique_ptr<Project> Project::createOfflineCopy (double sr, int maxBlock) const
{
    auto copy = std::make_unique<Project> (library);
    {
        std::lock_guard<std::mutex> lock (mutex);
        copy->knownSources = knownSources;
    }
    copy->prepare (sr, maxBlock);
    copy->fromString (toString());
    return copy;
}

void Project::clear()
{
    std::vector<std::shared_ptr<Track>> old;
    {
        std::lock_guard<std::mutex> lock (mutex);
        old.swap (tracks);
        nextTrackId = 1;
        nextClipId = 1;
        knownSources.clear();
        touch();
    }
    masterFx.clear();
    masterVolumeDb.store (0.0f);
    bpm.store (120.0);
    clearHistory();
}

// =============================================================================
//  Deshacer / rehacer
// =============================================================================
void Project::checkpoint (const std::string& label)
{
    const std::string state = toString();
    std::lock_guard<std::mutex> lock (historyMutex);
    if (! undoStack.empty() && undoStack.back().state == state)
        return;   // nada cambio desde el ultimo checkpoint
    undoStack.push_back ({ state, label });
    if (undoStack.size() > 200) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

bool Project::undo()
{
    HistoryEntry entry;
    {
        std::lock_guard<std::mutex> lock (historyMutex);
        if (undoStack.empty()) return false;
        entry = undoStack.back();
        undoStack.pop_back();
    }
    const std::string current = toString();
    {
        std::lock_guard<std::mutex> lock (historyMutex);
        redoStack.push_back ({ current, entry.label });
    }
    return fromString (entry.state);
}

bool Project::redo()
{
    HistoryEntry entry;
    {
        std::lock_guard<std::mutex> lock (historyMutex);
        if (redoStack.empty()) return false;
        entry = redoStack.back();
        redoStack.pop_back();
    }
    const std::string current = toString();
    {
        std::lock_guard<std::mutex> lock (historyMutex);
        undoStack.push_back ({ current, entry.label });
    }
    return fromString (entry.state);
}

bool Project::canUndo() const { std::lock_guard<std::mutex> l (historyMutex); return ! undoStack.empty(); }
bool Project::canRedo() const { std::lock_guard<std::mutex> l (historyMutex); return ! redoStack.empty(); }

std::string Project::getUndoLabel() const
{
    std::lock_guard<std::mutex> l (historyMutex);
    return undoStack.empty() ? std::string() : undoStack.back().label;
}

std::string Project::getRedoLabel() const
{
    std::lock_guard<std::mutex> l (historyMutex);
    return redoStack.empty() ? std::string() : redoStack.back().label;
}

void Project::clearHistory()
{
    std::lock_guard<std::mutex> l (historyMutex);
    undoStack.clear();
    redoStack.clear();
}

} // namespace daw
