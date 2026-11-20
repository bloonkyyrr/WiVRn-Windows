/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "platform.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wivrn
{

/// State of the OpenXR/OpenVR runtime selection, for the desktop UI.
struct runtime_info
{
	bool manifest_found = false;
	std::filesystem::path manifest;
	/// Current HKLM ActiveRuntime value, if any.
	std::optional<std::string> active_runtime;
	/// The runtime WiVRn replaced, restored when the session ends.
	std::optional<std::string> previous_runtime;
	bool wivrn_active = false;
	/// Whether the system-wide value can be written (administrator rights).
	bool can_set_active = false;
	std::filesystem::path openvr_compat;
	bool openvr_configured = false;
};

/// Makes WiVRn the active OpenXR (and OpenVR) runtime while a session runs.
///
/// On Windows the OpenXR loader resolves the runtime from
/// HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime, and honours the
/// XR_RUNTIME_JSON environment variable before that. The registry needs
/// administrator rights, so the variable is always set (it is inherited by the
/// applications the server launches) and the registry is updated opportunistically.
/// The value that was active before WiVRn is saved and restored when the session
/// ends, including after a crash.
class active_runtime
{
	std::vector<std::filesystem::path> active_runtime_json;
	std::filesystem::path openvr_manifest;
	platform::process_id pid;

	bool registry_updated = false;
	std::optional<std::string> previous_active_runtime;
	std::optional<std::string> previous_xr_runtime_json;

public:
	active_runtime();
	active_runtime(const active_runtime &) = delete;
	active_runtime & operator=(const active_runtime &) = delete;
	~active_runtime();

	static std::vector<std::filesystem::path> manifest_path();
	static std::filesystem::path openvr_compat_path();

	/// Inspect the current runtime selection. Safe to call at any time.
	static runtime_info query();

	/// Make WiVRn the system-wide OpenXR runtime and keep the previous one for
	/// restoring later. Returns false and fills \p error on failure (usually a
	/// missing administrator rights).
	static bool set_active(std::string & error);

	/// Restore the runtime that was active before WiVRn, or remove the value if
	/// there was none. Returns false and fills \p error on failure.
	static bool restore_previous(std::string & error);

	static void cleanup_openxr();
};
} // namespace wivrn
