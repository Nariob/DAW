#include "ProjectFile.h"

#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

namespace daw {

ProjectFile::ProjectFile (Project& p, ClipLibrary& lib, Transport& t, AudioEngine& e)
    : project (p), library (lib), transport (t), engine (e)
{
    markAsSaved();
}

juce::File ProjectFile::audioFolderFor (const juce::File& projectFile)
{
    return projectFile.getParentDirectory().getChildFile (projectFile.getFileNameWithoutExtension() + "_audio");
}

juce::String ProjectFile::getDisplayName() const
{
    return hasFile() ? currentFile.getFileNameWithoutExtension() : juce::String ("Sin titulo");
}

bool ProjectFile::hasUnsavedChanges() const
{
    return project.getVersion() != savedProjectVersion || library.getVersion() != savedLibraryVersion;
}

void ProjectFile::markAsSaved()
{
    savedProjectVersion = project.getVersion();
    savedLibraryVersion = library.getVersion();
}

void ProjectFile::newProject()
{
    transport.pause();
    transport.setPosition (0.0);
    transport.loopEnabled = false;
    project.clear();
    library.clear();
    currentFile = juce::File();
    written.clear();
    markAsSaved();
}

// =============================================================================
//  Guardar
// =============================================================================
bool ProjectFile::save (const juce::File& fileIn, juce::String& error)
{
    juce::File file = fileIn;
    if (! file.hasFileExtension (getExtension()))
        file = file.withFileExtension (getExtension());

    if (file != currentFile)
        written.clear();

    const juce::File audioDir = audioFolderFor (file);
    if (! audioDir.isDirectory() && ! audioDir.createDirectory())
    {
        error = "No se pudo crear la carpeta " + audioDir.getFullPathName();
        return false;
    }

    // Audios: los de la biblioteca + los que usan los clips (aunque se hayan
    // quitado de la biblioteca)
    std::vector<AudioDataPtr> sources;
    std::set<uint32_t> inLibrary, ids;
    for (auto& d : library.getAll())
    {
        sources.push_back (d);
        inLibrary.insert (d->id);
        ids.insert (d->id);
    }
    for (auto& t : project.getTracks())
        for (auto& c : project.getClips (t->getId()))
            if (c.source != nullptr && ids.insert (c.source->id).second)
                sources.push_back (c.source);

    std::ostringstream os;
    os.imbue (std::locale::classic());
    os << std::setprecision (12);
    os << "MINIDAW 1\n";

    for (auto& d : sources)
    {
        const juce::String fileName = juce::String (d->id) + ".wav";
        const juce::File target = audioDir.getChildFile (fileName);

        auto it = written.find (d->id);
        const bool upToDate = it != written.end() && it->second.lock() == d && target.existsAsFile();
        if (! upToDate)
        {
            if (! AudioEngine::writeWav (target, d->getLeft(), d->getRight(), d->getNumSamples(),
                                         d->getSampleRate(), 32, error))
                return false;
            written[d->id] = d;
        }

        os << "AUDIO " << d->id << ' ' << (inLibrary.count (d->id) ? 1 : 0) << ' '
           << std::quoted (d->name) << ' ' << std::quoted (fileName.toStdString()) << ' '
           << std::quoted (d->filePath) << '\n';
    }

    os << "TRANSPORT " << (transport.loopEnabled.load() ? 1 : 0) << ' ' << transport.loopStart.load() << ' '
       << transport.loopEnd.load() << ' ' << (transport.metronomeEnabled.load() ? 1 : 0) << ' '
       << transport.metronomeVolumeDb.load() << ' ' << transport.beatsPerBar.load() << ' '
       << transport.getPosition() << '\n';

    os << "PROJECT\n" << project.toString();
    const std::string text = os.str();

    // Guardado seguro: primero a un temporal, luego se reemplaza
    juce::TemporaryFile temp (file);
    if (! temp.getFile().replaceWithData (text.data(), text.size()) || ! temp.overwriteTargetFileWithTemporary())
    {
        error = "No se pudo escribir " + file.getFullPathName();
        return false;
    }

    // Borrar audios viejos que ya no se usan
    for (auto& f : audioDir.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        const juce::String stem = f.getFileNameWithoutExtension();
        if (stem.containsOnly ("0123456789") && ids.count ((uint32_t) stem.getLargeIntValue()) == 0)
            f.deleteFile();
    }

    currentFile = file;
    markAsSaved();
    return true;
}

// =============================================================================
//  Abrir
// =============================================================================
bool ProjectFile::load (const juce::File& file, juce::String& error, juce::StringArray& warnings)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data))
    {
        error = "No se pudo abrir " + file.getFullPathName();
        return false;
    }
    const std::string text (static_cast<const char*> (data.getData()), data.getSize());

    const std::string marker = "\nPROJECT\n";
    const size_t split = text.find (marker);
    if (text.rfind ("MINIDAW", 0) != 0 || split == std::string::npos)
    {
        error = "El archivo no es un proyecto valido.";
        return false;
    }
    const std::string header = text.substr (0, split + 1);
    const std::string projectText = text.substr (split + marker.size());

    transport.pause();
    project.clear();
    library.clear();
    written.clear();

    const juce::File audioDir = audioFolderFor (file);
    std::vector<uint32_t> hidden;
    double position = 0.0;

    std::istringstream is (header);
    is.imbue (std::locale::classic());
    std::string line;
    while (std::getline (is, line))
    {
        std::istringstream ls (line);
        ls.imbue (std::locale::classic());
        std::string kind;
        ls >> kind;

        if (kind == "AUDIO")
        {
            uint32_t id = 0; int inLib = 1; std::string name, rel, original;
            if (! (ls >> id >> inLib >> std::quoted (name) >> std::quoted (rel) >> std::quoted (original)))
                continue;

            juce::File source = audioDir.getChildFile (fromUtf8 (rel));
            bool fromFolder = true;
            if (! source.existsAsFile() && ! original.empty())
            {
                source = juce::File (fromUtf8 (original));
                fromFolder = false;
            }

            juce::String err;
            auto audio = source.existsAsFile() ? engine.loadAudioFile (source, err) : nullptr;
            if (audio == nullptr)
            {
                warnings.add ("Falta el audio \"" + fromUtf8 (name) + "\"" + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
                continue;
            }
            audio->name = name;
            audio->filePath = original;
            auto added = library.addWithId (audio, id, engine.getSampleRate());
            if (fromFolder && added == audio)
                written[id] = added;
            if (inLib == 0)
                hidden.push_back (id);
        }
        else if (kind == "TRANSPORT")
        {
            int loop = 0, metro = 0, bpb = 4; double ls0 = 0, le0 = 4; float mv = -6.0f;
            if (ls >> loop >> ls0 >> le0 >> metro >> mv >> bpb >> position)
            {
                transport.loopEnabled = loop != 0;
                transport.loopStart = ls0;
                transport.loopEnd = le0;
                transport.metronomeEnabled = metro != 0;
                transport.metronomeVolumeDb = mv;
                transport.beatsPerBar = bpb;
            }
        }
    }

    if (! project.fromString (projectText))
    {
        error = "No se pudo leer el contenido del proyecto.";
        project.clear();
        library.clear();
        return false;
    }

    // Audios que usaban los clips pero que no estaban en la biblioteca
    for (auto id : hidden)
        library.remove (id);

    project.clearHistory();
    transport.setPosition (position);
    currentFile = file;
    markAsSaved();
    return true;
}

} // namespace daw
