# CPack configuration for the Windows builds.
#
#   cmake --build build-windows --config RelWithDebInfo
#   cpack --config build-windows/CPackConfig.cmake -C RelWithDebInfo

set(CPACK_PACKAGE_NAME "WiVRn")
set(CPACK_PACKAGE_VENDOR "WiVRn")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "WiVRn PC server for Windows")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/WiVRn/WiVRn")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "WiVRn")

# The project version is a 0.0.0 placeholder; the real version is the one git
# reports, the same string the release job publishes (v26.9-44-g2629cf3).
set(WIVRN_PACKAGE_VERSION "")
if(GIT_TAG)
    string(REGEX REPLACE "^v" "" WIVRN_PACKAGE_VERSION "${GIT_TAG}")
elseif(GIT_DESC)
    string(REGEX REPLACE "^v" "" WIVRN_PACKAGE_VERSION "${GIT_DESC}")
elseif(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _wivrn_git_desc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _wivrn_git_result
    )
    if(_wivrn_git_result EQUAL 0 AND _wivrn_git_desc)
        string(REGEX REPLACE "^v" "" WIVRN_PACKAGE_VERSION "${_wivrn_git_desc}")
    endif()
endif()
if(NOT WIVRN_PACKAGE_VERSION)
    # No git metadata, as in a source tarball: nothing better than the placeholder.
    set(WIVRN_PACKAGE_VERSION "${PROJECT_VERSION}")
endif()

set(CPACK_PACKAGE_VERSION "${WIVRN_PACKAGE_VERSION}")
# NSIS writes the major/minor/patch numbers into its version resource, so take
# the leading numbers of the version and leave the patch field at 0.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _wivrn_version_numbers "${WIVRN_PACKAGE_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(CPACK_PACKAGE_VERSION_PATCH "0")
if(NOT CPACK_PACKAGE_VERSION_MAJOR)
    set(CPACK_PACKAGE_VERSION_MAJOR "0")
    set(CPACK_PACKAGE_VERSION_MINOR "0")
endif()

set(CPACK_PACKAGE_FILE_NAME "WiVRn-${WIVRN_PACKAGE_VERSION}-win64")

set(CPACK_GENERATOR "NSIS")
set(CPACK_NSIS_DISPLAY_NAME "WiVRn")
set(CPACK_NSIS_PACKAGE_NAME "WiVRn")
set(CPACK_NSIS_MUI_ICON "")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_PACKAGE_EXECUTABLES "wivrn-server" "WiVRn server")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "WiVRn")
set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\wivrn-server.exe")

# The server and the OpenXR runtime DLLs are installed side by side in bin/,
# together with openxr_wivrn.json.
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")

include(CPack)
