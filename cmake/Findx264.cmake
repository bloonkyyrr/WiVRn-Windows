# Find x264
#
# The vcpkg x264 port installs the library, its header and a pkg-config file,
# but no CMake package configuration, so find_package(x264) has to be answered
# by this module.
#
# Defines the imported target x264::x264.

find_path(X264_INCLUDE_DIR NAMES x264.h)
find_library(X264_LIBRARY NAMES x264 libx264)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(x264 REQUIRED_VARS X264_INCLUDE_DIR X264_LIBRARY)

if(x264_FOUND AND NOT TARGET x264::x264)
	add_library(x264::x264 UNKNOWN IMPORTED)
	set_target_properties(x264::x264 PROPERTIES
		IMPORTED_LOCATION "${X264_LIBRARY}"
		INTERFACE_INCLUDE_DIRECTORIES "${X264_INCLUDE_DIR}")
endif()

mark_as_advanced(X264_INCLUDE_DIR X264_LIBRARY)
