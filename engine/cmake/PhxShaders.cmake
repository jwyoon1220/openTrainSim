# phx_add_shaders(<target>
#     SOURCES <.vert/.frag/.comp ...>
#     [PREFIX <subdir>]          # output subdirectory, e.g. "phx" → loadSpirv("phx/taa.comp")
#     [INCLUDE_DIRS <dirs ...>]  # extra glslc include paths (the engine shader dir is always added)
#     [DEPENDS <files ...>])     # headers (.glsl) whose change rebuilds every shader
#
# Compiles GLSL to SPIR-V into PHX_SHADER_OUTPUT_DIR[/PREFIX] (what phx::loadSpirv reads by
# default) and creates a custom target that builds them. Games include engine headers as
# "phx/bindless.glsl" etc.
find_program(GLSLC glslc REQUIRED)

set(PHX_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders" CACHE PATH "Where compiled SPIR-V is written (engine + game)")

function(phx_add_shaders target)
    cmake_parse_arguments(ARG "" "PREFIX" "SOURCES;INCLUDE_DIRS;DEPENDS" ${ARGN})
    set(out_dir "${PHX_SHADER_OUTPUT_DIR}")
    if (ARG_PREFIX)
        string(APPEND out_dir "/${ARG_PREFIX}")
    endif ()
    set(include_flags)
    foreach (dir ${ARG_INCLUDE_DIRS} ${PHX_ENGINE_SHADER_DIR})
        list(APPEND include_flags -I ${dir})
    endforeach ()
    file(GLOB engine_headers CONFIGURE_DEPENDS ${PHX_ENGINE_SHADER_DIR}/phx/*.glsl)
    set(outputs)
    foreach (src ${ARG_SOURCES})
        get_filename_component(name ${src} NAME)
        set(out "${out_dir}/${name}.spv")
        add_custom_command(OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
            COMMAND ${GLSLC} --target-env=vulkan1.3 -O ${include_flags} -o ${out} ${src}
            DEPENDS ${src} ${ARG_DEPENDS} ${engine_headers}
            COMMENT "GLSL ${name}" VERBATIM)
        list(APPEND outputs ${out})
    endforeach ()
    add_custom_target(${target} ALL DEPENDS ${outputs})
endfunction()

# phx_engine_executable(<target>): links phx::engine, builds the engine shaders with it and
# copies the runtime files the engine loads from the executable's directory (DLSS runtime).
function(phx_engine_executable target)
    target_link_libraries(${target} PRIVATE phx::engine)
    add_dependencies(${target} phx_engine_shaders)
    get_target_property(runtime phx_engine PHX_RUNTIME_FILES)
    if (runtime)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${runtime} $<TARGET_FILE_DIR:${target}>
            VERBATIM)
    endif ()
endfunction()
