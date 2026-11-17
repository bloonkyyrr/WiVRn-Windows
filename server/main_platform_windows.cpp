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

#include "main_platform.h"

#include <windns.h>

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

class publisher_windows : public service_publisher
{
	DNS_SERVICE_REGISTER_REQUEST request{};
	PDNS_SERVICE_INSTANCE instance = nullptr;
	bool registered = false;

public:
	publisher_windows(const std::string & name,
	                  const std::string & type,
	                  int port,
	                  const std::map<std::string, std::string> & txt)
	{
		platform::initialize_networking();

		std::string instance_name = name + "." + type;
		if (instance_name.find(".local") == std::string::npos)
			instance_name += ".local";

		char host_buffer[256]{};
		gethostname(host_buffer, sizeof(host_buffer));
		std::string host = host_buffer;
		if (host.find('.') == std::string::npos)
			host += ".local";

		std::vector<std::wstring> txt_keys;
		std::vector<std::wstring> txt_values;
		for (const auto & [key, value]: txt)
		{
			txt_keys.push_back(widen(key));
			txt_values.push_back(widen(value));
		}

		std::vector<PCWSTR> key_ptrs;
		std::vector<PCWSTR> value_ptrs;
		for (const auto & key: txt_keys)
			key_ptrs.push_back(key.c_str());
		for (const auto & value: txt_values)
			value_ptrs.push_back(value.c_str());

		auto wide_instance = widen(instance_name);
		auto wide_host = widen(host);

		instance = DnsServiceConstructInstance(wide_instance.c_str(),
		                                       wide_host.c_str(),
		                                       nullptr,
		                                       nullptr,
		                                       htons((unsigned short)port),
		                                       0,
		                                       0,
		                                       (DWORD)key_ptrs.size(),
		                                       key_ptrs.empty() ? nullptr : key_ptrs.data(),
		                                       value_ptrs.empty() ? nullptr : value_ptrs.data());

		if (not instance)
		{
			std::cerr << "DnsServiceConstructInstance failed" << std::endl;
			return;
		}

		request.Version = DNS_QUERY_REQUEST_VERSION1;
		request.InterfaceIndex = 0;
		request.pServiceInstance = instance;
		request.unicastEnabled = FALSE;

		// A null completion routine makes DnsServiceRegister synchronous.
		DNS_STATUS status = DnsServiceRegister(&request, nullptr);
		registered = status == ERROR_SUCCESS;
		if (not registered)
			std::cerr << "DnsServiceRegister failed: " << status << std::endl;
	}

	~publisher_windows() override
	{
		if (registered)
			DnsServiceDeRegister(&request, nullptr);
		if (instance)
			DnsServiceFreeInstance(instance);
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
