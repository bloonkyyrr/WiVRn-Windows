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

#include <winsock2.h>
#include <ws2tcpip.h>

#include "main_platform.h"

#include "active_runtime.h"
#include "mdns.h"

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace wivrn
{

namespace
{

constexpr wchar_t window_class_name[] = L"WiVRnServerStatus";
constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_SHUTDOWN = WM_APP + 2;

constexpr int editor_id = 100;

constexpr int command_pair = 2001;
constexpr int command_disconnect = 2002;
constexpr int command_config = 2003;
constexpr int command_quit = 2004;
constexpr int command_runtime_set = 2005;
constexpr int command_runtime_restore = 2006;
constexpr int command_runtime_folder = 2007;

std::wstring widen(const std::string & value)
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

std::string format_status(const server_status & status, const std::string & error)
{
	std::ostringstream out;

	out << "Headset: ";
	if (status.headset_connected)
		out << (status.system_name.empty() ? "connected" : status.system_name);
	else
		out << "waiting for a connection";

	out << "\nSession: " << (status.session_running ? "running" : "idle");

	if (status.pairing_enabled)
		out << "\n\nPairing PIN: " << (status.pin.empty() ? "----" : status.pin)
		    << "\nEnter this code in the WiVRn app to pair the headset.";

	if (status.preferred_refresh_rate > 0)
		out << "\nRefresh rate: " << status.preferred_refresh_rate << " Hz";
	if (status.bitrate > 0)
		out << "\nBitrate: " << (status.bitrate / 1000000) << " Mbit/s";

	out << "\n\nHand tracking: " << (status.hand_tracking ? "yes" : "no")
	    << "\nEye tracking: " << (status.eye_gaze ? "yes" : "no")
	    << "\nFace tracking: " << (status.face_tracking ? "yes" : "no");

	out << "\n\nPaired headsets:";
	if (status.known_keys.empty())
	{
		out << "\n  (none)";
	}
	else
	{
		for (const auto & key: status.known_keys)
			out << "\n  " << (key.name.empty() ? "Unknown headset" : key.name);
	}

	if (not error.empty())
		out << "\n\nLast error: " << error;

	// Runtime selection, so the user can tell why an application does not see
	// the headset.
	try
	{
		auto runtime = active_runtime::query();
		out << "\n\nOpenXR runtime:";
		if (runtime.wivrn_active)
			out << "\n  WiVRn is the active runtime";
		else if (not runtime.manifest_found)
			out << "\n  the WiVRn runtime manifest was not found (reinstall WiVRn)";
		else if (runtime.active_runtime)
			out << "\n  active: " << *runtime.active_runtime;
		else
			out << "\n  no runtime is registered";

		if (runtime.manifest_found and not runtime.can_set_active)
			out << "\n  WiVRn can only make itself the system-wide runtime with administrator rights";

		out << "\n  OpenVR compatibility layer: "
		    << (runtime.openvr_configured ? runtime.openvr_compat.string() : "not configured");
	}
	catch (std::exception & e)
	{
		out << "\n\nOpenXR runtime: cannot be queried (" << e.what() << ")";
	}

	out << "\n\nConfiguration: " << platform::config_directory().string();
	out << "\nLogs are written by the console this process was started from.";

	return out.str();
}

class server_ui_windows : public server_ui
{
	event_loop * loop;
	ui_callbacks callbacks;

	std::mutex mutex;
	server_status status;
	std::string last_error;

	std::atomic<HWND> window{nullptr};
	std::atomic<bool> shutting_down{false};
	HWND editor = nullptr;
	std::jthread thread;

	static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
	{
		if (message == WM_NCCREATE)
		{
			auto * create = reinterpret_cast<CREATESTRUCTW *>(lparam);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		}

		auto * self = reinterpret_cast<server_ui_windows *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (not self)
			return DefWindowProcW(hwnd, message, wparam, lparam);

		return self->handle(hwnd, message, wparam, lparam);
	}

	/// Runs a user callback on the event loop thread, which owns the session state.
	void dispatch(const std::function<void()> & callback)
	{
		if (callback and loop)
			loop->invoke(callback);
	}

	void refresh()
	{
		std::string text;
		{
			std::lock_guard lock(mutex);
			text = format_status(status, last_error);
		}

		if (editor)
		{
			auto wide = widen(text);
			SetWindowTextW(editor, wide.c_str());
		}
	}

	/// Show the result of a menu action in the status text.
	void set_last_error(const std::string & message)
	{
		{
			std::lock_guard lock(mutex);
			last_error = message;
		}

		refresh();
	}

	void create_menu(HWND hwnd)
	{
		HMENU menu = CreateMenu();

		HMENU session = CreatePopupMenu();
		AppendMenuW(session, MF_STRING, command_disconnect, L"&Disconnect headset");
		AppendMenuW(session, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(session, MF_STRING, command_quit, L"&Quit");
		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(session), L"&Session");

		HMENU pairing = CreatePopupMenu();
		AppendMenuW(pairing, MF_STRING, command_pair, L"&Pair a new headset");
		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pairing), L"&Pairing");

		HMENU runtime_menu = CreatePopupMenu();
		AppendMenuW(runtime_menu, MF_STRING, command_runtime_set, L"Make WiVRn the &active OpenXR runtime");
		AppendMenuW(runtime_menu, MF_STRING, command_runtime_restore, L"&Restore the previous OpenXR runtime");
		AppendMenuW(runtime_menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(runtime_menu, MF_STRING, command_runtime_folder, L"Open the &runtime folder");
		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(runtime_menu), L"&Runtime");

		HMENU tools = CreatePopupMenu();
		AppendMenuW(tools, MF_STRING, command_config, L"Open &configuration folder");
		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");

		SetMenu(hwnd, menu);
	}

	LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
	{
		switch (message)
		{
			case WM_CREATE:
				editor = CreateWindowExW(0,
				                         L"EDIT",
				                         L"",
				                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
				                         0,
				                         0,
				                         0,
				                         0,
				                         hwnd,
				                         reinterpret_cast<HMENU>(editor_id),
				                         GetModuleHandleW(nullptr),
				                         nullptr);
				if (editor)
					SendMessageW(editor, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
				refresh();
				return 0;

			case WM_SIZE: {
				RECT area;
				GetClientRect(hwnd, &area);
				if (editor)
					MoveWindow(editor, 8, 8, std::max(0L, area.right - 16), std::max(0L, area.bottom - 16), TRUE);
				return 0;
			}

			case WM_APP_STATUS:
				refresh();
				return 0;

			case WM_APP_SHUTDOWN:
				shutting_down = true;
				DestroyWindow(hwnd);
				return 0;

			case WM_CLOSE:
				if (not shutting_down.exchange(true))
					dispatch(callbacks.quit);
				DestroyWindow(hwnd);
				return 0;

			case WM_DESTROY:
				editor = nullptr;
				window = nullptr;
				PostQuitMessage(0);
				return 0;

			case WM_COMMAND:
				switch (LOWORD(wparam))
				{
					case command_disconnect:
						dispatch(callbacks.disconnect);
						return 0;

					case command_quit:
						if (not shutting_down.exchange(true))
							dispatch(callbacks.quit);
						DestroyWindow(hwnd);
						return 0;

					case command_config: {
						auto path = platform::config_directory();
						std::error_code ec;
						std::filesystem::create_directories(path, ec);
						ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
						return 0;
					}

					case command_runtime_set: {
						std::string error;
						if (active_runtime::set_active(error))
							set_last_error({});
						else
							set_last_error(error);
						return 0;
					}

					case command_runtime_restore: {
						std::string error;
						if (active_runtime::restore_previous(error))
							set_last_error({});
						else
							set_last_error(error);
						return 0;
					}

					case command_runtime_folder: {
						auto manifests = active_runtime::manifest_path();
						auto path = manifests.empty() ? platform::config_directory() : manifests.front().parent_path();
						ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
						return 0;
					}

					case command_pair: {
						// Pairing needs the event loop thread; the PIN shows up in
						// the status text once the state machine generates it.
						auto enable = callbacks.enable_pairing;
						if (enable and loop)
						{
							loop->invoke([enable] {
								if (enable(0).empty())
									std::cerr << "Pairing is not available while a session is running" << std::endl;
							});
						}
						return 0;
					}

					default:
						return 0;
				}
		}

		return DefWindowProcW(hwnd, message, wparam, lparam);
	}

	void run()
	{
		WNDCLASSEXW window_class{};
		window_class.cbSize = sizeof(window_class);
		window_class.lpfnWndProc = &server_ui_windows::window_proc;
		window_class.hInstance = GetModuleHandleW(nullptr);
		window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		window_class.lpszClassName = window_class_name;
		RegisterClassExW(&window_class);

		HWND hwnd = CreateWindowExW(0,
		                            window_class_name,
		                            L"WiVRn",
		                            WS_OVERLAPPEDWINDOW,
		                            CW_USEDEFAULT,
		                            CW_USEDEFAULT,
		                            720,
		                            520,
		                            nullptr,
		                            nullptr,
		                            window_class.hInstance,
		                            this);
		if (not hwnd)
		{
			std::cerr << "Cannot create the WiVRn window, running without a desktop UI" << std::endl;
			return;
		}

		window = hwnd;
		create_menu(hwnd);
		ShowWindow(hwnd, SW_SHOWNORMAL);
		UpdateWindow(hwnd);

		MSG message;
		while (GetMessageW(&message, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}

public:
	explicit server_ui_windows(event_loop * loop_) :
	        loop(loop_) {}

	~server_ui_windows() override
	{
		if (not thread.joinable())
			return;

		shutting_down = true;
		if (HWND hwnd = window.load())
			PostMessageW(hwnd, WM_APP_SHUTDOWN, 0, 0);

		thread.request_stop();
		thread.join();
	}

	void start(const server_status & initial,
	           const std::string &,
	           ui_callbacks cb,
	           std::function<void()> on_ready) override
	{
		{
			std::lock_guard lock(mutex);
			status = initial;
		}

		callbacks = std::move(cb);

		// The caller wires the rest of the server up from this hook and expects
		// it to run before the window appears.
		if (on_ready)
			on_ready();

		thread = std::jthread([this](std::stop_token) { run(); });
	}

	void apply(const server_status & new_status) override
	{
		{
			std::lock_guard lock(mutex);
			status = new_status;
		}

		if (HWND hwnd = window.load())
			PostMessageW(hwnd, WM_APP_STATUS, 0, 0);
	}

	void emit_error(const std::string & where, const std::string & message) override
	{
		{
			std::lock_guard lock(mutex);
			last_error = where + ": " + message;
		}

		std::cerr << "Server error in " << where << ": " << message << std::endl;

		if (HWND hwnd = window.load())
			PostMessageW(hwnd, WM_APP_STATUS, 0, 0);
	}

	void show_pin(const std::string & pin) override
	{
		{
			std::lock_guard lock(mutex);
			status.pin = pin;
			status.pairing_enabled = true;
		}

		if (HWND hwnd = window.load())
		{
			ShowWindow(hwnd, SW_SHOWNORMAL);
			SetForegroundWindow(hwnd);
			PostMessageW(hwnd, WM_APP_STATUS, 0, 0);
		}
	}

	void clear_pin() override
	{
		{
			std::lock_guard lock(mutex);
			status.pin.clear();
			status.pairing_enabled = false;
		}

		if (HWND hwnd = window.load())
			PostMessageW(hwnd, WM_APP_STATUS, 0, 0);
	}

	std::unique_ptr<children_manager> make_children_manager(std::function<void()> state_changed) override
	{
		return std::make_unique<forked_children>(std::move(state_changed));
	}
};

mdns_record_t ptr_record(const std::string & service_type, const std::string & instance)
{
	mdns_record_t record{};
	record.name = {service_type.data(), service_type.size()};
	record.type = MDNS_RECORDTYPE_PTR;
	record.data.ptr.name = {instance.data(), instance.size()};
	return record;
}

mdns_record_t srv_record(const std::string & instance, const std::string & host, int port)
{
	mdns_record_t record{};
	record.name = {instance.data(), instance.size()};
	record.type = MDNS_RECORDTYPE_SRV;
	record.data.srv.priority = 0;
	record.data.srv.weight = 0;
	record.data.srv.port = (uint16_t)port;
	record.data.srv.name = {host.data(), host.size()};
	return record;
}

mdns_record_t txt_record(const std::string & instance, const std::string & key, const std::string & value)
{
	mdns_record_t record{};
	record.name = {instance.data(), instance.size()};
	record.type = MDNS_RECORDTYPE_TXT;
	record.data.txt.key = {key.data(), key.size()};
	record.data.txt.value = {value.data(), value.size()};
	return record;
}

mdns_record_t a_record(const std::string & host, in_addr address)
{
	mdns_record_t record{};
	record.name = {host.data(), host.size()};
	record.type = MDNS_RECORDTYPE_A;
	record.data.a.addr.sin_family = AF_INET;
	record.data.a.addr.sin_addr = address;
	return record;
}

std::string machine_host_name()
{
	char buffer[256]{};
	if (gethostname(buffer, sizeof(buffer)) != 0 or buffer[0] == '\0')
		return "wivrn";
	return buffer;
}

/// IPv4 addresses of this computer, so the headset can pick one that its
/// network can reach.
std::vector<in_addr> local_ipv4_addresses()
{
	// 169.254.0.0/16 (APIPA, the address of a network adapter that found no DHCP
	// server) only reaches the link it belongs to. A headset that tries it first
	// waits for the connection attempt to time out, so it is only announced when
	// the computer has no other address.
	std::vector<in_addr> routable;
	std::vector<in_addr> link_local;

	auto add = [&routable, &link_local](in_addr address) {
		if (address.s_addr == 0 or address.s_addr == htonl(INADDR_LOOPBACK))
			return;

		auto & addresses = (ntohl(address.s_addr) & 0xffff0000) == 0xa9fe0000 ? link_local : routable;

		for (const auto & existing: addresses)
		{
			if (existing.s_addr == address.s_addr)
				return;
		}
		addresses.push_back(address);
	};

	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo * result = nullptr;
	char host[256]{};
	if (gethostname(host, sizeof(host)) == 0 and getaddrinfo(host, nullptr, &hints, &result) == 0)
	{
		for (addrinfo * it = result; it; it = it->ai_next)
		{
			if (it->ai_family == AF_INET and it->ai_addr)
				add(reinterpret_cast<const sockaddr_in *>(it->ai_addr)->sin_addr);
		}
		freeaddrinfo(result);
	}

	if (routable.empty() and link_local.empty())
	{
		// The host name does not resolve to an address: ask the routing table
		// which local address reaches the network.
		int sock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock >= 0)
		{
			sockaddr_in destination{};
			destination.sin_family = AF_INET;
			destination.sin_port = htons(53);
			inet_pton(AF_INET, "8.8.8.8", &destination.sin_addr);
			if (connect(sock, reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) == 0)
			{
				sockaddr_in local{};
				int length = sizeof(local);
				if (getsockname(sock, reinterpret_cast<sockaddr *>(&local), &length) == 0)
					add(local.sin_addr);
			}
			platform::close_socket(sock);
		}
	}

	if (not routable.empty())
		return routable;

	return link_local;
}

/// Advertises the server over mDNS so headsets list the computer without any
/// manual address entry.
///
/// Windows' own DNS-SD registration (DnsServiceRegister) fails with
/// ERROR_INVALID_PARAMETER on some systems, which left the computer missing from
/// the headset's server list. Sending the records directly only needs a
/// UDP socket, so it works where that API does not.
class publisher_windows : public service_publisher
{
	std::jthread thread;

	static void announce(std::stop_token token,
	                     std::string service_type,
	                     std::string instance,
	                     std::string host,
	                     int port,
	                     std::map<std::string, std::string> txt)
	{
		auto addresses = local_ipv4_addresses();

		// One socket per local address: the announcement then reaches every network
		// the computer is on, not only the one holding the default route (a VPN
		// adapter often does).
		std::vector<int> sockets;
		for (in_addr address: addresses)
		{
			sockaddr_in iface{};
			iface.sin_family = AF_INET;
			iface.sin_addr = address;
			iface.sin_port = htons(MDNS_PORT);

			int sock = mdns_socket_open_ipv4(&iface);
			if (sock >= 0)
				sockets.push_back(sock);
		}

		if (sockets.empty())
		{
			int sock = mdns_socket_open_ipv4(nullptr);
			if (sock >= 0)
				sockets.push_back(sock);
		}

		if (sockets.empty())
		{
			std::cerr << "Cannot open the mDNS socket: headsets will not find this computer automatically" << std::endl;
			return;
		}

		auto answer = ptr_record(service_type, instance);

		std::vector<mdns_record_t> authority;
		authority.push_back(srv_record(instance, host, port));
		for (const auto & [key, value]: txt)
			authority.push_back(txt_record(instance, key, value));

		std::vector<mdns_record_t> additional;
		for (in_addr address: addresses)
			additional.push_back(a_record(host, address));

		std::cerr << "Advertising " << instance << " on port " << port << " through mDNS" << std::endl;

		// The records are small: 2KB is far more than the service needs, and a
		// 32-bit buffer keeps the records aligned as the mDNS writer expects.
		std::array<uint32_t, 512> buffer{};
		auto capacity = buffer.size() * sizeof(uint32_t);

		auto send = [&](bool goodbye) {
			for (int sock: sockets)
			{
				int result = goodbye
				        ? mdns_goodbye_multicast(sock, buffer.data(), capacity, answer,
				                                 authority.data(), authority.size(),
				                                 additional.empty() ? nullptr : additional.data(), additional.size())
				        : mdns_announce_multicast(sock, buffer.data(), capacity, answer,
				                                  authority.data(), authority.size(),
				                                  additional.empty() ? nullptr : additional.data(), additional.size());

				if (result)
					std::cerr << "Cannot send the mDNS " << (goodbye ? "goodbye" : "announcement") << std::endl;
			}
		};

		send(false);
		while (not token.stop_requested())
		{
			for (int i = 0; i < 50 and not token.stop_requested(); ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));

			if (token.stop_requested())
				break;

			// Refresh well before the records expire in the headset's cache.
			send(false);
		}

		send(true);

		for (int sock: sockets)
			mdns_socket_close(sock);
	}

public:
	publisher_windows(const std::string & name,
	                  const std::string & type,
	                  int port,
	                  const std::map<std::string, std::string> & txt)
	{
		platform::initialize_networking();

		std::string service_type = type;
		if (not service_type.ends_with('.'))
			service_type += '.';
		if (not service_type.ends_with(".local."))
			service_type += "local.";

		std::string instance = name + "." + service_type;
		// A host name of its own: Bonjour and the Windows mDNS responder also answer
		// for <computer>.local, and they hand out addresses that the headset cannot
		// reach (such as the link-local address of an idle adapter).
		std::string host = machine_host_name() + "-wivrn.local";

		thread = std::jthread([service_type, instance, host, port, txt](std::stop_token token) {
			announce(token, service_type, instance, host, port, txt);
		});
	}
};

struct fd_source
{
	int fd;
	std::function<void()> callback;
};

struct timeout_source
{
	std::chrono::steady_clock::time_point deadline;
	std::function<void()> callback;
};

class event_loop_windows : public event_loop
{
	struct task
	{
		std::function<void()> callback;
	};

	std::mutex mutex;
	std::vector<task> tasks;
	std::map<source_id, fd_source> fds;
	std::map<source_id, timeout_source> timeouts;
	std::vector<std::jthread> process_watchers;
	source_id next_id = 1;
	bool quitting = false;

public:
	void run() override
	{
		while (true)
		{
			{
				std::lock_guard lock(mutex);
				if (quitting)
					break;
			}

			std::vector<task> pending;
			{
				std::lock_guard lock(mutex);
				pending.swap(tasks);
			}
			for (auto & t: pending)
				t.callback();

			auto now = std::chrono::steady_clock::now();
			std::vector<std::function<void()>> expired;
			{
				std::lock_guard lock(mutex);
				for (auto it = timeouts.begin(); it != timeouts.end();)
				{
					if (it->second.deadline <= now)
					{
						expired.push_back(std::move(it->second.callback));
						it = timeouts.erase(it);
					}
					else
					{
						++it;
					}
				}
			}
			for (auto & callback: expired)
				callback();

			std::vector<pollfd> pollfds;
			std::vector<std::function<void()>> callbacks;
			{
				std::lock_guard lock(mutex);
				pollfds.reserve(fds.size());
				for (auto & [id, source]: fds)
				{
					pollfds.push_back({(SOCKET)(uintptr_t)source.fd, POLLIN, 0});
				}
			}

			int ready = 0;
			if (not pollfds.empty())
				ready = platform::poll_sockets(pollfds.data(), (unsigned long)pollfds.size(), 10);
			else
				std::this_thread::sleep_for(std::chrono::milliseconds(10));

			if (ready > 0)
			{
				std::lock_guard lock(mutex);
				size_t index = 0;
				for (auto & [id, source]: fds)
				{
					if (index < pollfds.size() and pollfds[index].revents)
						callbacks.push_back(source.callback);
					++index;
				}
			}
			for (auto & callback: callbacks)
				callback();
		}
	}

	void quit() override
	{
		std::lock_guard lock(mutex);
		quitting = true;
	}

	void invoke(std::function<void()> callback) override
	{
		std::lock_guard lock(mutex);
		tasks.push_back({std::move(callback)});
	}

	source_id add_fd(int fd, std::function<void()> callback) override
	{
		std::lock_guard lock(mutex);
		source_id id = next_id++;
		fds.emplace(id, fd_source{fd, std::move(callback)});
		return id;
	}

	source_id add_timeout(int milliseconds, std::function<void()> callback) override
	{
		std::lock_guard lock(mutex);
		source_id id = next_id++;
		timeouts.emplace(id, timeout_source{std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds), std::move(callback)});
		return id;
	}

	source_id add_process_watch(process_id pid, std::function<void(int)> callback) override
	{
		source_id id;
		{
			std::lock_guard lock(mutex);
			id = next_id++;
		}
		process_watchers.emplace_back([this, pid, callback = std::move(callback)](std::stop_token) {
			int status = 0;
			if (platform::wait_process(pid, status))
			{
				invoke([callback, status] {
					if (callback)
						callback(status);
				});
			}
		});
		return id;
	}

	void remove_source(source_id id) override
	{
		std::lock_guard lock(mutex);
		fds.erase(id);
		timeouts.erase(id);
	}

	void run_in_thread(std::function<int()> entry, std::function<void(int)> on_exit) override
	{
		std::thread([entry = std::move(entry), on_exit = std::move(on_exit), this] {
			int result = entry();
			invoke([on_exit, result] {
				if (on_exit)
					on_exit(result);
			});
		}).detach();
	}

	std::unique_ptr<service_publisher> publish_service(
	        const std::string & name,
	        const std::string & type,
	        int port,
	        const std::map<std::string, std::string> & txt) override
	{
		return std::make_unique<publisher_windows>(name, type, port, txt);
	}
};

} // namespace

std::unique_ptr<server_ui> make_server_ui(event_loop * loop)
{
	return std::make_unique<server_ui_windows>(loop);
}

std::unique_ptr<event_loop> make_event_loop()
{
	return std::make_unique<event_loop_windows>();
}

std::filesystem::path ipc_socket_path()
{
	return platform::runtime_directory() / "comp_ipc";
}

int create_ipc_listen_socket()
{
	platform::initialize_networking();

	int fd = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0)
		platform::throw_socket_error("socket");

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (::bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
	{
		platform::close_socket(fd);
		platform::throw_socket_error("bind");
	}

	if (::listen(fd, SOMAXCONN) < 0)
	{
		platform::close_socket(fd);
		platform::throw_socket_error("listen");
	}

	platform::set_close_on_exec(fd);
	return fd;
}

} // namespace wivrn
