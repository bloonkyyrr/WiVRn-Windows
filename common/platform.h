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

// Small cross-platform shim for the POSIX primitives used across WiVRn.
// Everything that is not portable is concentrated here so the rest of the
// codebase can be compiled on Windows without a forest of #ifdef blocks.

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef POLLIN
#define POLLIN 0x0100
#endif
#ifndef POLLHUP
#define POLLHUP 0x0002
#endif
#ifndef POLLERR
#define POLLERR 0x0008
#endif
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wivrn::platform
{
#if defined(_WIN32)
using process_id = DWORD;
using socket_handle = SOCKET;
#else
using process_id = pid_t;
using socket_handle = int;
#endif

/// Last error raised by a socket operation.
int last_socket_error();

/// Error code for the given socket error value.
std::error_code socket_error_code(int error);

/// Error code for the last socket operation.
inline std::error_code last_socket_error_code()
{
	return socket_error_code(last_socket_error());
}

/// Throw std::system_error for the last socket error.
[[noreturn]] void throw_socket_error(const char * what);

/// Human-readable text for an OS error value (strerror/FormatMessage).
std::string error_message(int error);

/// Close a socket/fd.
int close_socket(int fd);

/// Wait for socket events (poll/WSAPoll).
int poll_sockets(pollfd * fds, unsigned long count, int timeout_ms);

/// Set close-on-exec where the platform supports it (no-op on Windows sockets).
void set_close_on_exec(int fd);

/// Toggle non-blocking mode on a socket. Returns false on failure.
bool set_nonblocking(int fd, bool non_blocking);

/// Shut a socket down for reading and writing.
int shutdown_socket(int fd);

/// Create a connected pair of datagram-like sockets.
/// POSIX uses socketpair(AF_UNIX); Windows emulates it with a loopback TCP pair.
std::pair<int, int> create_socket_pair();

/// Initialise platform networking (WSAStartup on Windows). Idempotent.
void initialize_networking();

/// Process helpers.
process_id current_process_id();
bool process_exists(process_id pid);
void terminate_process(process_id pid, bool force);

/// User directories, following XDG on POSIX and the standard Windows folders.
std::filesystem::path home_directory();
std::filesystem::path config_directory();
std::filesystem::path cache_directory();
std::filesystem::path data_directory();
std::filesystem::path runtime_directory();

} // namespace wivrn::platform
