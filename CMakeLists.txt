cmake_minimum_required(VERSION 3.28)
# openTrainSim: an openBVE-compatible train simulator on the phx Vulkan engine (engine/).
project(openTrainSim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif ()

# Engine examples / tests are not needed; DLSS (SR / DLAA / frame generation) is.
set(PHX_DLSS ON CACHE BOOL "" FORCE)
set(PHX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PHX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(engine)

find_package(Iconv REQUIRED)

# GPU-free part: text/paths, openBVE parsers, simulation.
set(OTS_CORE_SOURCES
    src/util/Text.cpp
    src/util/Paths.cpp
    src/util/Config.cpp
    src/util/ThreadPool.cpp
    src/bve/FunctionScript.cpp
    src/bve/CsvB3dParser.cpp
    src/bve/XParser.cpp
    src/bve/AnimatedParser.cpp
    src/bve/ObjectLibrary.cpp
    src/bve/RouteParser.cpp
    src/bve/TrainParser.cpp
    src/bve/Controls.cpp
    src/sim/Train.cpp
    src/sim/Plugin.cpp
    src/sim/World.cpp
    src/audio/Wav.cpp
    src/audio/SoundCfg.cpp
)
add_library(ots_core STATIC ${OTS_CORE_SOURCES})
target_include_directories(ots_core PUBLIC src)
target_link_libraries(ots_core PUBLIC phx::core glm::glm Iconv::Iconv Threads::Threads glfw)
target_compile_definitions(ots_core PUBLIC GLFW_INCLUDE_NONE)
target_compile_options(ots_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# Renderer + game (Vulkan).
add_library(ots_game STATIC
    src/render/TextureCache.cpp
    src/render/MeshCache.cpp
    src/render/Renderer.cpp
    src/render/Hud.cpp
    src/audio/Audio.cpp
    src/audio/TrainSounds.cpp
    src/game/Game.cpp)
find_package(OpenAL REQUIRED)
target_link_libraries(ots_game PUBLIC ots_core phx::engine OpenAL::OpenAL)
target_compile_options(ots_game PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(openTrainSim src/main.cpp)
target_link_libraries(openTrainSim PRIVATE ots_game)
phx_engine_executable(openTrainSim)

file(GLOB OTS_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag)
phx_add_shaders(ots_shaders SOURCES ${OTS_SHADERS} INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/shaders
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.glsl ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh_body.glsl)
add_dependencies(openTrainSim ots_shaders)

# Headless route / train / object checker: parses everything and prints statistics.
add_executable(ots_check tools/check.cpp)
target_link_libraries(ots_check PRIVATE ots_core)

# .NET train plugins (openBVE OpenBveApi.Runtime.IRuntime, e.g. ATS / ATC / ATP) run in a Mono
# host process. Needs mono's C# compiler and openBVE's OpenBveApi.dll (from an openBVE build or
# installation).
set(OTS_OPENBVE_DIR "$ENV{HOME}/OpenBVE/bin_release" CACHE PATH "openBVE binaries (OpenBveApi.dll) for the plugin host")
find_program(OTS_MCS NAMES mcs csc)
if (OTS_MCS AND EXISTS "${OTS_OPENBVE_DIR}/OpenBveApi.dll")
    set(PH_OUT ${CMAKE_BINARY_DIR}/PluginHost.exe)
    add_custom_command(OUTPUT ${PH_OUT} ${CMAKE_BINARY_DIR}/OpenBveApi.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OTS_OPENBVE_DIR}/OpenBveApi.dll ${CMAKE_BINARY_DIR}/OpenBveApi.dll
        COMMAND ${OTS_MCS} -nologo -target:exe -optimize+ -out:${PH_OUT} -r:${CMAKE_BINARY_DIR}/OpenBveApi.dll
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/PluginHost/PluginHost.cs
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/PluginHost/PluginHost.cs
        COMMENT "C# PluginHost.exe (train plugins)" VERBATIM)
    add_custom_target(ots_plugin_host ALL DEPENDS ${PH_OUT})
    add_dependencies(openTrainSim ots_plugin_host)
else ()
    message(WARNING "Train plugins disabled: need mcs/csc (mono) and ${OTS_OPENBVE_DIR}/OpenBveApi.dll (set OTS_OPENBVE_DIR)")
endif ()
