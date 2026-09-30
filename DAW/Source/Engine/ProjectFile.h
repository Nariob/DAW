#pragma once
// =============================================================================
//  ProjectFile.h  -  Guardar / abrir proyectos.
//
//  MiCancion.minidaw          <- texto: pistas, clips, nodos, loop, metronomo...
//  MiCancion_audio/1.wav      <- cada audio usado, en WAV 32-bit float
//  MiCancion_audio/2.wav
//
//  La carpeta _audio hace que el proyecto sea portable (se puede copiar a
//  otro computador). Al guardar de nuevo solo se escriben los audios nuevos.
// =============================================================================
#include "AudioEngine.h"

#include <map>
#include <memory>

namespace daw {

class ProjectFile
{
public:
    ProjectFile (Project& project, ClipLibrary& library, Transport& transport, AudioEngine& engine);

    static juce::String getExtension() { return ".minidaw"; }
    static juce::String getWildcard()  { return "*.minidaw"; }

    bool save (const juce::File& file, juce::String& error);
    // 'warnings' recibe los audios que no se pudieron encontrar
    bool load (const juce::File& file, juce::String& error, juce::StringArray& warnings);
    void newProject();

    juce::File getCurrentFile() const { return currentFile; }
    bool hasFile() const              { return currentFile != juce::File(); }
    juce::String getDisplayName() const;

    bool hasUnsavedChanges() const;
    void markAsSaved();

private:
    static juce::File audioFolderFor (const juce::File& projectFile);
    static juce::String fromUtf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

    Project& project;
    ClipLibrary& library;
    Transport& transport;
    AudioEngine& engine;

    juce::File currentFile;
    std::map<uint32_t, std::weak_ptr<const AudioData>> written;   // audios ya escritos en la carpeta
    uint32_t savedProjectVersion = 0, savedLibraryVersion = 0;
};

} // namespace daw
