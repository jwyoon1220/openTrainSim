cmake_minimum_required(VERSION 3.28)
# openTrainSim: an openBVE-compatible train simulator on the phx Vulkan engine (engine/).
project(openTrainSim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif ()

# Compilers: GCC / Clang (Linux, MinGW) and MSVC (Windows). Set before the engine so it gets the
# same options.
if (MSVC)
    add_compile_options(/utf-8 /bigobj /Zc:preprocessor "$<$<COMPILE_LANGUAGE:CXX>:/permissive-;/Zc:__cplusplus;/EHsc>")
    set(OTS_WARNINGS /W3 /wd4244 /wd4267 /wd4305 /wd4996)
else ()
    set(OTS_WARNINGS -Wall -Wextra -Wno-missing-field-initializers)
endif ()
if (WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif ()

# Engine examples / tests are not needed; DLSS (SR / DLAA / frame generation) is.
set(PHX_DLSS ON CACHE BOOL "" FORCE)
set(PHX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PHX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(engine)

if (NOT WIN32)
    find_package(Iconv REQUIRED) # legacy code pages (CP949, Shift_JIS); Windows uses its own API
endif ()

# GPU-free part: text/paths, openBVE parsers, simulation.
set(OTS_CORE_SOURCES
    src/util/Text.cpp
    src/util/Paths.cpp
    src/util/Config.cpp
    src/util/Json.cpp
    src/util/Platform.cpp
    src/util/Image.cpp
    src/util/ThreadPool.cpp
    src/bve/FunctionScript.cpp
    src/bve/CsvB3dParser.cpp
    src/bve/XParser.cpp
    src/bve/AnimatedParser.cpp
    src/bve/Gltf.cpp
    src/bve/ObjectLibrary.cpp
    src/bve/RouteParser.cpp
    src/bve/TrainParser.cpp
    src/bve/Controls.cpp
    src/sim/Train.cpp
    src/sim/Plugin.cpp
    src/sim/World.cpp
    src/sim/Traffic.cpp
    src/safety/Safety.cpp
    src/safety/Ats.cpp
    src/safety/Tvm430.cpp
    src/safety/Atp.cpp
    src/route/NativeRoute.cpp
    src/terrain/Geo.cpp
    src/terrain/Track.cpp
    src/terrain/Model.cpp
    src/terrain/Mesh.cpp
    src/terrain/Terrain.cpp
    src/audio/SoundCfg.cpp
    src/audio/Vvvf.cpp
)
add_library(ots_core STATIC ${OTS_CORE_SOURCES})
target_include_directories(ots_core PUBLIC src)
target_include_directories(ots_core SYSTEM PRIVATE third_party/cgltf)
target_link_libraries(ots_core PRIVATE stb)
target_link_libraries(ots_core PUBLIC phx::core glm::glm Threads::Threads glfw)
if (NOT WIN32)
    target_link_libraries(ots_core PUBLIC Iconv::Iconv)
endif ()
target_compile_definitions(ots_core PUBLIC GLFW_INCLUDE_NONE)
target_compile_options(ots_core PRIVATE ${OTS_WARNINGS})

# Renderer + game (Vulkan).
add_library(ots_game STATIC
    src/render/TextureCache.cpp
    src/render/MeshCache.cpp
    src/render/Renderer.cpp
    src/render/Hud.cpp
    src/audio/TrainSounds.cpp
    src/audio/SoundManager.cpp
    src/cab/Cab.cpp
    src/game/Game.cpp)
target_link_libraries(ots_game PUBLIC ots_core phx::engine phx::audio)
target_compile_options(ots_game PRIVATE ${OTS_WARNINGS})

# Windows: the manifest makes UTF-8 the process code page, so the UTF-8 std::string paths used
# everywhere (Korean / Japanese file names) reach the file system intact.
set(OTS_WIN_RESOURCES)
if (WIN32)
    if (MSVC)
        set(OTS_WIN_RESOURCES src/platform/windows/openTrainSim.manifest)
    else ()
        set(OTS_WIN_RESOURCES src/platform/windows/openTrainSim.rc)
    endif ()
endif ()

add_executable(openTrainSim src/main.cpp ${OTS_WIN_RESOURCES})
target_link_libraries(openTrainSim PRIVATE ots_game)
phx_engine_executable(openTrainSim)
if (MINGW)
    target_link_options(openTrainSim PRIVATE -static) # no libstdc++ / libc++ / winpthread DLLs to ship
endif ()
phx_audio_executable(openTrainSim)

file(GLOB OTS_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag)
phx_add_shaders(ots_shaders SOURCES ${OTS_SHADERS} INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/shaders
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.glsl ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh_body.glsl)
add_dependencies(openTrainSim ots_shaders)

# Headless route / train / object checker: parses everything and prints statistics.
add_executable(ots_check tools/check.cpp ${OTS_WIN_RESOURCES})
target_link_libraries(ots_check PRIVATE ots_core)
if (MINGW)
    target_link_options(ots_check PRIVATE -static)
endif ()

# .NET train plugins (openBVE OpenBveApi.Runtime.IRuntime, e.g. ATS / ATC / ATP) run in a host
# process: Mono on Linux, the .NET Framework on Windows. Needs a C# compiler (mono's mcs, or csc
# from the .NET Framework / Visual Studio) and openBVE's OpenBveApi.dll (from an openBVE build or
# installation).
if (CMAKE_HOST_WIN32) # the compiler runs on the build machine (a cross build from Linux uses mcs)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/OpenBVE/bin_release" OTS_OPENBVE_DEFAULT)
    file(TO_CMAKE_PATH "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319" OTS_NETFX_DIR)
    find_program(OTS_MCS NAMES csc mcs HINTS ${OTS_NETFX_DIR})
else ()
    set(OTS_OPENBVE_DEFAULT "$ENV{HOME}/OpenBVE/bin_release")
    find_program(OTS_MCS NAMES mcs csc)
endif ()
set(OTS_OPENBVE_DIR "${OTS_OPENBVE_DEFAULT}" CACHE PATH "openBVE binaries (OpenBveApi.dll) for the plugin host")
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
    message(WARNING "Train plugins disabled: need a C# compiler (mcs / csc) and ${OTS_OPENBVE_DIR}/OpenBveApi.dll (set OTS_OPENBVE_DIR)")
endif ()
