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

#include "adb.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <sstream>

namespace wivrn::adb
{

namespace
{

std::string narrow(const std::wstring & value)
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

std::string quote(const std::string & argument)
{
	std::string quoted = "\"";
	for (char c: argument)
	{
		if (c == '"')
			quoted += "\\\"";
		else
			quoted += c;
	}
	quoted += '"';
	return quoted;
}

struct command_result
{
	bool started = false;
	unsigned long exit_code = 1;
	std::string output;
};

/// Runs a command with its output captured and no console window.
command_result run_command(const std::string & command_line)
{
	command_result result;

	SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
	HANDLE read_pipe = nullptr;
	HANDLE write_pipe = nullptr;
	if (not CreatePipe(&read_pipe, &write_pipe, &attributes, 0))
		return result;
	SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	startup.hStdOutput = write_pipe;
	startup.hStdError = write_pipe;

	PROCESS_INFORMATION process{};
	std::wstring wide = widen(command_line);
	if (not CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
	{
		CloseHandle(write_pipe);
		CloseHandle(read_pipe);
		return result;
	}

	CloseHandle(write_pipe);
	result.started = true;

	// Drain the pipe while adb runs: a full pipe would block the child.
	std::thread reader([&result, read_pipe] {
		char buffer[4096];
		DWORD read = 0;
		while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) and read > 0)
			result.output.append(buffer, read);
	});

	if (WaitForSingleObject(process.hProcess, 15000) == WAIT_TIMEOUT)
		TerminateProcess(process.hProcess, 1);
	WaitForSingleObject(process.hProcess, 1000);
	GetExitCodeProcess(process.hProcess, &result.exit_code);

	reader.join();

	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	CloseHandle(read_pipe);

	return result;
}

std::string trim(std::string value)
{
	while (not value.empty() and (value.back() == '\n' or value.back() == '\r' or value.back() == ' ' or value.back() == '\t'))
		value.pop_back();
	while (not value.empty() and (value.front() == '\n' or value.front() == '\r' or value.front() == ' ' or value.front() == '\t'))
		value.erase(value.begin());
	return value;
}

} // namespace

std::string executable()
{
	static const std::string path = [] {
		wchar_t buffer[MAX_PATH];
		DWORD length = SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, buffer, nullptr);
		if (length == 0 or length >= MAX_PATH)
			return std::string{};
		return narrow(std::wstring(buffer, length));
	}();
	return path;
}

std::vector<device> list_devices(std::string & error)
{
	std::vector<device> devices;

	auto adb = executable();
	if (adb.empty())
	{
		error = "adb was not found on PATH: install Android platform-tools to connect a headset over USB";
		return devices;
	}

	auto command = run_command(quote(adb) + " devices -l");
	if (not command.started)
	{
		error = "cannot run adb";
		return devices;
	}

	std::istringstream lines(command.output);
	std::string line;
	while (std::getline(lines, line))
	{
		line = trim(line);
		if (line.empty() or line.starts_with("List of devices") or line.starts_with('*'))
			continue;

		std::istringstream fields(line);
		device entry;
		fields >> entry.serial >> entry.state;
		if (entry.serial.empty() or entry.state.empty())
			continue;

		std::string field;
		while (fields >> field)
		{
			if (field.starts_with("model:"))
				entry.model = field.substr(6);
		}

		// adb reports the model with underscores ("Quest_3").
		std::replace(entry.model.begin(), entry.model.end(), '_', ' ');

		devices.push_back(std::move(entry));
	}

	return devices;
}

bool forward(const device & target, int port, std::string & error)
{
	auto adb = executable();
	if (adb.empty())
	{
		error = "adb was not found on PATH";
		return false;
	}

	auto port_string = std::to_string(port);
	auto command = run_command(
	        quote(adb) + " -s " + quote(target.serial) +
	        " reverse tcp:" + port_string + " tcp:" + port_string);

	if (not command.started)
	{
		error = "cannot run adb";
		return false;
	}

	if (command.exit_code != 0)
	{
		error = trim(command.output);
		if (error.empty())
			error = "adb reverse failed";
		return false;
	}

	return true;
}

bool launch_client(const device & target, int port, std::string & error)
{
	auto adb = executable();
	if (adb.empty())
	{
		error = "adb was not found on PATH";
		return false;
	}

	// The intent alone is enough: the WiVRn app registers the wivrn+tcp scheme,
	// so this works for every install type (store, Github, local builds).
	auto uri = "wivrn+tcp://localhost:" + std::to_string(port);
	auto command = run_command(
	        quote(adb) + " -s " + quote(target.serial) +
	        " shell am start -a android.intent.action.VIEW -d " + quote(uri));

	if (not command.started)
	{
		error = "cannot run adb";
		return false;
	}

	if (command.exit_code != 0)
	{
		error = trim(command.output);
		if (error.empty())
			error = "cannot start the WiVRn app on the headset";
		return false;
	}

	return true;
}

void service::start(int port_, std::function<void()> on_change_)
{
	{
		std::lock_guard lock(mutex);
		port = port_;
		on_change = std::move(on_change_);
		current = {};
		current.enabled = true;
		current.available = not executable().empty();
	}

	thread = std::jthread([this](std::stop_token token) { run(token); });
}

void service::stop()
{
	if (thread.joinable())
	{
		thread.request_stop();
		thread.join();
	}
}

status service::get() const
{
	std::lock_guard lock(mutex);
	return current;
}

void service::refresh()
{
	poll_requested = true;
}

void service::connect()
{
	connect_requested = true;
}

void service::run(std::stop_token token)
{
	using clock = std::chrono::steady_clock;
	// Serial -> when its reverse forward was last set up.
	std::map<std::string, clock::time_point> forwarded;
	auto next_poll = clock::now();

	while (not token.stop_requested())
	{
		auto now = clock::now();
		// Read it once: the poll below is what launches the app.
		bool launch = connect_requested.exchange(false);
		if (launch)
			poll_requested = true;

		if (not poll_requested.exchange(false) and now < next_poll)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		next_poll = now + std::chrono::seconds(3);

		status next;
		next.enabled = true;
		next.available = not executable().empty();

		std::string error;
		if (next.available)
			next.devices = list_devices(error);
		if (not error.empty())
			next.message = error;

		for (auto & entry: next.devices)
		{
			if (entry.state != "device")
			{
				if (entry.state == "unauthorized")
					next.message = "Allow USB debugging in the headset (a prompt appears inside it)";
				continue;
			}

			// Forward again after a reconnect: the reverse tunnel lives in the
			// device's adb session, not in this process.
			auto & last = forwarded[entry.serial];
			if (last == clock::time_point{} or now - last > std::chrono::seconds(20))
			{
				std::string forward_error;
				if (forward(entry, port, forward_error))
				{
					last = now;
					entry.forwarded = true;
				}
				else
				{
					next.message = forward_error;
				}
			}
			else
				entry.forwarded = true;
		}

		std::erase_if(forwarded, [&next](const auto & entry) {
			return std::ranges::none_of(next.devices, [&entry](const device & d) { return d.serial == entry.first; });
		});

		{
			std::lock_guard lock(mutex);
			current = next;
		}

		if (on_change)
			on_change();

		if (launch)
		{
			std::string launch_error;
			for (const auto & entry: next.devices)
			{
				if (entry.state == "device" and not launch_client(entry, port, launch_error))
					break;
			}

			if (not launch_error.empty())
			{
				std::lock_guard lock(mutex);
				current.message = launch_error;
				if (on_change)
					on_change();
			}
		}
	}
}

} // namespace wivrn::adb
