#pragma once
// =============================================================================
//  ClipLibrary.h  -  Audios disponibles en el proyecto (pestana "Clips").
//  Aqui llegan los archivos que se suben y los recortes que el usuario
//  guarda desde el timeline. Desde aqui se arrastran a las pistas.
// =============================================================================
#include "AudioData.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace daw {

class ClipLibrary
{
public:
    // Agrega un audio (si su frecuencia no es la del proyecto, se remuestrea).
    // Devuelve el audio ya guardado (con id asignado).
    AudioDataPtr add (std::shared_ptr<AudioData> data, double projectSampleRate);
    // Igual que add, pero conserva el id dado (para cargar proyectos)
    AudioDataPtr addWithId (std::shared_ptr<AudioData> data, uint32_t id, double projectSampleRate);

    bool remove (uint32_t id);
    bool rename (uint32_t id, const std::string& newName);
    void clear();

    AudioDataPtr get (uint32_t id) const;
    std::vector<AudioDataPtr> getAll() const;
    size_t size() const;

    // Remuestrea todo a una nueva frecuencia (si cambia la tarjeta de sonido).
    // Devuelve pares (viejo, nuevo) para que el proyecto actualice sus clips.
    std::vector<std::pair<AudioDataPtr, AudioDataPtr>> resampleAll (double newSampleRate);

    // Aumenta con cada cambio (la UI lo usa para refrescar la lista)
    uint32_t getVersion() const { return version.load(); }

private:
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<AudioData>> items;
    uint32_t nextId = 1;
    std::atomic<uint32_t> version { 0 };
};

} // namespace daw
