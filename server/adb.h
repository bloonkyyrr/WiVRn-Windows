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

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wivrn::adb
{

/// A headset seen by `adb devices`.
struct device
{
	std::string serial;
	std::string model;
	/// "device" once the headset accepted the USB debugging prompt,
	/// "unauthorized" or "offline" otherwise.
	std::string state;
	/// True when the server port is forwarded to the headset's localhost.
	bool forwarded = false;
};

/// What the desktop UI shows about the USB connection.
struct status
{
	/// False when the feature was disabled on the command line.
	bool enabled = false;
	/// False when no adb executable was found.
	bool available = false;
	/// Last message for the user, such as an authorisation hint or an adb error.
	std::string message;
	std::vector<device> devices;
};

/// adb.exe found on PATH, empty when Android platform-tools are not installed.
std::string executable();

/// Runs `adb devices -l`.
std::vector<device> list_devices(std::string & error);

/// Makes the headset's localhost:\p port reach the server over the cable
/// (`adb reverse tcp:<port> tcp:<port>`).
bool forward(const device & target, int port, std::string & error);

/// Starts the WiVRn app on the headset with a `wivrn+tcp://localhost` intent, so
/// that it connects over the cable instead of the network.
bool launch_client(const device & target, int port, std::string & error);

/// Watches the USB devices in the background and keeps the port forwarded.
class service
{
public:
	service() = default;
	service(const service &) = delete;
	service & operator=(const service &) = delete;
	~service() { stop(); }

	/// \param on_change Called after every poll, from the service thread.
	void start(int port, std::function<void()> on_change);
	void stop();

	status get() const;

	/// Poll the devices now instead of waiting for the next period.
	void refresh();

	/// Run the WiVRn app on every authorised device.
	void connect();

private:
	void run(std::stop_token token);

	mutable std::mutex mutex;
	status current;
	std::jthread thread;
	int port = 9757;
	std::function<void()> on_change;
	std::atomic<bool> poll_requested{true};
	std::atomic<bool> connect_requested{false};
};

} // namespace wivrn::adb
