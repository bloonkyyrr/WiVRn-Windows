cmake_minimum_required(VERSION 3.28)
project(WiVRn VERSION 0.0.0 LANGUAGES C CXX)

# Set default for FetchContent projects
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(ANDROID)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/android)
endif()

# WiVRn builds the PC server for Windows and the headset client for Android.
if(NOT WIN32 AND NOT ANDROID)
    message(FATAL_ERROR "WiVRn supports Windows (server) and Android (client) only; see docs/windows.md")
endif()

# Default to PIC code
include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
include(ExternalProject)
include(GNUInstallDirs)

set(GIT_TAG "" CACHE STRING "Build for a tag (from git if empty)")
set(GIT_DESC "" CACHE STRING "Version to show in UI (from git if empty, exclusive with GIT_TAG)")
set(GIT_COMMIT "" CACHE STRING "Git SHA used to fetch client APK (from git if empty, exclusive with GIT_TAG)")

if (GIT_TAG AND (GIT_DESC OR GIT_COMMIT))
	message(FATAL_ERROR "cannot use GIT_DESC or GIT_COMMIT when using GIT_TAG")
endif()

option(WIVRN_USE_SYSTEM_FREETYPE "Use system FreeType" ON)
option(WIVRN_USE_SYSTEM_BOOST "Use system Boost headers" ON)
option(WIVRN_USE_SYSTEM_OPENXR "Use system OpenXR headers" ON)
option(WIVRN_USE_SYSTEM_LIBKTX "Use system KTX-Software library" ON)
option(WIVRN_COMPRESS_GLB "Compress textures in GLB files with gltf-transform" OFF)
option(WIVRN_USE_ANDROID_VALIDATION_LAYER "Download the android version of the Vulkan validation layer" OFF)
option(WIVRN_USE_ANDROID_BIN_LIBKTX "Download the prebuilt libktx for android" OFF)
option(WIVRN_BUILD_CLIENT "Build WiVRn client" ${ANDROID})
option(WIVRN_BUILD_SERVER "Build WiVRn server" ON)
option(WIVRN_BUILD_SERVER_LIBRARY "Build OpenXR library for WiVRn server" ${WIVRN_BUILD_SERVER})
option(WIVRN_BUILD_DISSECTOR "Build Wireshark dissector" OFF)
option(WIVRN_BUILD_TEST "Build test binaries" OFF)
option(WIVRN_WERROR "Treat warnings as errors" OFF)
option(WIVRN_OPTIMIZE_SHADERS "Run spirv-opt on compiled shaders" ON)
option(WIVRN_DEBUG_SHADERS "Compile shaders with debug information" OFF)
option(WIVRN_ADDRESS_SANITIZER "Use address sanitizer" OFF)

if(WIN32)
	# windows.h defines min and max as macros, which breaks every std::min/std::max
	# in shared code (and anything else taking a braced initializer list).
	add_compile_definitions(NOMINMAX)
	# The Vulkan Win32 headers include windows.h before winsock2.h, and the full
	# windows.h drags in the old winsock.h, which cannot coexist with winsock2.h.
	add_compile_definitions(WIN32_LEAN_AND_MEAN)
	# Enables the Vulkan Win32 external memory/handle entry points (Vulkan-Hpp guards
	# them behind this macro, Monado defines it for its own targets the same way).
	add_compile_definitions(VK_USE_PLATFORM_WIN32_KHR)
endif()

if(MSVC)
	# Without this MSVC reports __cplusplus as 199711L, so code guarded by
	# `__cplusplus >= 201703L` (Monado's u_file.cpp among others) silently takes its
	# pre-C++17 path and includes headers that no longer ship.
	add_compile_options(/Zc:__cplusplus)
	# The traditional MSVC preprocessor does not implement __VA_OPT__, which
	# common/vk/check.h uses.
	add_compile_options(/Zc:preprocessor)
	# Sources are UTF-8 without a BOM; without this MSVC reads them as the ANSI
	# codepage and rejects non-ASCII characters (quotes and symbols in comments).
	add_compile_options(/utf-8)
endif()

option(WIVRN_USE_NVENC "Enable nvenc (Nvidia) hardware encoder" ON)
option(WIVRN_USE_VULKAN_ENCODE "Enable vulkan video encoder" ON)
option(WIVRN_USE_X264 "Enable x264 software encoder" ON)

option(WIVRN_USE_PERFETTO "Enable Perfetto tracing via the Perfetto SDK (server only)" OFF)
option(WIVRN_TRACE_MONADO "Also instrument Monado (percetto) under the same WIVRN_TRACING switch. Requires WIVRN_USE_PERFETTO" OFF)
if(WIVRN_TRACE_MONADO AND NOT WIVRN_USE_PERFETTO)
	message(FATAL_ERROR "WIVRN_TRACE_MONADO=ON requires WIVRN_USE_PERFETTO=ON")
endif()

# Hardware encoder and audio backends that only exist on Linux. They are kept as
# compile-time constants so that shared code can keep its #if branches.
set(WIVRN_USE_VAAPI OFF)
set(WIVRN_USE_PIPEWIRE OFF)

# OpenVR compatibility layers: ';'-separated directories containing
# bin/win64/vrclient_x64.dll (xrizer, OpenComposite).
set(OVR_COMPAT_SEARCH_PATH "C:/Program Files/OpenComposite;C:/Program Files/xrizer"
    CACHE STRING "Where to look for the OpenVR compatibility layer, separated by ;")

option(WIVRN_FEATURE_DEBUG_GUI "Enable Monado debug GUI" OFF)
option(WIVRN_FEATURE_RENDERDOC "Support renderdoc" OFF)
option(WIVRN_FEATURE_SOLARXR "Enable SolarXR driver" ON)

# Monado's SolarXR driver depends on XRT_HAVE_LINUX: its only transport is a unix
# domain socket. It is unavailable in a Windows build, so the option is forced off
# and the server falls back to its own tracker feed.
if(WIN32 AND WIVRN_FEATURE_SOLARXR)
	message(STATUS "SolarXR driver requires Linux; building with WIVRN_FEATURE_SOLARXR=OFF")
	set(WIVRN_FEATURE_SOLARXR OFF)
endif()
option(WIVRN_FEATURE_STEAMVR_LIGHTHOUSE "Enable SteamVR Lighthouse driver" OFF)

set(WIVRN_CSS "${CMAKE_SOURCE_DIR}/images/wivrn-local.css" CACHE STRING "Stylesheet to use to render SVGs" )

option(WIVRN_SHOW_IMGUI_DEMO_WINDOW "Show the Dear ImGui demo window in the client" OFF)
option(WIVRN_CLIENT_DEBUG_MENU "Enable the debug menu in the client" OFF)

set(WIVRN_OPENXR_MANIFEST_TYPE_VALUES relative absolute filename)
set(WIVRN_OPENXR_MANIFEST_TYPE "relative" CACHE STRING "How to reference libraries in OpenXR manifest: ${WIVRN_OPENXR_MANIFEST_TYPE_VALUES}")
if (NOT WIVRN_OPENXR_MANIFEST_TYPE IN_LIST WIVRN_OPENXR_MANIFEST_TYPE_VALUES)
    message(FATAL_ERROR "WIVRN_OPENXR_MANIFEST_TYPE must be one of ${WIVRN_OPENXR_MANIFEST_TYPE_VALUES}")
endif()
option(WIVRN_OPENXR_MANIFEST_ABI "Generate an ABI specific OpenXR manifest")

set(WIVRN_ENVIRONMENTS_URL "https://github.com/WiVRn/WiVRn-environments/releases/latest/download/index.json" CACHE STRING "URL hosting the custom environments json file")

if (WIVRN_BUILD_CLIENT OR WIVRN_BUILD_SERVER OR WIVRN_BUILD_DISSECTOR)
    set(NEED_COMMON TRUE)
else()
    set(NEED_COMMON FALSE)
endif()

find_package(Vulkan REQUIRED)
find_package(Git)
if (NEED_COMMON)
    find_package(OpenSSL REQUIRED)
endif()
include(VulkanVersion)

if (WIVRN_USE_SYSTEM_BOOST AND NEED_COMMON)
    if (WIVRN_BUILD_DISSECTOR OR WIVRN_BUILD_CLIENT)
        find_package(Boost 1.84.0 REQUIRED COMPONENTS locale url iostreams)
    else()
        find_package(Boost 1.75.0 REQUIRED COMPONENTS locale url iostreams)
    endif()
endif()

if (WIVRN_BUILD_SERVER)
    if (Vulkan_VERSION  VERSION_LESS "1.4.304")
        message(FATAL_ERROR "Vulkan version must be at least 1.4.304, found ${Vulkan_VERSION}")
    endif()

    if (NOT WIVRN_USE_NVENC AND NOT WIVRN_USE_X264 AND NOT WIVRN_USE_VULKAN_ENCODE)
        message(FATAL_ERROR "No encoder selected, use at least one of WIVRN_USE_NVENC, WIVRN_USE_VULKAN_ENCODE or WIVRN_USE_X264")
    endif()

    if (WIVRN_USE_X264)
        find_package(x264 REQUIRED)
    endif()

    find_package(Eigen3 REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(CLI11 REQUIRED)
endif()

if (WIVRN_BUILD_SERVER)
    find_package(LibArchive REQUIRED)
    find_package(PNG REQUIRED)
endif()

if (WIVRN_BUILD_CLIENT)
    find_package(Gettext REQUIRED)
    find_program(RSVG_CONVERT rsvg-convert REQUIRED)
    find_program(KTX ktx REQUIRED)

    if (WIVRN_COMPRESS_GLB)
        find_program(GLTF_TRANSFORM gltf-transform REQUIRED)
    endif()

    if(NOT ANDROID)
        find_package(CURL REQUIRED COMPONENTS HTTP HTTPS)
    endif()
endif()

if(ANDROID)
    set(ANDROID_NDK $ENV{ANDROID_NDK_ROOT})
    set(CMAKE_ANDROID_NDK $ENV{ANDROID_NDK_ROOT})
    include(AndroidNdkModules)
    android_ndk_import_module_native_app_glue()
endif()

if (WIVRN_BUILD_DISSECTOR)
    if (WIVRN_USE_SYSTEM_OPENXR)
        find_package(OpenXR 1.1.53 REQUIRED)
    endif()
    find_package(Wireshark REQUIRED)
endif()

# Common dependencies
FetchContent_Declare(boost         EXCLUDE_FROM_ALL SYSTEM URL https://github.com/boostorg/boost/releases/download/boost-1.89.0/boost-1.89.0-cmake.tar.xz
                                   URL_HASH SHA256=67acec02d0d118b5de9eb441f5fb707b3a1cdd884be00ca24b9a73c995511f74)

# Client dependencies
FetchContent_Declare(simdjson      EXCLUDE_FROM_ALL SYSTEM URL https://github.com/simdjson/simdjson/archive/refs/tags/v4.2.2.tar.gz
                                   URL_HASH SHA256=3efae22cb41f83299fe0b2e8a187af543d3dda93abbb910586f897df670f9eaa)
FetchContent_Declare(fastgltf      EXCLUDE_FROM_ALL SYSTEM URL https://github.com/spnda/fastgltf/archive/refs/tags/v0.9.0.tar.gz
                                   URL_HASH SHA256=0bb564e127b14c22f062db50f89381dd2e0a20dbaf4987ca138a4ae8728712f9)
FetchContent_Declare(glm           EXCLUDE_FROM_ALL SYSTEM URL https://github.com/g-truc/glm/releases/download/1.0.2/glm-1.0.2.zip
                                   URL_HASH SHA256=77fff075c5ab0caf076fade41558c5458d9a242aba2b2076bb72008bdb51a35e)
FetchContent_Declare(openxr_loader EXCLUDE_FROM_ALL SYSTEM URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.58.tar.gz
                                   URL_HASH SHA256=815b933219c398314366c289221526428418d799205a73a9640c97ca4242602b)
FetchContent_Declare(spdlog        EXCLUDE_FROM_ALL SYSTEM URL https://github.com/gabime/spdlog/archive/refs/tags/v1.16.0.tar.gz
                                   URL_HASH SHA256=8741753e488a78dd0d0024c980e1fb5b5c85888447e309d9cb9d949bdb52aa3e)
FetchContent_Declare(stb           EXCLUDE_FROM_ALL SYSTEM URL https://github.com/nothings/stb/archive/013ac3beddff3dbffafd5177e7972067cd2b5083.zip
                                   URL_HASH SHA256=b7f476902bbef1b30f8ecc2d9d95c459c32302c8b559d09b589b5955463b7af8)
set(KTX_VERSION "4.4.2")
set(KTX_VERSION_FULL "v${KTX_VERSION}")
FetchContent_Declare(libktx        EXCLUDE_FROM_ALL SYSTEM URL https://github.com/KhronosGroup/KTX-Software/archive/refs/tags/${KTX_VERSION_FULL}.tar.gz
                                   URL_HASH SHA256=9412cb45045a503005acd47d98f9e8b47154634a50b4df21e17a1dfa8971d323
                                   )
FetchContent_Declare(implot        EXCLUDE_FROM_ALL SYSTEM URL https://github.com/epezent/implot/releases/download/v1.0/implot-v1.0.tar.gz
                                   URL_HASH SHA256=20ca14d5a46f36a650746096c74eba8bea99c9432cd3c877343b96b17a6ae566)
file(GLOB IMGUI_PATCHES CONFIGURE_DEPENDS patches/imgui/*)
# Hash apply.cmake so the FetchContent stamp changes whenever the patch logic
# changes: otherwise a cached source tree is never re-patched.
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/patches/apply.cmake" APPLY_CMAKE_HASH)
FetchContent_Declare(imgui         EXCLUDE_FROM_ALL SYSTEM URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz
                                   URL_HASH SHA256=af97ed649182c39314320514a672b82008ab462b9293fe23d37b30bfa5d05519
                                   PATCH_COMMAND    "${CMAKE_COMMAND}" "-DPATCHES=${IMGUI_PATCHES}" "-DAPPLY_CMAKE_HASH=${APPLY_CMAKE_HASH}" -P "${CMAKE_CURRENT_LIST_DIR}/patches/apply.cmake"
)
FetchContent_Declare(uni-algo      EXCLUDE_FROM_ALL SYSTEM URL https://github.com/uni-algo/uni-algo/archive/v1.2.0.tar.gz
                                   URL_HASH SHA256=f2a1539cd8635bc6088d05144a73ecfe7b4d74ee0361fabed6f87f9f19e74ca9)
FetchContent_Declare(entt          EXCLUDE_FROM_ALL SYSTEM URL https://github.com/skypjack/entt/archive/refs/tags/v4.0.0.tar.gz
                                   URL_HASH SHA256=32a2ff2c72cb047dfd57306006ef238820b70da7c6ce4e7e8a507ac63365212e)
FetchContent_Declare(curl          EXCLUDE_FROM_ALL SYSTEM URL https://github.com/curl/curl/releases/download/curl-8_17_0/curl-8.17.0.tar.xz
                                   URL_HASH SHA256=955f6e729ad6b3566260e8fef68620e76ba3c31acf0a18524416a185acf77992)
FetchContent_Declare(spirv-reflect EXCLUDE_FROM_ALL SYSTEM URL https://github.com/KhronosGroup/SPIRV-Reflect/archive/refs/tags/vulkan-sdk-1.4.328.1.tar.gz
                                   URL_HASH SHA256=77f4b5b5630d960d7c17e39ddd5bf2e7dd62e6d8da24259ed14ca5b59783ea35)

file(GLOB MONADO_PATCHES CONFIGURE_DEPENDS patches/monado/*)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS monado-rev)
file(STRINGS monado-rev MONADO_REV)
# Server dependencies
FetchContent_Declare(monado
    GIT_REPOSITORY   https://gitlab.freedesktop.org/monado/monado.git
    GIT_TAG          ${MONADO_REV}
    EXCLUDE_FROM_ALL
)
# Populate (stamp-gated, may skip on a cached tree) then patch unconditionally:
# FetchContent's stamp-gated PATCH_COMMAND is not re-run on a cached,
# possibly half-patched source tree, which left 0013 unapplied in CI.
FetchContent_Populate(monado)
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPATCHES=${MONADO_PATCHES}" "-DAPPLY_CMAKE_HASH=${APPLY_CMAKE_HASH}" -P "${CMAKE_CURRENT_LIST_DIR}/patches/apply.cmake"
    WORKING_DIRECTORY "${monado_SOURCE_DIR}"
    RESULT_VARIABLE _monado_patch_result
)
if(NOT _monado_patch_result EQUAL 0)
    message(FATAL_ERROR "Failed to patch monado")
endif()
# add_subdirectory is done in server/CMakeLists.txt, after the XRT_FEATURE_*
# options are set (monado_SOURCE_DIR/BINARY_DIR are already populated here).

add_custom_target(wivrn-version ALL
    COMMAND ${CMAKE_COMMAND}
        -D GIT_EXECUTABLE=${GIT_EXECUTABLE}
        -D INPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/common/version.cpp.in
        -D OUTPUT_FILE=${CMAKE_BINARY_DIR}/version.cpp
        -D INPUT_FILE_1=${CMAKE_CURRENT_SOURCE_DIR}/server/u_git_tag.cpp.in
        -D OUTPUT_FILE_1=${CMAKE_BINARY_DIR}/u_git_tag.cpp
        -D GIT_TAG="${GIT_TAG}"
        -D GIT_DESC="${GIT_DESC}"
        -D GIT_COMMIT="${GIT_COMMIT}"
        -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
        BYPRODUCTS ${CMAKE_BINARY_DIR}/version.cpp ${CMAKE_BINARY_DIR}/u_git_tag.cpp
        SOURCES common/version.cpp.in server/u_git_tag.cpp.in
    )

add_subdirectory(external)
if (NEED_COMMON)
    add_subdirectory(common)
endif()

if (WIVRN_BUILD_CLIENT)
    add_subdirectory(client)
endif()

if (WIVRN_BUILD_SERVER OR WIVRN_BUILD_SERVER_LIBRARY)
    find_package(Python3 REQUIRED Interpreter)
    add_subdirectory(server)

    if (WIVRN_BUILD_SERVER)
        message(STATUS "Selected encoders:")
        message(STATUS "\tNVENC : ${WIVRN_USE_NVENC}")
        message(STATUS "\tVulkan: ${WIVRN_USE_VULKAN_ENCODE}")
        message(STATUS "\tx264  : ${WIVRN_USE_X264}")
        message(STATUS "")
        message(STATUS "Audio backend: WASAPI")
    endif()
endif()

add_subdirectory(tools)

foreach(TARGET_NAME wivrn wivrn-server wivrn-common wivrn-dissector)
    if(TARGET ${TARGET_NAME})
        if(MSVC)
            target_compile_options(${TARGET_NAME} PRIVATE
                /W3 /permissive- /Zc:__cplusplus /EHsc
                /wd4244 /wd4267 /wd4996
                $<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>
            )
        else()
            target_compile_options(${TARGET_NAME} PRIVATE
                -fdiagnostics-color -Wall -Wextra -pedantic
                -ffast-math
                -Wundef
                -Wno-missing-field-initializers
                -Wno-unused-parameter
                -Wno-comment
                -Wno-unused-variable
                -Wno-sign-compare
                $<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-volatile -Wno-unneeded-internal-declaration -Wno-c99-extensions -Wno-unused-private-field>
            )
        endif()

        if (WIVRN_WERROR)
            if(MSVC)
                target_compile_options(${TARGET_NAME} PRIVATE /WX)
            else()
                target_compile_options(${TARGET_NAME} PRIVATE -Werror)
            endif()
        endif()

        if (WIVRN_ADDRESS_SANITIZER AND NOT ${TARGET_NAME} STREQUAL wivrn-dissector)
            if(MSVC)
                target_compile_options(${TARGET_NAME} PRIVATE /fsanitize=address)
            else()
                target_compile_options(${TARGET_NAME} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
                target_link_options(${TARGET_NAME} PRIVATE -fsanitize=address)
            endif()
        endif()

    endif()
endforeach()

if(WIN32 AND WIVRN_BUILD_SERVER)
    include(Packaging)
endif()
