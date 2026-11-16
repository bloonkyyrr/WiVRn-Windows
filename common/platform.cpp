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

#include "platform.h"

#include <cstdlib>
#include <cstring>

namespace wivrn::platform
{
int last_socket_error()
{
#if defined(_WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

std::error_code socket_error_code(int error)
{
	return {error, std::system_category()};
}

void throw_socket_error(const char * what)
{
	throw std::system_error(last_socket_error_code(), what);
}

std::string error_message(int error)
{
#if defined(_WIN32)
	char * buffer = nullptr;
	DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                              nullptr,
	                              (DWORD)error,
	                              0,
	                              (LPSTR)&buffer,
	                              0,
	                              nullptr);
	std::string message = length ? std::string(buffer, length) : ("error " + std::to_string(error));
	if (buffer)
		LocalFree(buffer);
	return message;
#else
	return std::strerror(error);
#endif
}

int close_socket(int fd)
{
#if defined(_WIN32)
	return closesocket((SOCKET)(uintptr_t)fd);
#else
	return ::close(fd);
#endif
}

int poll_sockets(pollfd * fds, unsigned long count, int timeout_ms)
{
#if defined(_WIN32)
	return WSAPoll(fds, count, timeout_ms);
#else
	return ::poll(fds, count, timeout_ms);
#endif
}

void set_close_on_exec(int fd)
{
#if !defined(_WIN32)
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#else
	(void)fd; // Windows sockets are not inherited unless explicitly requested
#endif
}

bool set_nonblocking(int fd, bool non_blocking)
{
#if defined(_WIN32)
	u_long mode = non_blocking ? 1 : 0;
	return ioctlsocket((SOCKET)(uintptr_t)fd, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return false;
	flags = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return fcntl(fd, F_SETFL, flags) == 0;
#endif
}

int shutdown_socket(int fd)
{
	return ::shutdown(fd, SHUT_RDWR);
}

void initialize_networking()
{
#if defined(_WIN32)
	static const bool initialized = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	(void)initialized;
#endif
}

std::pair<int, int> create_socket_pair()
{
#if defined(_WIN32)
	initialize_networking();

	int listener = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener < 0)
		throw_socket_error("socketpair: listener");

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (::bind(listener, (sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close_socket(listener);
		throw_socket_error("socketpair: bind");
	}

	socklen_t addr_len = sizeof(addr);
	if (getsockname(listener, (sockaddr *)&addr, &addr_len) < 0)
	{
		close_socket(listener);
		throw_socket_error("socketpair: getsockname");
	}

	if (::listen(listener, 1) < 0)
	{
		close_socket(listener);
		throw_socket_error("socketpair: listen");
	}

	int client = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (client < 0)
	{
		close_socket(listener);
		throw_socket_error("socketpair: client");
	}

	if (::connect(client, (sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close_socket(client);
		close_socket(listener);
		throw_socket_error("socketpair: connect");
	}

	int server = (int)::accept(listener, nullptr, nullptr);
	close_socket(listener);

	if (server < 0)
	{
		close_socket(client);
		throw_socket_error("socketpair: accept");
	}

	return {client, server};
#else
	int fds[2];
	if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) < 0)
		throw_socket_error("socketpair");
	return {fds[0], fds[1]};
#endif
}

process_id current_process_id()
{
#if defined(_WIN32)
	return GetCurrentProcessId();
#else
	return getpid();
#endif
}

bool process_exists(process_id pid)
{
#if defined(_WIN32)
	HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!handle)
		return false;
	DWORD code = 0;
	bool running = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
	CloseHandle(handle);
	return running;
#else
	return ::kill(pid, 0) == 0 || errno == EPERM;
#endif
}

void terminate_process(process_id pid, bool force)
{
#if defined(_WIN32)
	HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
	if (!handle)
		return;
	TerminateProcess(handle, force ? 1 : 0);
	CloseHandle(handle);
#else
	::kill(pid, force ? SIGKILL : SIGTERM);
#endif
}

namespace
{
std::filesystem::path environment_path(const char * name)
{
	if (const char * value = std::getenv(name); value && *value)
		return value;
	return {};
}
} // namespace

std::filesystem::path home_directory()
{
#if defined(_WIN32)
	if (auto user_profile = environment_path("USERPROFILE"); !user_profile.empty())
		return user_profile;

	auto drive = environment_path("HOMEDRIVE").string();
	auto path = environment_path("HOMEPATH").string();
	if (!drive.empty() || !path.empty())
		return drive + path;
#else
	if (auto home = environment_path("HOME"); !home.empty())
		return home;
#endif
	return {};
}

std::filesystem::path config_directory()
{
#if defined(_WIN32)
	if (auto appdata = environment_path("APPDATA"); !appdata.empty())
		return appdata / "wivrn";
	return home_directory() / "AppData" / "Roaming" / "wivrn";
#else
	if (auto xdg = environment_path("XDG_CONFIG_HOME"); !xdg.empty())
		return xdg / "wivrn";
	return home_directory() / ".config" / "wivrn";
#endif
}

std::filesystem::path cache_directory()
{
#if defined(_WIN32)
	if (auto local = environment_path("LOCALAPPDATA"); !local.empty())
		return local / "wivrn";
	return home_directory() / "AppData" / "Local" / "wivrn";
#else
	if (auto xdg = environment_path("XDG_CACHE_HOME"); !xdg.empty())
		return xdg / "wivrn";
	return home_directory() / ".cache" / "wivrn";
#endif
}

std::filesystem::path data_directory()
{
#if defined(_WIN32)
	if (auto local = environment_path("LOCALAPPDATA"); !local.empty())
		return local / "wivrn";
	return home_directory() / "AppData" / "Local" / "wivrn";
#else
	if (auto xdg = environment_path("XDG_DATA_HOME"); !xdg.empty())
		return xdg / "wivrn";
	return home_directory() / ".local" / "share" / "wivrn";
#endif
}

std::filesystem::path runtime_directory()
{
#if defined(_WIN32)
	return std::filesystem::temp_directory_path() / "wivrn";
#else
	if (auto xdg = environment_path("XDG_RUNTIME_DIR"); !xdg.empty())
		return xdg / "wivrn";
	return std::filesystem::temp_directory_path() / "wivrn";
#endif
}

} // namespace wivrn::platform
