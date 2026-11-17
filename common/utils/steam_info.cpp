/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "steam_info.h"
#include "vdf.h"
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>
#include <string_view>
#include <windows.h>

namespace
{

class binary_reader
{
	std::ifstream in;

public:
	binary_reader(std::ifstream && in) : in{std::move(in)} {}

	void discard(size_t bytes)
	{
		in.seekg(bytes, std::ios_base::cur);
	}

	template <typename T>
	T read();

	template <typename T>
	        requires std::is_integral_v<T>
	T read()
	{
		T res;
		in.read((char *)&res, sizeof(T));
		if (not in)
			throw std::runtime_error{"File truncated"};

		static_assert(std::endian::native == std::endian::big or std::endian::native == std::endian::little);

		if constexpr (std::endian::native == std::endian::big)
			return std::byteswap(res);
		else
			return res;
	}

	std::vector<std::string> string_table();

	auto tellg()
	{
		return in.tellg();
	}
	void seekg(size_t pos)
	{
		in.seekg(pos);
	}
};

template <>
std::string binary_reader::read()
{
	std::string res;
	std::getline(in, res, '\0');
	if (not in)
		throw std::runtime_error{"File truncated"};
	return res;
}

std::vector<std::string> binary_reader::string_table()
{
	std::vector<std::string> res;
	size_t string_offset = read<uint64_t>();
	auto pos = in.tellg();

	in.seekg(string_offset);
	auto count = read<uint32_t>();
	res.reserve(count);
	for (size_t i = 0; i < count; ++i)
		res.emplace_back(read<std::string>());
	in.seekg(pos);
	return res;
}

template <typename T>
void read_dict(binary_reader & in, T & visitor, const std::vector<std::string> * string_table = nullptr)
{
	while (true)
	{
		auto type = in.read<uint8_t>();
		if (type == 8)
		{
			visitor.end_dict();
			return;
		}

		// Key
		const std::string & key = string_table ? string_table->at(in.read<uint32_t>()) : in.read<std::string>();

		// Value
		switch (type)
		{
			case 0: // Dictionary
				visitor.begin_dict(key);
				read_dict(in, visitor, string_table);
				break;
			case 1: // UTF-8 string
				visitor.on_string(key, in.read<std::string>());
				break;
			case 2: // uint32_t
				visitor.on_uint32_t(key, in.read<uint32_t>());
				break;
				// case 3: // float
			default:
				throw std::runtime_error{"Unknown object type " + std::to_string(type)};
		}
	}
}

bool iequals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;

	for (size_t i = 0; i < a.size(); ++i)
	{
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return true;
}

struct registry_location
{
	HKEY root;
	const char * subkey;
	const char * name;
};

std::filesystem::path steam_path_from_registry()
{
	for (const auto & location: {
	             registry_location{HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath"},
	             registry_location{HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath"},
	             registry_location{HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath"},
	     })
	{
		char buffer[MAX_PATH * 2]{};
		DWORD size = sizeof(buffer);
		if (RegGetValueA(location.root, location.subkey, location.name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
			continue;
		if (buffer[0] == '\0')
			continue;
		return std::filesystem::path(buffer);
	}
	return {};
}

/// Library folders declared in steamapps/libraryfolders.vdf, primary install included.
std::vector<std::filesystem::path> steam_library_folders(const std::filesystem::path & root)
{
	std::vector<std::filesystem::path> result;
	if (root.empty())
		return result;

	std::ifstream file(root / "steamapps" / "libraryfolders.vdf");
	if (not file)
		return result;

	std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

	static const std::regex path_entry(R"re("path"[ 	]*"((?:[^"\]|\.)*)")re");
	for (auto it = std::sregex_iterator(content.begin(), content.end(), path_entry); it != std::sregex_iterator(); ++it)
	{
		std::string value = (*it)[1].str();
		std::string unescaped;
		unescaped.reserve(value.size());
		for (size_t i = 0; i < value.size(); ++i)
		{
			if (value[i] == '\\' and i + 1 < value.size())
				unescaped += value[++i];
			else
				unescaped += value[i];
		}

		auto path = std::filesystem::path(unescaped);
		if (not path.empty())
			result.push_back(std::move(path));
	}

	return result;
}

std::optional<uint32_t> guess_steam_userid(const std::filesystem::path & root)
{
	auto localconfig = root / "config/loginusers.vdf";
	if (not std::filesystem::exists(localconfig))
		return {};

	try
	{
		wivrn::vdf::root loginusers(localconfig);
		if (loginusers.key != "users")
			return {};

		std::optional<uint32_t> res;
		for (const auto & [userid, entries]: std::get<std::vector<wivrn::vdf::keyvalue>>(loginusers.value))
		{
			uint64_t v;
			// MSVC's string_view iterators are not plain pointers, which from_chars needs.
			if (std::from_chars(userid.data.data(), userid.data.data() + userid.data.size(), v).ec != std::errc{})
				continue;
			res = v; // truncate to 32 bits
			for (const auto & [key, value]: std::get<std::vector<wivrn::vdf::keyvalue>>(entries))
			{
				if (key == "MostRecent" and std::get<wivrn::vdf::string>(value) == "1")
					return res;
			}
		}
		return res;
	}
	catch (std::exception & e)
	{
		return {};
	}
}

} // namespace

void read_steam_icons(std::filesystem::path path, std::unordered_map<uint32_t, wivrn::steam::icon> & icons)
{
	binary_reader in{std::ifstream{path}};

	uint32_t magic = in.read<uint32_t>();
	if ((magic & 0xff'ff'ff'00) != 0x07'56'44'00)
		throw std::runtime_error{"Wrong magic number"};

	auto universe = in.read<uint32_t>();

	int version = magic & 0xff;

	std::vector<std::string> string_table;
	std::vector<std::string> * string_table_ptr = nullptr;
	if (version >= 41)
	{
		string_table = in.string_table();
		string_table_ptr = &string_table;
	}

	struct V
	{
		wivrn::steam::icon current{};

		void begin_dict(const std::string & key)
		{
		}
		void end_dict()
		{
		}
		void on_string(const std::string & key, std::string && val)
		{
			if (iequals(key, "clienticon"))
				current.clienticon = std::move(val);
		}
		void on_uint32_t(const std::string & key, uint32_t val)
		{
		}
	};

	while (true)
	{
		// See https://github.com/SteamDatabase/SteamAppInfo/blob/master/README.md#app-entry-repeated
		auto app_id = in.read<uint32_t>();
		if (app_id == 0)
			break;

		size_t size_data = in.read<uint32_t>();
		size_t pos = in.tellg();
		in.discard(
		        4    // info_state
		        + 4  // last_updated
		        + 8  // pics_token
		        + 20 // SHA1 of the app info
		        + 4  // change number
		        + 20 // SHA1 of the bin data
		        + 4  // checksum
		        + 1  // type
		);
		try
		{
			V v;
			read_dict(in, v, string_table_ptr);
			if (not v.current.clienticon.empty())
				icons.emplace(app_id, std::move(v.current));
		}
		catch (...)
		{
			// Ignore errors
		}
		in.seekg(pos + size_data);
	}
}

void read_steam_shortcuts(
        const std::filesystem::path & path,
        std::vector<wivrn::steam::application> & apps,
        std::unordered_map<uint32_t, std::filesystem::path> & icons)
{
	if (not std::filesystem::exists(path))
		return;
	binary_reader in{std::ifstream{path}};
	auto type = in.read<uint8_t>();
	auto name = in.read<std::string>();

	if (type != 0 or name != "shortcuts")
		throw std::runtime_error{"Invalid type for shortcuts file"};

	std::vector<wivrn::steam_shortcut> res;
	struct V
	{
		std::vector<wivrn::steam::application> & apps;
		std::unordered_map<uint32_t, std::filesystem::path> & icons;

		int depth = 0;
		wivrn::steam::application current{};
		std::filesystem::path icon;
		bool VR = false;

		void begin_dict(const std::string & key)
		{
			++depth;
			if (depth == 1)
			{
				current = decltype(current){};
				icon.clear();
				VR = false;
			}
		}
		void end_dict()
		{
			--depth;
			if (depth == 0)
			{
				if (VR)
				{
					if (not icon.empty())
						icons[current.appid] = std::move(icon);
					// ¯\_(ツ)_/¯
					current.appid = (current.appid << 32) | 0x02000000;
					current.url = std::format("steam://rungameid/{}", current.appid);
					apps.push_back(std::move(current));
				}
			}
		}
		void on_string(const std::string & key, std::string && val)
		{
			if (depth == 1)
			{
				if (iequals(key, "AppName"))
					current.name[""] = std::move(val);
				else if (iequals(key, "icon") and not val.empty())
					icon = std::move(val);
			}
		}
		void on_uint32_t(const std::string & key, uint32_t val)
		{
			if (depth == 1)
			{
				if (iequals(key, "appid"))
					current.appid = val;
				else if (iequals(key, "OpenVR"))
					VR = val;
			}
		}
	} v{apps, icons};

	read_dict(in, v);
}

wivrn::steam::steam(std::filesystem::path root) :
        root(std::move(root)),
        default_userid(guess_steam_userid(this->root))
{
}

std::vector<wivrn::steam> wivrn::steam::find_installations()
{
	std::vector<wivrn::steam> res;

	auto try_construct = [&res](const std::filesystem::path & root) {
		try
		{
			if (not root.empty() and std::filesystem::exists(root))
			{
				res.push_back(wivrn::steam(root));
				return true;
			}
		}
		catch (const std::exception &)
		{
		}
		return false;
	};

	// The official installer records the location in the registry; portable and
	// SteamCMD installs fall back to the default directory.
	std::filesystem::path primary = steam_path_from_registry();
	if (primary.empty())
	{
		if (const char * program_files = std::getenv("ProgramFiles(x86)"); program_files and *program_files)
			primary = std::filesystem::path(program_files) / "Steam";
	}

	try_construct(primary);

	// Additional libraries only hold steamapps/, but they are enumerated so that
	// games installed outside the primary directory are visible.
	for (const auto & library: steam_library_folders(primary))
	{
		if (library != primary)
			try_construct(library);
	}

	return res;
}

std::vector<wivrn::steam::application> wivrn::steam::list_applications()
{
	std::vector<wivrn::steam::application> res;
	// Steam games, from VR manifest
	try
	{
		std::ifstream manifest(root / "config/steamapps.vrmanifest");
		nlohmann::json json = nlohmann::json::parse(manifest);
		for (auto & i: json["applications"])
		{
			try
			{
				// Steam games have an url launch
				// shortcuts may be in this file, or may not...
				if (i["launch_type"] != "url")
					continue;

				std::string app_key = i["app_key"];
				const char prefix[] = "steam.app.";
				if (not app_key.starts_with(prefix))
					continue;

				application app{
				        .appid = stoul(app_key.substr(strlen(prefix))),
				        .url = i["url"],
				};

				for (auto [locale, items]: i["strings"].items())
				{
					if (auto it = items.find("name"); it != items.end())
						app.name[locale] = *it;
				}

				if (not app.name.contains(""))
				{
					auto it = app.name.find("en_us");
					if (it == app.name.end())
						it = app.name.begin();
					if (it != app.name.end())
						app.name[""] = it->second;
					else
						continue; // skip unnamed app
				}
				res.push_back(std::move(app));
			}
			catch (std::exception & e)
			{
				std::cerr << "Failed to parse Steam VR manifest: " << e.what() << std::endl;
			}
		}
	}
	catch (std::exception & e)
	{
	}

	// Shortcuts
	if (default_userid)
	{
		try
		{
			read_steam_shortcuts(root / "userdata" / std::to_string(*default_userid) / "config/shortcuts.vdf", res, shortcut_icons);
		}
		catch (std::exception & e)
		{
		}
	}
	else
	{
		// Is it a good idea to just iterate over all users?
		std::error_code ec;
		for (auto const & entry: std::filesystem::directory_iterator{root / "userdata", ec})
			try
			{
				read_steam_shortcuts(entry.path() / "config/shortcuts.vdf", res, shortcut_icons);
			}
			catch (std::exception & e)
			{
			}
	}

	return res;
}

std::optional<std::filesystem::path> wivrn::steam::get_icon(uint64_t appid)
{
	if (appid & 0x02000000)
	{
		// Shortcut
		auto it = shortcut_icons.find(appid >> 32);
		if (it != shortcut_icons.end())
			return it->second;
		return {};
	}

	if (not icons)
	{
		icons.emplace();
		try
		{
			read_steam_icons(root / "appcache/appinfo.vdf", *icons);
		}
		catch (...)
		{}
	}
	auto it = icons->find(appid);
	if (it == icons->end())
		return {};

	auto icon_path = root / "steam/games" / (it->second.clienticon + ".ico");

	if (std::filesystem::exists(icon_path))
		return icon_path;

	return {};
}

std::string wivrn::steam::get_steam_command() const
{
	auto executable = root / "steam.exe";
	if (std::filesystem::exists(executable))
		return "\"" + executable.string() + "\"";
	return "steam";
}
