/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
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

#ifdef _WIN32

#include <windows.h>

#include <shellapi.h>

#include <string>
#include <vector>

namespace wivrn
{

/// Splits a command line using the Windows rules (CommandLineToArgvW).
///
/// The POSIX-style unescaping used on Linux treats a backslash as an escape
/// character, which turns "C:\Program Files (x86)\Steam\steam.exe" into
/// "C:Program Files (x86)Steamsteam.exe". Windows only gives a backslash a
/// special meaning when it precedes a quote, and CommandLineToArgvW implements
/// exactly that, so the arguments round-trip through spawn_process unchanged.
inline std::vector<std::string> split_windows_command_line(const std::string & command_line)
{
	if (command_line.empty())
		return {};

	int wide_size = MultiByteToWideChar(CP_UTF8, 0, command_line.data(), static_cast<int>(command_line.size()), nullptr, 0);
	if (wide_size <= 0)
		return {};

	std::wstring wide(wide_size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, command_line.data(), static_cast<int>(command_line.size()), wide.data(), wide_size);

	int argc = 0;
	LPWSTR * argv = CommandLineToArgvW(wide.c_str(), &argc);
	if (argv == nullptr)
		return {};

	std::vector<std::string> result;
	result.reserve(argc);
	for (int i = 0; i < argc; ++i)
	{
		int utf8_size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
		if (utf8_size <= 1)
		{
			result.emplace_back();
			continue;
		}

		std::string argument(utf8_size - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, argument.data(), utf8_size, nullptr, nullptr);
		result.push_back(std::move(argument));
	}

	LocalFree(argv);
	return result;
}

} // namespace wivrn

#endif
