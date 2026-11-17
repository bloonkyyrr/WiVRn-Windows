# CPack configuration for the Windows builds.
#
#   cmake --build build-windows --config RelWithDebInfo
#   cpack --config build-windows/CPackConfig.cmake -C RelWithDebInfo

set(CPACK_PACKAGE_NAME "WiVRn")
set(CPACK_PACKAGE_VENDOR "WiVRn")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "WiVRn PC server for Windows")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/WiVRn/WiVRn")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "WiVRn")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

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
