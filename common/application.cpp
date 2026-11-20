/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "application.h"

#include "utils/path_utf8.h"
#include "utils/steam_info.h"

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace wivrn
{
namespace
{
std::wstring to_wide(const std::string & value)
{
	if (value.empty())
		return {};

	int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
	if (size <= 0)
		return {};
	std::wstring result(size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), size);
	return result;
}

std::string to_utf8(const std::wstring & value)
{
	if (value.empty())
		return {};

	int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};
	std::string result(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), size, nullptr, nullptr);
	return result;
}

/// Scoped COM initialisation; the shell interfaces used below need an apartment.
class com_scope
{
	bool uninitialize = false;

public:
	com_scope()
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		// RPC_E_CHANGED_MODE means another thread already used another model.
		uninitialize = SUCCEEDED(hr);
	}
	~com_scope()
	{
		if (uninitialize)
			CoUninitialize();
	}

	com_scope(const com_scope &) = delete;
	com_scope & operator=(const com_scope &) = delete;
};

std::vector<std::filesystem::path> start_menu_directories()
{
	std::vector<std::filesystem::path> result;

	for (auto csidl: {CSIDL_COMMON_PROGRAMS, CSIDL_PROGRAMS})
	{
		char buffer[MAX_PATH]{};
		if (SHGetSpecialFolderPathA(nullptr, buffer, csidl, FALSE))
			result.emplace_back(buffer);
	}

	// SHGetSpecialFolderPath already covers these, but a redirected Start Menu
	// is not always reported; add the environment-based locations as well.
	for (const char * variable: {"ProgramData", "APPDATA"})
	{
		if (const char * value = std::getenv(variable); value and *value)
		{
			auto path = std::filesystem::path(value) / "Microsoft" / "Windows" / "Start Menu" / "Programs";
			if (std::filesystem::is_directory(path))
				result.push_back(std::move(path));
		}
	}

	return result;
}

struct shortcut
{
	std::filesystem::path target;
	std::string arguments;
	std::string working_directory;
};

std::optional<shortcut> read_shortcut(const std::filesystem::path & path)
{
	com_scope com;

	IShellLinkW * link = nullptr;
	if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
		return {};

	std::optional<shortcut> result;

	IPersistFile * persist = nullptr;
	if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist))))
	{
		auto wide_path = path.wstring();
		if (SUCCEEDED(persist->Load(wide_path.c_str(), STGM_READ)))
		{
			wchar_t buffer[MAX_PATH * 2];
			WIN32_FIND_DATAW data{};

			// GetPath fails for URL and shell shortcuts, which are not launched
			// as processes by the server.
			if (SUCCEEDED(link->GetPath(buffer, (int)std::size(buffer), &data, SLGP_RAWPATH)) and buffer[0] != L'\0')
			{
				shortcut entry;
				entry.target = std::filesystem::path(buffer);

				if (SUCCEEDED(link->GetArguments(buffer, (int)std::size(buffer))))
					entry.arguments = to_utf8(buffer);
				if (SUCCEEDED(link->GetWorkingDirectory(buffer, (int)std::size(buffer))))
					entry.working_directory = to_utf8(buffer);

				result = std::move(entry);
			}
		}
		persist->Release();
	}

	link->Release();
	return result;
}

bool is_uninstaller(const std::filesystem::path & target)
{
	auto name = path_to_utf8(target.filename());
	return name.starts_with("unins") or name.starts_with("Uninstall") or name.starts_with("uninstall");
}
} // namespace

std::unordered_map<std::string, application> list_applications()
{
	std::unordered_map<std::string, application> res;

	for (auto & steam: steam::find_installations())
	{
		auto cmd = steam.get_steam_command() + " ";
		for (auto && app: steam.list_applications())
		{
			res.emplace(std::to_string(app.appid),
			            application{
			                    .name = std::move(app.name),
			                    .exec = cmd + app.url,
			                    .icon_path = steam.get_icon(app.appid),
			            });
		}
	}

	for (const auto & directory: start_menu_directories())
	{
		std::error_code ec;
		if (not std::filesystem::is_directory(directory, ec))
			continue;

		for (std::filesystem::recursive_directory_iterator entry(directory,
		                                                          std::filesystem::directory_options::skip_permission_denied,
		                                                          ec),
		     end;
		     entry != end;
		     entry.increment(ec))
		{
			if (ec)
				break;

			if (not entry->is_regular_file(ec))
				continue;
			if (entry->path().extension() != ".lnk")
				continue;

			auto shortcut = read_shortcut(entry->path());
			if (not shortcut)
				continue;
			if (shortcut->target.extension() != ".exe")
				continue;
			if (is_uninstaller(shortcut->target))
				continue;

			auto name = path_to_utf8(entry->path().stem());
			if (name.empty())
				continue;

			application app;
			app.name[""] = std::move(name);
			app.exec = "\"" + path_to_utf8(shortcut->target) + "\"";
			if (not shortcut->arguments.empty())
				app.exec += " " + shortcut->arguments;

			if (not shortcut->working_directory.empty())
				app.path = shortcut->working_directory;
			else
				app.path = path_to_utf8(shortcut->target.parent_path());

			res.emplace("shortcut:" + path_to_utf8(entry->path()), std::move(app));
		}
	}

	return res;
}

} // namespace wivrn
