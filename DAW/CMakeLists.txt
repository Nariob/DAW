cmake_minimum_required(VERSION 3.22)
project(MiniDAW VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DAW_BUILD_TESTS "Compilar los programas de prueba (no necesitan JUCE)" ON)

# -----------------------------------------------------------------------------
#  Nucleo en C++ puro: nodos, modelo, transporte (no depende de JUCE)
# -----------------------------------------------------------------------------
file(GLOB DAW_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Nodes/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Model/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Engine/Transport.cpp)

add_library(daw_core STATIC ${DAW_CORE_SOURCES})
target_include_directories(daw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Source)
if(MSVC)
    target_compile_options(daw_core PUBLIC /utf-8)
endif()

# -----------------------------------------------------------------------------
#  Tests (ejecutar con: ctest --output-on-failure)
# -----------------------------------------------------------------------------
if(DAW_BUILD_TESTS)
    enable_testing()
    foreach(test_name NodeGraphTest ModelTest EngineTest)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Tests/${test_name}.cpp)
            add_executable(${test_name} Tests/${test_name}.cpp)
            target_link_libraries(${test_name} PRIVATE daw_core)
            add_test(NAME ${test_name} COMMAND ${test_name})
        endif()
    endforeach()
endif()

# -----------------------------------------------------------------------------
#  Aplicacion (JUCE se descarga automaticamente la primera vez)
#  Se activa cuando existe Source/Main.cpp (parte 3: interfaz).
# -----------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Source/Main.cpp)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.3
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)

    juce_add_gui_app(MiniDAW
        PRODUCT_NAME "MiniDAW"
        COMPANY_NAME "MiniDAW"
        VERSION      ${PROJECT_VERSION})

    file(GLOB_RECURSE DAW_UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/UI/*.cpp)

    target_sources(MiniDAW PRIVATE
        Source/Main.cpp
        Source/Engine/AudioEngine.cpp
        Source/Engine/ProjectFile.cpp
        ${DAW_UI_SOURCES})

    target_compile_definitions(MiniDAW PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MiniDAW,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MiniDAW,JUCE_VERSION>")

    target_link_libraries(MiniDAW
        PRIVATE
            daw_core
            juce::juce_audio_basics
            juce::juce_audio_devices
            juce::juce_audio_formats
            juce::juce_audio_utils
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags)
else()
    message(STATUS "Source/Main.cpp aun no existe: solo se compilan el nucleo y los tests.")
endif()
