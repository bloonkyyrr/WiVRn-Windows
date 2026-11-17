set(_THIS_MODULE_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Compiled shaders are turned into C++ arrays by a CMake script, so the build does
# not need a hexdump binary (it does not exist on Windows).
set(WIVRN_SPV_TO_CXX "${_THIS_MODULE_BASE_DIR}/SpvToCxx.cmake")

# glslangValidator and spirv-opt normally come from the LunarG Vulkan SDK, but the
# validator is also usable standalone and on the PATH.
find_program(WIVRN_GLSLANGVALIDATOR NAMES glslangValidator glslangValidator.exe)
find_program(WIVRN_SPIRV_OPT NAMES spirv-opt spirv-opt.exe)

if(NOT WIVRN_GLSLANGVALIDATOR)
    message(FATAL_ERROR "glslangValidator not found: install the Vulkan SDK or add it to PATH")
endif()

if(WIVRN_OPTIMIZE_SHADERS AND NOT WIVRN_SPIRV_OPT)
    message(STATUS "spirv-opt not found: shaders will be embedded unoptimised")
endif()

function(compile_glsl_aux shader_stage shader_name glsl_filename output target_env)

    string(TOUPPER ${shader_stage} shader_stage_upper)
    cmake_path(REPLACE_FILENAME output "${shader_name}.spv" OUTPUT_VARIABLE SPV_FILE)

    file(APPEND ${output} "\
{ \"${shader_name}\", {
#include \"${shader_name}.spv\"
}},
")

    if (WIVRN_DEBUG_SHADERS)
        set(SHADER_FLAGS -gVS)
    else()
        set(SHADER_FLAGS )
    endif()

    add_custom_command(
        OUTPUT "${SPV_FILE}-nopt"
        COMMAND "${WIVRN_GLSLANGVALIDATOR}" -V --target-env ${target_env} -S ${shader_stage} -D${shader_stage_upper}_SHADER ${glsl_filename} ${SHADER_FLAGS} -o "${SPV_FILE}-nopt" --depfile "${SPV_FILE}.d"
        DEPENDS "${glsl_filename}"
        DEPFILE "${SPV_FILE}.d"
        VERBATIM
    )

    if (WIVRN_OPTIMIZE_SHADERS AND WIVRN_SPIRV_OPT)
        add_custom_command(
            OUTPUT "${SPV_FILE}-opt"
            COMMAND "${WIVRN_SPIRV_OPT}" --target-env=${target_env} -O "${SPV_FILE}-nopt" -o "${SPV_FILE}-opt"
            DEPENDS "${SPV_FILE}-nopt"
            VERBATIM
        )
        set(_spv "${SPV_FILE}-opt")
    else()
        set(_spv "${SPV_FILE}-nopt")
    endif()
    add_custom_command(
        OUTPUT "${SPV_FILE}"
        COMMAND "${CMAKE_COMMAND}" "-DSPV_IN=${_spv}" "-DSPV_OUT=${SPV_FILE}" -P "${WIVRN_SPV_TO_CXX}"
        DEPENDS "${_spv}"
        VERBATIM
    )

    get_source_file_property(deps "${output}" OBJECT_DEPENDS)
    if (deps)
        set(deps "${deps};")
    else()
        set(deps "")
    endif()
    set_source_files_properties(${output} OBJECT_DEPENDS "${deps}${SPV_FILE}")

endfunction()



function(wivrn_compile_glsl target)
    set(options)
    set(oneValueArgs TARGET_ENV NAMESPACE)
    set(multiValueArgs SHADERS)
    cmake_parse_arguments(_args "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT _args_TARGET_ENV)
        set(_args_TARGET_ENV vulkan1.0)
    endif()

    if (_args_NAMESPACE)
        set(_namespace_begin "namespace ${_args_NAMESPACE} {")
        set(_namespace_end "}")
    endif()

    cmake_path(APPEND OUTPUT "${CMAKE_CURRENT_BINARY_DIR}" "${target}_shaders.cpp")

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${target}_shaders.h" "\
#include <cstdint>
#include <map>
#include <vector>
#include <string>
${_namespace_begin}
extern const std::map<std::string, std::vector<uint32_t>> shaders;
${_namespace_end}")

    file(WRITE ${OUTPUT} "\
#include <cstdint>
#include <map>
#include <vector>
#include <string>
${_namespace_begin}
extern const std::map<std::string, std::vector<uint32_t>> shaders = {
")

    foreach(in_file IN LISTS _args_SHADERS)
        if (in_file MATCHES "\.\(vert|frag|tesc|tese|geom|comp\)\(\.glsl\)?$")
            set(shader_stage ${CMAKE_MATCH_1})
            cmake_path(GET in_file STEM LAST_ONLY shader_name)
            compile_glsl_aux(${shader_stage} ${shader_name} ${in_file} ${OUTPUT} ${_args_TARGET_ENV})
        else()
            cmake_path(GET in_file STEM LAST_ONLY shader_name)
            compile_glsl_aux(vert ${shader_name}.vert ${in_file} ${OUTPUT} ${_args_TARGET_ENV})
            compile_glsl_aux(frag ${shader_name}.frag ${in_file} ${OUTPUT} ${_args_TARGET_ENV})
        endif()


    endforeach()

    file(APPEND ${OUTPUT} "};${_namespace_end}")

    target_sources(${target} PRIVATE ${OUTPUT})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

endfunction()
