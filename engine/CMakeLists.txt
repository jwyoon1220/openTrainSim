cmake_minimum_required(VERSION 3.28)
# Phx engine: Vulkan 1.3 core (context, swapchain, bindless, jobs, window), post-processing
# (DLSS SR/DLAA/FG + Reflex, TAA, FSR 1, SSAA, FXAA, bloom) and a retained-mode UI library.
# Use it from a game with add_subdirectory(engine) (or FetchContent), then
#   add_executable(MyGame ...)  phx_engine_executable(MyGame)
#   phx_add_shaders(my_shaders SOURCES ... INCLUDE_DIRS ...)
project(PhxEngine LANGUAGES C CXX)

if (PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif ()

option(PHX_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(PHX_BUILD_EXAMPLES "Build the engine examples (engine/examples)" ON)
option(PHX_DLSS "NVIDIA DLSS Super Resolution / DLAA / Frame Generation via NGX" ON)

set(PHX_ENGINE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders" CACHE INTERNAL "Engine GLSL include root")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PhxShaders.cmake)

# ---------------------------------------------------------------- dependencies
include(FetchContent)

find_package(Vulkan REQUIRED GLOBAL)          # headers (+ loader, volk loads dynamically anyway)
find_package(Threads REQUIRED GLOBAL)
find_package(glm REQUIRED GLOBAL)

find_package(glfw3 3.4 QUIET GLOBAL)
if (NOT glfw3_FOUND AND NOT TARGET glfw)
    FetchContent_Declare(glfw
        URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif ()

FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/1.4.304.tar.gz)
FetchContent_Declare(vma   # header-only: SOURCE_SUBDIR keeps its CMake project out of our build
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
    SOURCE_SUBDIR _none)
set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
FetchContent_Declare(stb URL https://github.com/nothings/stb/archive/f58f558c120e9b32c217290b80bad1a0729fbb2c.tar.gz SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(volk vma stb)
if (NOT TARGET stb)
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
endif ()
if (NOT TARGET vma)
    add_library(vma INTERFACE)
    target_include_directories(vma SYSTEM INTERFACE ${vma_SOURCE_DIR}/include)
endif ()

# AMD FidelityFX Super Resolution 1 (EASU + RCAS, MIT): two headers shared by C++ (constants)
# and GLSL (shaders); fetched at configure time.
set(PHX_FSR1_DIR ${CMAKE_BINARY_DIR}/_deps/fidelityfx-fsr1-1.0.2)
foreach (f ffx_a.h ffx_fsr1.h)
    if (NOT EXISTS ${PHX_FSR1_DIR}/${f})
        message(STATUS "FidelityFX FSR 1.0.2: downloading ${f}")
        file(DOWNLOAD https://raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR/v1.0.2/ffx-fsr/${f}
             ${PHX_FSR1_DIR}/${f} STATUS st TLS_VERIFY ON)
        list(GET st 0 code)
        if (NOT code EQUAL 0)
            file(REMOVE ${PHX_FSR1_DIR}/${f})
            message(FATAL_ERROR "FidelityFX FSR download failed (${f}): ${st}")
        endif ()
    endif ()
endforeach ()

# NVIDIA DLSS (NGX SDK, "NVIDIA RTX SDKs" licence: LICENSE.txt in the SDK). Linux x86_64 only;
# headers, the static NGX loader and the DLSS runtime are fetched at configure time.
set(PHX_DLSS_VERSION 310.9.16)
set(PHX_DLSS_AVAILABLE OFF)
set(PHX_RUNTIME_FILES)
if (PHX_DLSS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(PHX_DLSS_DIR ${CMAKE_BINARY_DIR}/_deps/dlss-sdk-${PHX_DLSS_VERSION})
    set(PHX_DLSS_URL https://raw.githubusercontent.com/NVIDIA/DLSS/v${PHX_DLSS_VERSION})
    set(PHX_DLSS_FILES
        LICENSE.txt
        lib/Linux_x86_64/libnvsdk_ngx.a
        lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.${PHX_DLSS_VERSION}
        lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.${PHX_DLSS_VERSION})
    foreach (h nvsdk_ngx.h nvsdk_ngx_defs.h nvsdk_ngx_defs_dlssd.h nvsdk_ngx_defs_dlssg.h nvsdk_ngx_defs_vk.h
               nvsdk_ngx_helpers.h nvsdk_ngx_helpers_vk.h nvsdk_ngx_helpers_dlssd.h nvsdk_ngx_helpers_dlssd_vk.h
               nvsdk_ngx_helpers_dlssg.h nvsdk_ngx_helpers_dlssg_vk.h nvsdk_ngx_params.h nvsdk_ngx_params_dlssd.h
               nvsdk_ngx_params_dlssg.h nvsdk_ngx_vk.h)
        list(APPEND PHX_DLSS_FILES include/${h})
    endforeach ()
    set(PHX_DLSS_OK ON)
    foreach (f ${PHX_DLSS_FILES})
        if (NOT EXISTS ${PHX_DLSS_DIR}/${f})
            message(STATUS "DLSS SDK ${PHX_DLSS_VERSION}: downloading ${f}")
            file(DOWNLOAD ${PHX_DLSS_URL}/${f} ${PHX_DLSS_DIR}/${f}.part STATUS st TLS_VERIFY ON)
            list(GET st 0 code)
            if (code EQUAL 0)
                file(RENAME ${PHX_DLSS_DIR}/${f}.part ${PHX_DLSS_DIR}/${f})
            else ()
                file(REMOVE ${PHX_DLSS_DIR}/${f}.part)
                message(WARNING "DLSS SDK download failed (${f}): ${st}; building without DLSS")
                set(PHX_DLSS_OK OFF)
                break()
            endif ()
        endif ()
    endforeach ()
    if (PHX_DLSS_OK)
        add_library(phx_ngx STATIC IMPORTED)
        set_target_properties(phx_ngx PROPERTIES
            IMPORTED_LOCATION ${PHX_DLSS_DIR}/lib/Linux_x86_64/libnvsdk_ngx.a
            INTERFACE_INCLUDE_DIRECTORIES ${PHX_DLSS_DIR}/include
            INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS};stdc++")
        # The DLSS runtime ("snippet") is loaded by NGX from the application directory:
        # phx_engine_executable() copies it next to the executable.
        foreach (rt dlss dlssg)
            list(APPEND PHX_RUNTIME_FILES ${PHX_DLSS_DIR}/lib/Linux_x86_64/rel/libnvidia-ngx-${rt}.so.${PHX_DLSS_VERSION})
        endforeach ()
        set(PHX_DLSS_AVAILABLE ON)
    endif ()
endif ()

set(PHX_WARNINGS -Wall -Wextra)

# ---------------------------------------------------------------- core (logging, PNG, types)
add_library(phx_core STATIC
    phx/core/Log.cpp
    phx/core/Png.cpp)
add_library(phx::core ALIAS phx_core)
target_include_directories(phx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(phx_core PUBLIC cxx_std_20)
target_compile_options(phx_core PRIVATE ${PHX_WARNINGS})

# ---------------------------------------------------------------- jobs (no GPU deps, testable alone)
add_library(phx_jobs STATIC
    phx/jobs/JobSystem.cpp)
add_library(phx::jobs ALIAS phx_jobs)
target_link_libraries(phx_jobs PUBLIC phx_core Threads::Threads)
target_compile_options(phx_jobs PRIVATE ${PHX_WARNINGS})

# ---------------------------------------------------------------- UI (retained widgets → quads; GPU-free)
add_library(phx_ui STATIC
    phx/ui/Layout.cpp
    phx/ui/Painter.cpp
    phx/ui/Widgets.cpp
    phx/ui/Ui.cpp)
add_library(phx::ui ALIAS phx_ui)
target_link_libraries(phx_ui PUBLIC phx_core stb)
target_compile_options(phx_ui PRIVATE ${PHX_WARNINGS})

# ---------------------------------------------------------------- engine (Vulkan, window, post-processing)
add_library(phx_engine STATIC
    phx/gpu/BlockCompress.cpp
    phx/gpu/GpuProfiler.cpp
    phx/gpu/Resources.cpp
    phx/gpu/Swapchain.cpp
    phx/gpu/Textures.cpp
    phx/gpu/Upload.cpp
    phx/gpu/VmaImpl.cpp
    phx/gpu/VulkanContext.cpp
    phx/platform/UiInput.cpp
    phx/platform/Window.cpp
    phx/render/AntiAliasing.cpp
    phx/render/Bloom.cpp
    phx/render/UiRenderer.cpp
    phx/render/Upscaler.cpp)
add_library(phx::engine ALIAS phx_engine)
target_include_directories(phx_engine SYSTEM PRIVATE ${PHX_FSR1_DIR})
target_link_libraries(phx_engine PUBLIC phx_core phx_jobs phx_ui volk vma glfw Vulkan::Headers glm::glm stb)
target_compile_definitions(phx_engine PUBLIC GLFW_INCLUDE_NONE)
target_compile_definitions(phx_engine PRIVATE PHX_SHADER_DIR="${PHX_SHADER_OUTPUT_DIR}")
if (PHX_DLSS_AVAILABLE)
    target_link_libraries(phx_engine PUBLIC phx_ngx)
    target_compile_definitions(phx_engine PUBLIC PHX_HAS_DLSS=1 PHX_DLSS_VERSION="${PHX_DLSS_VERSION}")
endif ()
set_property(TARGET phx_engine PROPERTY PHX_RUNTIME_FILES "${PHX_RUNTIME_FILES}")
# Vulkan structs are initialised as `T x{sType}`; the remaining members are zeroed by design.
target_compile_options(phx_engine PRIVATE ${PHX_WARNINGS} -Wno-missing-field-initializers)
# Third-party implementation TU: don't hold it to our warning level.
set_source_files_properties(phx/gpu/VmaImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(phx/gpu/Textures.cpp PROPERTIES COMPILE_OPTIONS "-O2;-Wno-unused-function")
set_source_files_properties(phx/gpu/BlockCompress.cpp PROPERTIES COMPILE_OPTIONS "-O2")

# ---------------------------------------------------------------- engine shaders → <shader dir>/phx/*.spv
file(GLOB PHX_ENGINE_SHADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/phx/*.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/phx/*.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/phx/*.comp)
phx_add_shaders(phx_engine_shaders PREFIX phx SOURCES ${PHX_ENGINE_SHADERS} INCLUDE_DIRS ${PHX_FSR1_DIR})

# ---------------------------------------------------------------- examples
if (PHX_BUILD_EXAMPLES)
    add_executable(phx_ui_demo examples/ui_demo.cpp)
    phx_engine_executable(phx_ui_demo)
    target_compile_options(phx_ui_demo PRIVATE ${PHX_WARNINGS} -Wno-missing-field-initializers)
endif ()

# ---------------------------------------------------------------- tests
if (PHX_BUILD_TESTS)
    enable_testing()
    add_executable(phx_job_tests tests/JobSystemTests.cpp)
    target_link_libraries(phx_job_tests PRIVATE phx_jobs)
    add_test(NAME job_system COMMAND phx_job_tests)
    add_executable(phx_ui_tests tests/UiTests.cpp)
    target_link_libraries(phx_ui_tests PRIVATE phx_ui)
    add_test(NAME ui COMMAND phx_ui_tests)
endif ()
