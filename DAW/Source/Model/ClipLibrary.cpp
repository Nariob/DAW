#include "ClipLibrary.h"

#include <algorithm>
#include <cmath>

namespace daw {

AudioDataPtr ClipLibrary::add (std::shared_ptr<AudioData> data, double projectSampleRate)
{
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock (mutex);
        id = nextId;
    }
    return addWithId (std::move (data), id, projectSampleRate);
}

AudioDataPtr ClipLibrary::addWithId (std::shared_ptr<AudioData> data, uint32_t id, double projectSampleRate)
{
    if (data == nullptr)
        return nullptr;

    // El remuestreo se hace fuera del lock (puede tardar)
    if (projectSampleRate > 0.0 && std::abs (data->getSampleRate() - projectSampleRate) > 0.01)
    {
        auto r = data->resampled (projectSampleRate);
        r->name = data->name;
        r->filePath = data->filePath;
        data = r;
    }

    std::lock_guard<std::mutex> lock (mutex);
    for (auto& it : items)
        if (it->id == id) { id = nextId; break; }   // id ocupado -> uno nuevo

    data->id = id;
    nextId = std::max (nextId, id + 1);
    items.push_back (data);
    ++version;
    return data;
}

bool ClipLibrary::remove (uint32_t id)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto it = std::find_if (items.begin(), items.end(), [id] (auto& d) { return d->id == id; });
    if (it == items.end()) return false;
    items.erase (it);   // los clips que lo usan conservan su shared_ptr y siguen sonando
    ++version;
    return true;
}

bool ClipLibrary::rename (uint32_t id, const std::string& newName)
{
    std::lock_guard<std::mutex> lock (mutex);
    for (auto& d : items)
        if (d->id == id) { d->name = newName; ++version; return true; }
    return false;
}

void ClipLibrary::clear()
{
    std::lock_guard<std::mutex> lock (mutex);
    items.clear();
    nextId = 1;
    ++version;
}

AudioDataPtr ClipLibrary::get (uint32_t id) const
{
    std::lock_guard<std::mutex> lock (mutex);
    for (auto& d : items)
        if (d->id == id) return d;
    return nullptr;
}

std::vector<AudioDataPtr> ClipLibrary::getAll() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return { items.begin(), items.end() };
}

size_t ClipLibrary::size() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return items.size();
}

std::vector<std::pair<AudioDataPtr, AudioDataPtr>> ClipLibrary::resampleAll (double newRate)
{
    std::vector<std::shared_ptr<AudioData>> copy;
    {
        std::lock_guard<std::mutex> lock (mutex);
        copy = items;
    }

    std::vector<std::pair<AudioDataPtr, AudioDataPtr>> changes;
    std::vector<std::shared_ptr<AudioData>> updated;
    for (auto& d : copy)
    {
        if (std::abs (d->getSampleRate() - newRate) < 0.01) { updated.push_back (d); continue; }
        auto r = d->resampled (newRate);
        r->id = d->id; r->name = d->name; r->filePath = d->filePath;
        changes.push_back ({ d, r });
        updated.push_back (r);
    }

    std::lock_guard<std::mutex> lock (mutex);
    items = updated;
    ++version;
    return changes;
}

} // namespace daw
