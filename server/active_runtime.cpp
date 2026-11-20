/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "active_runtime.h"

#include "driver/configuration.h"
#include "platform.h"
#include "wivrn_config.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace
{
constexpr const char * openxr_registry_key = "SOFTWARE\\Khronos\\OpenXR\\1";
constexpr const char * openxr_registry_value = "ActiveRuntime";

std::filesystem::path executable_path()
{
	std::vector<char> buffer(MAX_PATH);
	while (true)
	{
		DWORD length = GetModuleFileNameA(nullptr, buffer.data(), (DWORD)buffer.size());
		if (length == 0)
			return {};
		if (length < buffer.size())
			return std::filesystem::path(std::string(buffer.data(), length));
		buffer.resize(buffer.size() * 2);
	}
}

std::filesystem::path executable_directory()
{
	return executable_path().parent_path();
}

std::filesystem::path program_data_directory()
{
	if (const char * appdata = std::getenv("APPDATA"); appdata and *appdata)
		return appdata;
	return wivrn::platform::config_directory().parent_path();
}

std::filesystem::path local_appdata_directory()
{
	if (const char * local = std::getenv("LOCALAPPDATA"); local and *local)
		return local;
	return wivrn::platform::cache_directory().parent_path();
}

std::filesystem::path openvr_manifest_path()
{
	return local_appdata_directory() / "openvr" / "openvrpaths.vrpath";
}

std::filesystem::path backup_name(std::filesystem::path file)
{
	file += ".wivrn-backup";
	return file;
}

void move_file(const std::filesystem::path & from, const std::filesystem::path & to)
{
	std::error_code ec;
	auto from_status = std::filesystem::symlink_status(from, ec);
	if (ec or from_status.type() == std::filesystem::file_type::not_found)
		return;
	std::filesystem::remove(to, ec);
	std::filesystem::rename(from, to, ec);
	if (ec)
		throw std::system_error(ec, "rename " + from.string());
}

/// Locates the OpenVR client DLL inside a compatibility layer directory.
std::filesystem::path openvr_client_dll(const std::filesystem::path & compat_root)
{
	for (const auto & candidate: {compat_root / "bin" / "win64" / "vrclient_x64.dll",
	                              compat_root / "vrclient_x64.dll",
	                              compat_root / "bin" / "win32" / "vrclient.dll"})
	{
		if (std::filesystem::exists(candidate))
			return candidate;
	}
	return {};
}

/// Registry value that was active before WiVRn took over. It is kept on disk so
/// that the user's runtime is restored even if the server crashed.
struct runtime_backup
{
	std::string manifest;
	std::optional<std::string> previous;
};

std::filesystem::path backup_path()
{
	return wivrn::platform::config_directory() / "active_runtime_backup.json";
}

std::optional<runtime_backup> read_backup()
{
	try
	{
		std::ifstream file(backup_path());
		if (not file)
			return std::nullopt;

		nlohmann::json json;
		file >> json;

		runtime_backup backup;
		backup.manifest = json.value("manifest", std::string{});
		if (json.contains("previous") and json["previous"].is_string())
			backup.previous = json["previous"].get<std::string>();
		return backup;
	}
	catch (std::exception & e)
	{
		return std::nullopt;
	}
}

void write_backup(const std::filesystem::path & manifest, const std::optional<std::string> & previous)
{
	try
	{
		nlohmann::json json;
		json["manifest"] = manifest.string();
		json["previous"] = previous ? nlohmann::json(*previous) : nlohmann::json(nullptr);

		std::error_code ec;
		std::filesystem::create_directories(backup_path().parent_path(), ec);

		std::ofstream file(backup_path());
		file.exceptions(std::ofstream::failbit);
		file << json;
	}
	catch (std::exception & e)
	{
		std::cerr << "Cannot save the previous OpenXR runtime: " << e.what() << std::endl;
	}
}

void remove_backup()
{
	std::error_code ec;
	std::filesystem::remove(backup_path(), ec);
}

std::optional<std::string> read_active_runtime()
{
	HKEY key = nullptr;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, openxr_registry_key, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return std::nullopt;

	std::optional<std::string> value;
	DWORD type = 0;
	DWORD size = 0;
	if (RegQueryValueExA(key, openxr_registry_value, nullptr, &type, nullptr, &size) == ERROR_SUCCESS and
	    type == REG_SZ and size > 0)
	{
		std::string buffer(size, '\0');
		if (RegQueryValueExA(key, openxr_registry_value, nullptr, &type, reinterpret_cast<BYTE *>(buffer.data()), &size) == ERROR_SUCCESS)
		{
			while (not buffer.empty() and buffer.back() == '\0')
				buffer.pop_back();
			if (not buffer.empty())
				value = buffer;
		}
	}

	RegCloseKey(key);
	return value;
}

bool write_active_runtime(const std::string & value, std::string & error)
{
	HKEY key = nullptr;
	LSTATUS status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, openxr_registry_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	if (status != ERROR_SUCCESS or key == nullptr)
	{
		error = "cannot open HKLM\\" + std::string(openxr_registry_key) +
		        " for writing (administrator rights required), error " + std::to_string(status);
		return false;
	}

	status = RegSetValueExA(key,
	                        openxr_registry_value,
	                        0,
	                        REG_SZ,
	                        reinterpret_cast<const BYTE *>(value.c_str()),
	                        (DWORD)value.size() + 1);
	RegCloseKey(key);

	if (status != ERROR_SUCCESS)
	{
		error = "cannot write the OpenXR active runtime registry value, error " + std::to_string(status);
		return false;
	}

	return true;
}

bool delete_active_runtime(std::string & error)
{
	HKEY key = nullptr;
	LSTATUS status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, openxr_registry_key, 0, KEY_SET_VALUE, &key);
	if (status != ERROR_SUCCESS or key == nullptr)
	{
		error = "cannot open HKLM\\" + std::string(openxr_registry_key) +
		        " for writing (administrator rights required), error " + std::to_string(status);
		return false;
	}

	status = RegDeleteValueA(key, openxr_registry_value);
	RegCloseKey(key);

	if (status != ERROR_SUCCESS and status != ERROR_FILE_NOT_FOUND)
	{
		error = "cannot restore the OpenXR active runtime registry value, error " + std::to_string(status);
		return false;
	}

	return true;
}

bool restore_active_runtime_value(const std::optional<std::string> & previous, std::string & error)
{
	if (previous)
		return write_active_runtime(*previous, error);
	return delete_active_runtime(error);
}
} // namespace

namespace wivrn
{

std::vector<std::filesystem::path> active_runtime::manifest_path()
{
	auto bin = executable_directory();

	// Running from the build tree: CMake writes the manifest next to the server.
	for (const auto & candidate: {bin / "openxr_wivrn-dev.json", bin / "openxr_wivrn.json"})
	{
		if (std::filesystem::exists(candidate))
			return {candidate};
	}

	// Installed layout (share/wivrn/openxr or share/openxr/1 next to the install prefix).
	for (const auto & candidate: {std::filesystem::path(WIVRN_INSTALL_PREFIX) / "share" / "openxr" / "1" / "openxr_wivrn.json",
	                              std::filesystem::path(WIVRN_INSTALL_PREFIX) / "share" / "wivrn" / "openxr_wivrn.json"})
	{
		if (std::filesystem::exists(candidate))
			return {candidate};
	}

	return {};
}

std::filesystem::path active_runtime::openvr_compat_path()
{
	return std::visit(
	        [](auto && value) -> std::filesystem::path {
		        using T = std::decay_t<decltype(value)>;

		        if constexpr (std::is_same_v<T, std::monostate>)
		        {
			        // No user configuration: probe the configured search path.
			        for (auto path: std::ranges::split_view(std::string_view(OVR_COMPAT_SEARCH_PATH), std::string_view(";")))
			        {
				        std::filesystem::path candidate{std::string(path.begin(), path.end())};
				        if (candidate.empty())
					        continue;
				        if (not openvr_client_dll(candidate).empty())
					        return candidate;
			        }
			        return {};
		        }
		        else if constexpr (std::is_same_v<T, std::string>)
		        {
			        return std::filesystem::path(value);
		        }
		        else
		        {
			        // Explicit null: do not set any compatibility layer.
			        return {};
		        }
	        },
	        configuration().openvr_compat_path);
}

active_runtime::active_runtime() :
        pid(platform::current_process_id())
{
	auto manifests = manifest_path();
	if (manifests.empty())
	{
		std::cerr << "Cannot find the WiVRn OpenXR runtime manifest, leaving the active runtime unchanged" << std::endl;
	}
	for (const auto & manifest: manifests)
	{
		active_runtime_json.push_back(manifest);

		// Per-process runtime selection. The variable is inherited by every
		// application the server launches, so this works without elevation.
		if (const char * previous = std::getenv("XR_RUNTIME_JSON"); previous and *previous)
		{
			if (not previous_xr_runtime_json)
				previous_xr_runtime_json = previous;
		}
		platform::set_environment_variable("XR_RUNTIME_JSON", manifest.string().c_str());

		// System-wide selection: HKLM is writable by administrators only. The value
		// that was active before is kept so it can be restored later.
		previous_active_runtime = read_active_runtime();
		std::string error;
		if (write_active_runtime(manifest.string(), error))
		{
			registry_updated = true;
			if (not read_backup() and (not previous_active_runtime or *previous_active_runtime != manifest.string()))
				write_backup(manifest, previous_active_runtime);
		}
		else
		{
			std::cerr << error << "; applications launched by WiVRn still use XR_RUNTIME_JSON" << std::endl;
		}
		break;
	}

	try
	{
		auto ovr_compat = openvr_compat_path();
		if (ovr_compat.empty())
		{
			std::cerr << "Not setting an OpenVR compatibility library" << std::endl;
			return;
		}

		std::cerr << "Setting the OpenVR compatibility library to " << ovr_compat.string() << std::endl;
		if (openvr_client_dll(ovr_compat).empty())
		{
			std::cerr << "Unable to find vrclient_x64.dll under " << ovr_compat.string()
			          << ", ensure your OpenVR compatibility library is set correctly" << std::endl;
		}

		openvr_manifest = openvr_manifest_path();
		std::filesystem::create_directories(openvr_manifest.parent_path());
		move_file(openvr_manifest, backup_name(openvr_manifest));

		nlohmann::json manifest;
		manifest["runtime"] = {ovr_compat.string()};
		manifest["version"] = 1;
		std::ofstream manifest_file(openvr_manifest);
		manifest_file.exceptions(std::ofstream::failbit);
		manifest_file << manifest;
		manifest_file.close();
	}
	catch (std::exception & e)
	{
		std::cerr << "Cannot set active OpenVR runtime: " << e.what() << std::endl;
		openvr_manifest.clear();
	}
}

active_runtime::~active_runtime()
{
	if (pid != platform::current_process_id())
		return;

	if (registry_updated)
	{
		// Restore the runtime the user had, which is in the backup when one was
		// written (it also covers a value changed from the UI).
		std::optional<std::string> previous = previous_active_runtime;
		if (not active_runtime_json.empty())
		{
			if (auto backup = read_backup(); backup and backup->manifest == active_runtime_json.front().string())
				previous = backup->previous;
		}

		std::string error;
		if (not restore_active_runtime_value(previous, error))
			std::cerr << error << std::endl;

		remove_backup();
		registry_updated = false;
	}

	if (previous_xr_runtime_json)
		platform::set_environment_variable("XR_RUNTIME_JSON", previous_xr_runtime_json->c_str());
	else
		platform::set_environment_variable("XR_RUNTIME_JSON", "");

	try
	{
		if (not openvr_manifest.empty())
		{
			std::error_code ec;
			std::filesystem::remove(openvr_manifest, ec);
			move_file(backup_name(openvr_manifest), openvr_manifest);
			openvr_manifest.clear();
		}
	}
	catch (std::exception & e)
	{
		std::cerr << "Cannot unset active OpenVR runtime: " << e.what() << std::endl;
	}
}

void active_runtime::cleanup_openxr()
{
	// Restore an OpenXR value left behind by an unclean shutdown.
	if (auto backup = read_backup())
	{
		auto current = read_active_runtime();
		if (current and *current == backup->manifest)
		{
			std::string error;
			if (restore_active_runtime_value(backup->previous, error))
				std::cerr << "Restored the OpenXR runtime that was active before WiVRn" << std::endl;
			else
				std::cerr << "Cannot restore the previous OpenXR runtime: " << error << std::endl;
		}
		remove_backup();
	}

	// Restore an OpenVR manifest backup left behind by an unclean shutdown.
	auto manifest = openvr_manifest_path();
	if (std::filesystem::exists(backup_name(manifest)))
	{
		std::cerr << "Restoring " << manifest.string() << " from backup" << std::endl;
		std::error_code ec;
		std::filesystem::remove(manifest, ec);
		std::filesystem::rename(backup_name(manifest), manifest, ec);
		if (ec)
			std::cerr << "Failed to restore " << manifest.string() << ": " << ec.message() << std::endl;
	}
}

runtime_info active_runtime::query()
{
	runtime_info info;

	auto manifests = manifest_path();
	if (not manifests.empty())
	{
		info.manifest = manifests.front();
		info.manifest_found = true;
	}

	info.active_runtime = read_active_runtime();
	if (auto backup = read_backup())
		info.previous_runtime = backup->previous;

	info.wivrn_active = info.manifest_found and info.active_runtime and *info.active_runtime == info.manifest.string();

	HKEY key = nullptr;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, openxr_registry_key, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
	{
		info.can_set_active = true;
		RegCloseKey(key);
	}

	info.openvr_compat = openvr_compat_path();
	info.openvr_configured = not info.openvr_compat.empty() and not openvr_client_dll(info.openvr_compat).empty();

	return info;
}

bool active_runtime::set_active(std::string & error)
{
	auto manifests = manifest_path();
	if (manifests.empty())
	{
		error = "the WiVRn OpenXR runtime manifest was not found next to the server";
		return false;
	}

	const auto & manifest = manifests.front();

	auto current = read_active_runtime();
	if (not current or *current != manifest.string())
	{
		// Remember the value that is being replaced, unless an earlier session
		// already saved it.
		if (not read_backup())
			write_backup(manifest, current);
	}

	platform::set_environment_variable("XR_RUNTIME_JSON", manifest.string().c_str());

	if (not write_active_runtime(manifest.string(), error))
	{
		error += "; applications launched by WiVRn still use XR_RUNTIME_JSON";
		return false;
	}

	return true;
}

bool active_runtime::restore_previous(std::string & error)
{
	auto backup = read_backup();
	if (not backup)
	{
		error = "there is no saved OpenXR runtime to restore";
		return false;
	}

	if (not restore_active_runtime_value(backup->previous, error))
		return false;

	if (backup->previous)
		platform::set_environment_variable("XR_RUNTIME_JSON", backup->previous->c_str());
	else
		platform::set_environment_variable("XR_RUNTIME_JSON", "");

	remove_backup();
	return true;
}

} // namespace wivrn
