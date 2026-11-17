set(U_GIT_TAG_CPP "${CMAKE_BINARY_DIR}/u_git_tag.cpp")

if(NOT WIN32)
    message(FATAL_ERROR "wivrn-server only builds on Windows")
endif()

set(XRT_BUILD_DRIVER_ARDUINO            OFF)
set(XRT_BUILD_DRIVER_BLUBUR_S1          OFF)
set(XRT_BUILD_DRIVER_DAYDREAM           OFF)
set(XRT_BUILD_DRIVER_EUROC              OFF)
set(XRT_BUILD_DRIVER_HDK                OFF)
set(XRT_BUILD_DRIVER_HYDRA              OFF)
set(XRT_BUILD_DRIVER_NS                 OFF)
set(XRT_BUILD_DRIVER_OHMD               OFF)
set(XRT_BUILD_DRIVER_OPENGLOVES         OFF)
set(XRT_BUILD_DRIVER_PSMV               OFF)
set(XRT_BUILD_DRIVER_PSSENSE            OFF)
set(XRT_BUILD_DRIVER_PSVR               OFF)
set(XRT_BUILD_DRIVER_QWERTY             OFF)
set(XRT_BUILD_DRIVER_REMOTE             OFF)
set(XRT_BUILD_DRIVER_RIFT               OFF)
set(XRT_BUILD_DRIVER_RIFT_S             OFF)
set(XRT_BUILD_DRIVER_ROKID              OFF)
set(XRT_BUILD_DRIVER_SIMULATED          OFF)
set(XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE ${WIVRN_FEATURE_STEAMVR_LIGHTHOUSE})
set(XRT_BUILD_DRIVER_SOLARXR            ${WIVRN_FEATURE_SOLARXR})
set(XRT_BUILD_DRIVER_TWRAP              OFF)
set(XRT_BUILD_DRIVER_VF                 OFF)
set(XRT_BUILD_DRIVER_VIVE               OFF)
set(XRT_BUILD_DRIVER_WMR                OFF)
set(XRT_BUILD_DRIVER_XREAL_AIR          OFF)
set(XRT_BUILD_SAMPLES                   OFF)

set(XRT_FEATURE_COMPOSITOR_NULL                   OFF)
set(XRT_FEATURE_DEBUG_GUI                         ${WIVRN_FEATURE_DEBUG_GUI})
set(XRT_FEATURE_SERVICE                           ON)
set(XRT_FEATURE_SERVICE_SYSTEMD                   OFF)
set(XRT_FEATURE_SLAM                              OFF)
set(XRT_FEATURE_STEAMVR_PLUGIN                    OFF)
set(XRT_FEATURE_WINDOW_PEEK                       OFF)
set(XRT_FEATURE_OPENXR_ACTIVE_ACTION_SET_PRIORITY ON)
set(XRT_FEATURE_RENDERDOC                         ${WIVRN_FEATURE_RENDERDOC})

set(XRT_FEATURE_OPENXR_BODY_TRACKING_FB          ON)
set(XRT_FEATURE_OPENXR_FACE_TRACKING_ANDROID     ON)
set(XRT_FEATURE_OPENXR_FACE_TRACKING2_FB         ON)
set(XRT_FEATURE_OPENXR_FACIAL_TRACKING_HTC       ON)
set(XRT_FEATURE_OPENXR_HAND_TRACKING_EXT         ON)
set(XRT_FEATURE_OPENXR_INTERACTION_EXT_HAND      ON)
set(XRT_FEATURE_OPENXR_INTERACTION_EXT_PALM_POSE ON)
set(XRT_FEATURE_OPENXR_INTERACTION_BYTEDANCE     ON)
set(XRT_FEATURE_OPENXR_INTERACTION_VIVE_FOCUS3   ON)
set(XRT_FEATURE_OPENXR_INTERACTION_TOUCH_PRO     ON)
set(XRT_FEATURE_OPENXR_INTERACTION_TOUCH_PLUS    ON)
set(XRT_FEATURE_OPENXR_LAYER_COLOR_SCALE_BIAS    ON)
set(XRT_FEATURE_OPENXR_LAYER_DEPTH              OFF)
set(XRT_FEATURE_OPENXR_VARJO_QUAD_VIEWS         OFF)

set(XRT_HAVE_BLUETOOTH               OFF)
set(XRT_HAVE_GST                     OFF)

set(XRT_HAVE_JPEG                    OFF)
if (NOT WIVRN_FEATURE_DEBUG_GUI)
	set(XRT_HAVE_SDL2                OFF)
endif()
set(XRT_HAVE_OPENCV                  OFF)
set(XRT_HAVE_DBUS                    OFF)
set(XRT_HAVE_LIBUDEV                 OFF)
set(XRT_HAVE_ONNXRUNTIME             OFF)

# Monado's WinRT helpers need the C++/WinRT projection headers, which are not part of
# this build's dependencies.
set(XRT_HAVE_WINRT                   OFF)

set(XRT_HAVE_SYSTEMD                 OFF)
set(XRT_INSTALL_SYSTEMD_UNIT_FILES   OFF)

set(BUILD_DOC                        OFF)
set(BUILD_TESTING                    OFF)

# Use the vcpkg cJSON: Monado's vendored copy clashes with it (dllimport definitions
# and a header/source version mismatch) when both are on the include path.
set(XRT_HAVE_SYSTEM_CJSON            ON)

# Drive Monado's tracing from the same switch as WiVRn's.
if(WIVRN_TRACE_MONADO)
	message(STATUS "WIVRN_TRACE_MONADO=ON: building Monado with percetto tracing (percetto must be available)")
	set(XRT_FEATURE_TRACING ON)
	set(XRT_HAVE_PERCETTO   ON)
endif()

set(XRT_IPC_MSG_SOCK_FILENAME     "wivrn/comp_ipc" CACHE STRING "Service socket filename")
set(XRT_IPC_SERVICE_PID_FILENAME  "wivrn.pid"      CACHE STRING "Service pidfile filename")
set(XRT_OXR_RUNTIME_SUFFIX        "wivrn"          CACHE STRING "OpenXR client library suffix")

FetchContent_MakeAvailable(monado)

if (WIVRN_BUILD_SERVER)
	add_executable(wivrn-server
			accept_connection.cpp
			active_runtime.cpp
			hostname.cpp
			main.cpp
			main_platform.h
			main_platform_windows.cpp
			sleep_inhibitor.cpp
			start_application.cpp
			ipc_server_cb.cpp
			target_instance_wivrn.cpp
			wivrn_ipc.cpp

			audio/audio_setup.cpp
			audio/audio_wasapi.cpp

			compositor/compositor.cpp
			compositor/layer_squasher.cpp
			compositor/foveation.cpp
			compositor/pacer.cpp

			encoder/encoder_settings.cpp
			encoder/idr_handler.cpp
			encoder/video_encoder.cpp
			encoder/video_encoder_raw.cpp

			driver/app_pacer.cpp
			driver/clock_offset.cpp
			driver/configuration.cpp
			driver/hand_joints_list.cpp
			driver/pose_list.cpp
			driver/tracking_control.cpp
			driver/view_list.cpp
			driver/wivrn_hmd.cpp
			driver/wivrn_controller.cpp
			driver/wivrn_eye_tracker.cpp
			driver/wivrn_gamepad.cpp
			driver/wivrn_android_face_tracker.cpp
			driver/wivrn_body_tracker.cpp
			driver/wivrn_fb_face2_tracker.cpp
			driver/wivrn_htc_face_tracker.cpp
			driver/wivrn_generic_tracker.cpp
			driver/wivrn_uinput.cpp
			driver/wivrn_session.cpp
			driver/wivrn_connection.cpp
			driver/xrt_cast.cpp

			utils/wivrn_vk_bundle.cpp
			utils/wivrn_trace.cpp
			utils/gpu_timestamp_pool.cpp
		)
	target_compile_features(wivrn-server PRIVATE cxx_std_20)
	target_compile_definitions(wivrn-server PRIVATE VULKAN_HPP_NO_CONSTRUCTORS)
	target_compile_definitions(wivrn-server PRIVATE UNICODE _UNICODE)

	target_link_libraries(wivrn-server PRIVATE dnsapi ole32 uuid)

	target_include_directories(wivrn-server SYSTEM PRIVATE ${monado_SOURCE_DIR}/src/xrt/compositor/)
	target_include_directories(wivrn-server PRIVATE .)

	target_link_libraries(wivrn-server PRIVATE CLI11::CLI11 xrt-external-renderdoc)

	if (WIVRN_FEATURE_STEAMVR_LIGHTHOUSE)
		target_include_directories(wivrn-server SYSTEM PRIVATE ${monado_SOURCE_DIR}/src/xrt/drivers/steamvr_lh/)
		target_link_libraries(wivrn-server PRIVATE drv_steamvr_lh)
	endif()

	if (WIVRN_FEATURE_SOLARXR)
		target_include_directories(wivrn-server SYSTEM PRIVATE ${monado_SOURCE_DIR}/src/xrt/drivers/solarxr/)
		target_link_libraries(wivrn-server PRIVATE drv_solarxr)
	endif()

	if(WIVRN_USE_NVENC)
		target_sources(
			wivrn-server
			PRIVATE encoder/video_encoder_nvenc.cpp
				encoder/video_encoder_nvenc_shared_state.cpp
			)
	endif()

	if(WIVRN_USE_VULKAN_ENCODE)
		target_sources(wivrn-server PRIVATE
			encoder/video_encoder_vulkan.cpp
			encoder/video_encoder_vulkan_h264.cpp
			encoder/video_encoder_vulkan_h265.cpp
			)
	endif()

	if(WIVRN_USE_X264)
		target_sources(wivrn-server PRIVATE encoder/video_encoder_x264.cpp)
		target_link_libraries(wivrn-server PRIVATE x264::x264)
	endif()

	if(WIVRN_USE_PERFETTO)
		set(PERFETTO_SDK_DIR "" CACHE PATH "Directory containing the Perfetto amalgamated SDK (perfetto.h)")
		if(NOT PERFETTO_SDK_DIR)
			message(FATAL_ERROR "WIVRN_USE_PERFETTO=ON but PERFETTO_SDK_DIR is not set. Download perfetto.h and perfetto.cc from the perfetto repository.")
		endif()
		add_library(wivrn-perfetto-sdk STATIC ${PERFETTO_SDK_DIR}/perfetto.cc)
		target_include_directories(wivrn-perfetto-sdk PUBLIC ${PERFETTO_SDK_DIR})
		if(MSVC)
			target_compile_options(wivrn-perfetto-sdk PRIVATE /w)
		else()
			target_compile_options(wivrn-perfetto-sdk PRIVATE -w)
		endif()
		target_link_libraries(wivrn-perfetto-sdk PUBLIC ws2_32)
		target_link_libraries(wivrn-server PRIVATE wivrn-perfetto-sdk)
		target_compile_definitions(wivrn-server PRIVATE WIVRN_USE_PERFETTO)
		if(WIVRN_TRACE_MONADO)
			target_compile_definitions(wivrn-server PRIVATE WIVRN_TRACE_MONADO)
		endif()
	endif()

	target_link_libraries(
		wivrn-server
		PRIVATE
			aux_os
			aux_render
			aux_util
			aux_vk
			base
			comp_multi
			comp_util
			ipc_server
			target_helpers
			xrt-external-openxr
			xrt-interfaces

			Eigen3::Eigen
			wivrn-common
			wivrn-common-server
			wivrn-external
			nlohmann_json::nlohmann_json
		)

	include(CompileGLSL)
	FetchContent_GetProperties(monado SOURCE_DIR _monado_src)

	wivrn_compile_glsl(wivrn-server
		TARGET_ENV vulkan1.3
		SHADERS
			"${_monado_src}/src/xrt/auxiliary/render/shaders/layer.comp"
			"${CMAKE_CURRENT_SOURCE_DIR}/compositor/shaders/foveation.comp"
		)

	target_compile_definitions(wivrn-server PRIVATE JSON_DIAGNOSTICS=1)

	install(TARGETS wivrn-server RUNTIME DESTINATION bin)
endif()

if (WIVRN_BUILD_SERVER_LIBRARY)
	set_target_properties(${RUNTIME_TARGET} PROPERTIES EXCLUDE_FROM_ALL FALSE)

	# Monado builds libmonado only on Linux (it is gated on XRT_HAVE_LINUX), so the
	# management library and its manifest entry only exist in a Linux build.
	set(_manifest_monado_field "")
	if(TARGET monado)
		set_target_properties(monado PROPERTIES EXCLUDE_FROM_ALL FALSE)
		set_target_properties(monado PROPERTIES OUTPUT_NAME "monado_wivrn")
		# Workaround: the manifest generator refers to the target by name.
		add_library(monado_wivrn ALIAS monado)
		set(_manifest_monado_field ", \"MND_libmonado_path\": \"./$<TARGET_FILE_NAME:monado>\"")
	endif()

	# The OpenXR loader looks the runtime up through the manifest, which must sit
	# next to the libraries it names.
	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dist/openxr_wivrn.json.in" _manifest_template)
	string(REPLACE "@library@" "./$<TARGET_FILE_NAME:${RUNTIME_TARGET}>" _manifest "${_manifest_template}")
	string(REPLACE "@monado_field@" "${_manifest_monado_field}" _manifest "${_manifest}")
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/openxr_wivrn.json" CONTENT "${_manifest}")

	# The OpenXR runtime is a MODULE library on Windows, which installs through
	# LIBRARY rather than RUNTIME.
	install(TARGETS ${RUNTIME_TARGET}
		RUNTIME DESTINATION bin
		LIBRARY DESTINATION bin
		COMPONENT Runtime)
	if(TARGET monado)
		install(TARGETS monado
			RUNTIME DESTINATION bin
			LIBRARY DESTINATION bin
			COMPONENT Runtime)
	endif()
	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openxr_wivrn.json" DESTINATION bin)
endif()
