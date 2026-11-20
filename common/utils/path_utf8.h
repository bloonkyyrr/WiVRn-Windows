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

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wivrn
{

/// Converts a path to UTF-8.
///
/// std::filesystem::path::string() converts to the ANSI code page on Windows and
/// throws std::system_error(ERROR_NO_UNICODE_TRANSLATION), reported as "No
/// mapping for the Unicode character exists in the target multi-byte code page",
/// for any name that code page cannot represent: an emoji, Cyrillic or CJK
/// character in a Start Menu entry, a Steam library or a shortcut. Use this
/// instead wherever a path becomes a string, so one exotic file name cannot take
/// the server down while it lists the applications.
inline std::string path_to_utf8(const std::filesystem::path & path)
{
#ifdef _WIN32
	const auto & native = path.native();
	if (native.empty())
		return {};

	int size = WideCharToMultiByte(CP_UTF8, 0, native.data(), static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};

	std::string result(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, native.data(), static_cast<int>(native.size()), result.data(), size, nullptr, nullptr);
	return result;
#else
	auto utf8 = path.u8string();
	return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
#endif
}

} // namespace wivrn
