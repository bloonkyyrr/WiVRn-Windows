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

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace
{
std::atomic<int> g_shutdown_signal{0};

#if defined(_WIN32)
BOOL WINAPI console_ctrl_handler(DWORD type)
{
	switch (type)
	{
		case CTRL_C_EVENT:
			g_shutdown_signal.store(SIGINT);
			return TRUE;
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			g_shutdown_signal.store(SIGTERM);
			return TRUE;
		default:
			return FALSE;
	}
}
#else
extern "C" void shutdown_signal_handler(int signal)
{
	g_shutdown_signal.store(signal);
}
#endif
} // namespace

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

	auto bind_loopback = [](int & fd, sockaddr_in & addr) {
		fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (fd < 0)
			throw_socket_error("socketpair: socket");

		addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;

		if (::bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
		{
			close_socket(fd);
			throw_socket_error("socketpair: bind");
		}

		socklen_t addr_len = sizeof(addr);
		if (getsockname(fd, (sockaddr *)&addr, &addr_len) < 0)
		{
			close_socket(fd);
			throw_socket_error("socketpair: getsockname");
		}
	};

	int first = -1;
	int second = -1;
	sockaddr_in first_addr{};
	sockaddr_in second_addr{};

	try
	{
		bind_loopback(first, first_addr);
		bind_loopback(second, second_addr);

		if (::connect(first, (sockaddr *)&second_addr, sizeof(second_addr)) < 0)
			throw_socket_error("socketpair: connect");
		if (::connect(second, (sockaddr *)&first_addr, sizeof(first_addr)) < 0)
			throw_socket_error("socketpair: connect");
	}
	catch (...)
	{
		if (first >= 0)
			close_socket(first);
		if (second >= 0)
			close_socket(second);
		throw;
	}

	return {first, second};
#else
	int fds[2];
	if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) < 0)
		throw_socket_error("socketpair");
	return {fds[0], fds[1]};
#endif
}

bool can_fork()
{
#if defined(_WIN32)
	return false;
#else
	return true;
#endif
}

process_id fork_process()
{
#if defined(_WIN32)
	throw std::logic_error("fork_process is not available on Windows");
#else
	pid_t pid = fork();
	if (pid < 0)
		throw std::system_error(errno, std::generic_category(), "fork");
	return pid;
#endif
}

void detach_session()
{
#if !defined(_WIN32)
	setsid();
#endif
}

void install_shutdown_handler()
{
#if defined(_WIN32)
	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
	struct sigaction action{};
	action.sa_handler = shutdown_signal_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);
#endif
}

bool shutdown_requested()
{
	return g_shutdown_signal.load() != 0;
}

int shutdown_signal_number()
{
	return g_shutdown_signal.load();
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

void terminate_process_group(process_id pid, bool force)
{
#if defined(_WIN32)
	// Windows has no process groups comparable to POSIX; terminate the process.
	terminate_process(pid, force);
#else
	::kill(-pid, force ? SIGKILL : SIGTERM);
#endif
}

void set_environment_variable(const char * name, const char * value, bool overwrite)
{
#if defined(_WIN32)
	(void)overwrite;
	_putenv_s(name, value);
#else
	setenv(name, value, overwrite ? 1 : 0);
#endif
}

process_id spawn_process(const std::vector<std::string> & args,
                         const std::optional<std::filesystem::path> & working_directory,
                         bool new_process_group)
{
	if (args.empty())
		throw std::invalid_argument("spawn_process: empty command");

#if defined(_WIN32)
	auto quote = [](const std::string & arg) {
		if (arg.find_first_of(" \t\"") == std::string::npos)
			return arg;

		std::string quoted = "\"";
		int backslashes = 0;
		for (char c: arg)
		{
			if (c == '\\')
			{
				++backslashes;
			}
			else if (c == '"')
			{
				quoted.append(backslashes * 2 + 1, '\\');
				quoted += '"';
				backslashes = 0;
			}
			else
			{
				quoted.append(backslashes, '\\');
				quoted += c;
				backslashes = 0;
			}
		}
		quoted.append(backslashes * 2, '\\');
		quoted += '"';
		return quoted;
	};

	std::string command_line;
	for (const auto & arg: args)
	{
		if (!command_line.empty())
			command_line += ' ';
		command_line += quote(arg);
	}

	std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
	mutable_command_line.push_back('\0');

	std::string cwd = working_directory ? working_directory->string() : std::string();

	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};

	DWORD flags = new_process_group ? CREATE_NEW_PROCESS_GROUP : 0;
	if (!CreateProcessA(nullptr,
	                    mutable_command_line.data(),
	                    nullptr,
	                    nullptr,
	                    FALSE,
	                    flags,
	                    nullptr,
	                    cwd.empty() ? nullptr : cwd.c_str(),
	                    &startup,
	                    &process))
	{
		throw std::system_error((int)GetLastError(), std::system_category(), "CreateProcess");
	}

	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return process.dwProcessId;
#else
	std::vector<char *> argv;
	argv.reserve(args.size() + 1);
	for (const auto & arg: args)
		argv.push_back(const_cast<char *>(arg.c_str()));
	argv.push_back(nullptr);

	pid_t pid = fork();
	if (pid < 0)
		throw std::system_error(errno, std::generic_category(), "fork");

	if (pid == 0)
	{
		if (new_process_group)
			setpgid(0, 0);

		if (working_directory && !working_directory->empty())
		{
			if (chdir(working_directory->c_str()) != 0)
				_exit(127);
		}

		execvp(argv[0], argv.data());
		_exit(127);
	}

	return pid;
#endif
}

bool wait_process(process_id pid, int & status)
{
#if defined(_WIN32)
	HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!handle)
		return false;

	bool ok = WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0;
	if (ok)
	{
		DWORD code = 0;
		ok = GetExitCodeProcess(handle, &code) != 0;
		if (ok)
			status = (int)code;
	}
	CloseHandle(handle);
	return ok;
#else
	int result;
	do
	{
		result = waitpid(pid, &status, 0);
	} while (result < 0 && errno == EINTR);
	return result == pid;
#endif
}

int process_exit_code(int status)
{
#if defined(_WIN32)
	return status;
#else
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool process_terminated_by_signal(int status)
{
#if defined(_WIN32)
	(void)status;
	return false;
#else
	return WIFSIGNALED(status);
#endif
}

std::string process_signal_name(int status)
{
#if defined(_WIN32)
	(void)status;
	return "terminated";
#else
	const char * name = strsignal(WTERMSIG(status));
	return name ? name : "unknown signal";
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
