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

#include "driver/configuration.h"
#include "platform.h"
#include "start_application.h"
#include "wivrn_packets.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wivrn
{

/// Snapshot of everything the desktop UI/service exposes.
struct server_status
{
	bool headset_connected = false;
	bool session_running = false;
	bool pairing_enabled = false;
	bool encryption_enabled = false;
	std::string pin;

	std::vector<float> available_refresh_rates;
	double preferred_refresh_rate = 0;
	int bitrate = 0;

	int speaker_channels = 0;
	int speaker_sample_rate = 0;
	int mic_channels = 0;
	int mic_sample_rate = 0;

	std::vector<XrFovf> field_of_view;
	bool hand_tracking = false;
	bool eye_gaze = false;
	bool face_tracking = false;

	std::vector<video_codec> supported_codecs;
	std::string system_name;
	std::string client_tab;
	std::string steam_command;
	std::vector<headset_key> known_keys;
};

/// Callbacks from the desktop UI back into the server state machine.
struct ui_callbacks
{
	std::function<void()> disconnect;
	std::function<void()> quit;
	std::function<void(std::string)> revoke_key;
	std::function<void(std::string, std::string)> rename_key;
	/// Returns the generated PIN, or an empty string if pairing is not allowed.
	std::function<std::string(int timeout_seconds)> enable_pairing;
	std::function<void()> disable_pairing;
	std::function<void(std::string)> set_client_tab;
	std::function<void(std::string)> json_configuration_changed;
	std::function<void(int)> bitrate_changed;
};

/// Desktop integration: a Win32 status window driven by the server state.
class server_ui
{
public:
	virtual ~server_ui() = default;

	virtual void start(const server_status & initial,
	                   const std::string & initial_json_configuration,
	                   ui_callbacks callbacks,
	                   std::function<void()> on_ready) = 0;

	virtual void apply(const server_status & status) = 0;
	virtual void emit_error(const std::string & where, const std::string & message) = 0;

	/// Called when the user can pair a new headset, brings the window forward.
	virtual void show_pin(const std::string & pin) = 0;
	virtual void clear_pin() = 0;

	/// Create the child manager for this platform.
	virtual std::unique_ptr<children_manager> make_children_manager(std::function<void()> state_changed) = 0;
};

class event_loop;

/// Create the desktop UI. User actions are posted to \p loop so that they run
/// on the thread that owns the session state.
std::unique_ptr<server_ui> make_server_ui(event_loop * loop);

/// mDNS advertisement of the server so headsets discover it automatically.
class service_publisher
{
public:
	virtual ~service_publisher() = default;
};

class event_loop
{
public:
	using source_id = std::uint64_t;

	virtual ~event_loop() = default;

	virtual void run() = 0;
	virtual void quit() = 0;

	/// Run a callback in the loop thread from any thread.
	virtual void invoke(std::function<void()> callback) = 0;

	virtual source_id add_fd(int fd, std::function<void()> callback) = 0;
	virtual source_id add_timeout(int milliseconds, std::function<void()> callback) = 0;
	virtual source_id add_process_watch(process_id pid, std::function<void(int)> callback) = 0;
	virtual void remove_source(source_id id) = 0;

	/// Run the server entry point in a thread of this process (used where fork is unavailable).
	virtual void run_in_thread(std::function<int()> entry, std::function<void(int)> on_exit) = 0;

	virtual std::unique_ptr<service_publisher> publish_service(
	        const std::string & name,
	        const std::string & type,
	        int port,
	        const std::map<std::string, std::string> & txt) = 0;
};

std::unique_ptr<event_loop> make_event_loop();

/// Path of the Monado IPC socket used between the service and the runtime.
std::filesystem::path ipc_socket_path();

/// Create the listening socket for the Monado IPC channel.
int create_ipc_listen_socket();

} // namespace wivrn
