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

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace wivrn
{

namespace
{

std::wstring widen(const std::string & value)
{
	if (value.empty())
		return {};

	int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
	std::wstring result(size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), size);
	return result;
}

class server_ui_windows : public server_ui
{
	server_status status;

public:
	void start(const server_status & initial,
	           const std::string &,
	           ui_callbacks,
	           std::function<void()> on_ready) override
	{
		status = initial;
		OutputDebugStringA("WiVRn: Windows server UI initialized\n");
		if (on_ready)
			on_ready();
	}

	void apply(const server_status & new_status) override
	{
		status = new_status;
	}

	void emit_error(const std::string & where, const std::string & message) override
	{
		std::cerr << "Server error in " << where << ": " << message << std::endl;
		OutputDebugStringA(("WiVRn server error: " + where + ": " + message + "\n").c_str());
	}

	void show_pin(const std::string & pin) override
	{
		OutputDebugStringA(("WiVRn pairing PIN: " + pin + "\n").c_str());
	}

	void clear_pin() override {}

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

std::unique_ptr<server_ui> make_server_ui()
{
	return std::make_unique<server_ui_windows>();
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
