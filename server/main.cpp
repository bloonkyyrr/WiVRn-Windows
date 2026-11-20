/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2026 Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2026  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "application.h"
#include "openxr/openxr.h"
#include "sleep_inhibitor.h"
#include "util/u_trace_marker.h"

#include "active_runtime.h"
#include "adb.h"
#include "driver/configuration.h"
#include "driver/wivrn_connection.h"
#include "exit_codes.h"
#include "hostname.h"
#include "ipc_server_cb.h"
#include "main_platform.h"
#include "platform.h"
#include "protocol_version.h"
#include "start_application.h"
#include "utils/overloaded.h"
#include "version.h"
#include "wivrn_config.h"
#include "wivrn_ipc.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <CLI/CLI.hpp>
#include <chrono> // IWYU pragma: keep
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <magic_enum.hpp>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>

// Insert the on load constructor to init trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)

extern "C"
{
	int listen_socket = -1;
}

using namespace wivrn;
using namespace std::chrono_literals;

std::optional<wivrn::typed_socket<wivrn::UnixDatagram, from_monado::packets, to_monado::packets>> wivrn_ipc_socket_main_loop;
std::optional<wivrn::typed_socket<wivrn::UnixDatagram, to_monado::packets, from_monado::packets>> wivrn_ipc_socket_monado;

namespace
{
int control_pipe_fds[2] = {-1, -1};

std::unique_ptr<event_loop> loop;
std::unique_ptr<server_ui> ui;
std::unique_ptr<service_publisher> publisher;

std::unique_ptr<TCPListener> listener;
server_status status;

event_loop::source_id listener_watch = 0;
event_loop::source_id server_watch = 0;
event_loop::source_id server_kill_watch = 0;
event_loop::source_id pairing_timeout = 0;

process_id server_pid = 0;
bool server_thread_running = false;
bool quitting_main_loop = false;
bool do_fork = true;
bool do_active_runtime = true;
#if WIVRN_FEATURE_DEBUG_GUI
bool show_debug_gui = false;
#endif

std::optional<std::jthread> connection_thread;
std::unique_ptr<children_manager> children;
wivrn::service_publication publication = wivrn::service_publication::none;

wivrn_connection::encryption_state enc_state = wivrn_connection::encryption_state::enabled;
std::string pin;
std::chrono::milliseconds delay_next_try = 10ms;
const std::chrono::milliseconds default_delay_next_try = 10ms;

std::optional<active_runtime> runtime_setter;
std::optional<sleep_inhibitor> inhibitor;

std::unique_ptr<adb::service> adb_watcher;
bool use_adb = true;

void update_fsm();
void start_publishing();
void stop_publishing();
void start_listening();
void stop_listening();
void set_encryption_state(wivrn_connection::encryption_state new_enc_state);
void on_headset_info_packet(const wivrn::from_headset::headset_info_packet & info);
void expose_known_keys();
void update_adb_status();

bool server_running()
{
	return server_watch != 0 or server_thread_running or connection_thread.has_value();
}

void on_server_exit(int status_code)
{
	server_thread_running = false;
	server_watch = 0;
	if (server_kill_watch)
	{
		loop->remove_source(server_kill_watch);
		server_kill_watch = 0;
	}
	status.session_running = false;
	ui->apply(status);
	display_child_status(status_code, "Server");
	update_fsm();
}

std::string steam_command()
{
	// On Windows the OpenVR runtime is selected through openvrpaths.vrpath, so
	// no Steam launch options are needed for the compatibility layer.
	return {};
}

void start_server(configuration config)
{
#if WIVRN_FEATURE_DEBUG_GUI
	ipc_server_main_info server_info{
	        .udgci = {.window_title = "WiVRn",
	                  .open = (config.debug_gui or show_debug_gui) ? U_DEBUG_GUI_OPEN_ALWAYS : U_DEBUG_GUI_OPEN_AUTO},
	        .no_stdin = true,
	};
#else
	ipc_server_main_info server_info{
	        .udgci = {.window_title = "WiVRn", .open = U_DEBUG_GUI_OPEN_NEVER},
	        .no_stdin = true,
	};
#endif

	auto run_server = [server_info]() -> int {
		platform::set_environment_variable("AMD_DEBUG", "lowlatencyenc", false);
		platform::set_environment_variable("INTEL_DEBUG", "noccs", false);
		platform::set_environment_variable("XRT_LOG", "info", false);

		wivrn::ipc_server_cb server_cb;
		try
		{
			return ipc_server_main_common(&server_info, &server_cb, nullptr);
		}
		catch (std::exception & e)
		{
			std::cerr << e.what() << std::endl;
			return EXIT_FAILURE;
		}
	};

	if (do_fork and platform::can_fork())
	{
		server_pid = platform::fork_process();

		if (server_pid == 0)
		{
			platform::detach_session();
			exit(run_server());
		}

		std::cerr << "Server started, PID " << server_pid << std::endl;
		status.session_running = true;
		ui->apply(status);
		server_watch = loop->add_process_watch(server_pid, [](int status_code) {
			on_server_exit(status_code);
		});
	}
	else if (platform::can_fork())
	{
		// --no-fork: run the runtime in this process, as before.
		exit(run_server());
	}
	else
	{
		server_thread_running = true;
		status.session_running = true;
		ui->apply(status);
		loop->run_in_thread(run_server, [](int status_code) {
			on_server_exit(status_code);
		});
	}

	if (do_active_runtime)
		runtime_setter.emplace();
}

void kill_server()
{
	wivrn_ipc_socket_main_loop->send(to_monado::stop{});

	if (platform::can_fork() and server_pid > 0)
	{
		// Send SIGTERM after 1s if it is still running
		server_kill_watch = loop->add_timeout(3500, [] {
			if (server_pid > 0)
				platform::terminate_process_group(server_pid, false);
		});
	}
}

void start_listening()
{
	if (listener)
		return;

	listener = std::make_unique<TCPListener>(configuration().port);
	listener_watch = loop->add_fd(listener->get_fd(), [] {
		if (not listener)
			return;
		TCP tcp = listener->accept().first;
		stop_listening();
		stop_publishing();

		auto tcp_ptr = std::make_shared<TCP>(std::move(tcp));
		connection_thread.emplace([tcp_ptr](std::stop_token stop_token, std::string pin_copy, wivrn_connection::encryption_state state) {
			try
			{
				connection = std::make_unique<wivrn_connection>(stop_token, state, pin_copy, std::move(*tcp_ptr));
				loop->invoke([] {
					if (connection_thread)
						connection_thread->request_stop();
					connection_thread.reset();
					if (enc_state == wivrn_connection::encryption_state::pairing)
						set_encryption_state(wivrn_connection::encryption_state::enabled);

					std::cerr << "Client connected" << std::endl;
					expose_known_keys();

					configuration c;
					start_server(c);
					try
					{
						children->start_application(c.application);
					}
					catch (std::exception & e)
					{
						std::cerr << "Failed to start application: " << e.what() << std::endl;
					}

					delay_next_try = default_delay_next_try;
					connection.reset();
				});
			}
			catch (wivrn::incorrect_pin &)
			{
				loop->invoke([] {
					connection_thread.reset();
					delay_next_try = 2 * delay_next_try;
					std::cerr << "Waiting " << delay_next_try << " until the next attempt is allowed" << std::endl;
					update_fsm();
				});
			}
			catch (std::exception & e)
			{
				loop->invoke([what = std::string(e.what())] {
					connection_thread.reset();
					std::cerr << "Client connection failed: " << what << std::endl;
					update_fsm();
				});
			}
		},
		                          pin,
		                          enc_state);
	});
}

void stop_listening()
{
	if (not listener)
		return;

	if (listener_watch)
		loop->remove_source(listener_watch);
	listener_watch = 0;
	listener.reset();
}

void start_publishing()
{
	if (publication == wivrn::service_publication::none or publisher)
		return;

	char protocol_string[17];
	sprintf(protocol_string, "%016" PRIx64, wivrn::protocol_version);
	std::map<std::string, std::string> txt = {
	        {"protocol", protocol_string},
	        {"version", wivrn::display_version()},
	        {"cookie", server_cookie()},
	};

	publisher = loop->publish_service(configuration().hostname, "_wivrn._tcp", configuration().port, txt);
}

void stop_publishing()
{
	publisher.reset();
}

void update_fsm()
{
	bool server_is_running = server_running();
	bool app_running = children and children->running();

	if (quitting_main_loop)
	{
		connection_thread.reset();

		if (server_is_running)
			kill_server();

		if (app_running)
			children->stop();

		if (not server_is_running and not app_running)
		{
			children.reset();
			loop->quit();
		}
	}
	else
	{
		if (not server_is_running and app_running)
			children->stop();

		if (not server_is_running)
		{
			runtime_setter.reset();

			loop->add_timeout(delay_next_try.count(), [] {
				start_listening();
				start_publishing();
				status.headset_connected = false;
				ui->apply(status);
			});
		}
	}
}

void control_received()
{
	auto packet = wivrn_ipc_socket_main_loop->receive();
	if (not packet)
		return;

	std::visit(utils::overloaded{
	                   [&](const wivrn::from_headset::headset_info_packet & info) {
		                   on_headset_info_packet(info);
		                   inhibitor.emplace();
		                   status.headset_connected = true;
		                   ui->apply(status);
	                   },
	                   [&](const wivrn::from_headset::settings_changed & settings) {
		                   status.preferred_refresh_rate = settings.preferred_refresh_rate;
		                   status.bitrate = settings.bitrate_bps;
		                   ui->apply(status);
	                   },
	                   [&](const wivrn::from_headset::start_app & request) {
		                   const auto & apps = list_applications();
		                   if (auto it = apps.find(request.app_id); it != apps.end())
		                   {
			                   try
			                   {
				                   children->start_application(it->second.exec, it->second.path);
			                   }
			                   catch (std::exception & e)
			                   {
				                   std::cerr << "Failed to launch application " << it->second.name.at("") << ": " << e.what() << std::endl;
			                   }
		                   }
	                   },
	                   [&](const from_monado::headset_connected &) {
		                   stop_publishing();
		                   inhibitor.emplace();
		                   status.headset_connected = true;
		                   ui->apply(status);
	                   },
	                   [&](const from_monado::headset_disconnected &) {
		                   start_publishing();
		                   inhibitor.reset();
		                   status.headset_connected = false;
		                   status.client_tab = "";
		                   ui->apply(status);
	                   },
	                   [&](const wivrn::from_headset::stream_tab_changed & event) {
		                   status.client_tab = magic_enum::enum_name(event.tab).data();
		                   ui->apply(status);
	                   },
	                   [&](const from_monado::server_error & e) {
		                   ui->emit_error(e.where, e.message);
	                   },
	           },
	           *packet);
}

void set_encryption_state(wivrn_connection::encryption_state new_enc_state)
{
	ui->clear_pin();

	status.pin.clear();
	status.pairing_enabled = false;
	status.encryption_enabled = false;

	switch (new_enc_state)
	{
		case wivrn_connection::encryption_state::disabled:
			std::cerr << "Encryption is disabled" << std::endl;
			break;

		case wivrn_connection::encryption_state::enabled:
			if (enc_state != wivrn_connection::encryption_state::enabled)
				std::cerr << "Headset pairing is disabled" << std::endl;
			status.encryption_enabled = true;
			break;

		case wivrn_connection::encryption_state::pairing: {
			std::random_device rd;
			std::mt19937 gen(rd());
			std::uniform_int_distribution<> distrib(0, 999999);

			char buffer[7];
			snprintf(buffer, 7, "%06d", distrib(gen));
			pin = buffer;
			status.pin = pin;
			status.pairing_enabled = true;
			status.encryption_enabled = true;
			std::cerr << "To pair a new headset use PIN code: " << pin << std::endl;
			ui->show_pin(pin);
			break;
		}
	}

	enc_state = new_enc_state;
	ui->apply(status);
}

void on_headset_info_packet(const wivrn::from_headset::headset_info_packet & info)
{
	status.available_refresh_rates = info.available_refresh_rates;
	status.preferred_refresh_rate = info.settings.preferred_refresh_rate;
	status.bitrate = info.settings.bitrate_bps;

	auto speaker = info.speaker.value_or(wivrn::from_headset::headset_info_packet::audio_description{});
	status.speaker_channels = speaker.num_channels;
	status.speaker_sample_rate = speaker.sample_rate;

	auto mic = info.microphone.value_or(wivrn::from_headset::headset_info_packet::audio_description{});
	status.mic_channels = mic.num_channels;
	status.mic_sample_rate = mic.sample_rate;

	status.field_of_view.assign(std::begin(info.fov), std::end(info.fov));
	status.hand_tracking = info.hand_tracking;
	status.eye_gaze = info.eye_gaze;
	status.face_tracking = info.face_tracking != wivrn::from_headset::face_type::none;
	status.supported_codecs = info.supported_codecs;
	status.system_name = info.system_name;
}

void expose_known_keys()
{
	status.known_keys = known_keys();
	ui->apply(status);
}

void update_adb_status()
{
	if (not adb_watcher or not ui)
		return;

	auto snapshot = adb_watcher->get();
	status.adb_enabled = snapshot.enabled;
	status.adb_available = snapshot.available;
	status.adb_message = snapshot.message;
	status.adb_devices = std::move(snapshot.devices);
	ui->apply(status);
}

void on_name_acquired()
{
	children = ui->make_children_manager(update_fsm);

	on_headset_info_packet({});
	status.steam_command = steam_command();
	status.known_keys = known_keys();
	status.client_tab = "";

	if (enc_state != wivrn_connection::encryption_state::disabled and known_keys().empty())
		set_encryption_state(wivrn_connection::encryption_state::pairing);
	else
		set_encryption_state(enc_state);

	ui->apply(status);
}

ui_callbacks make_callbacks()
{
	ui_callbacks callbacks;

	callbacks.disconnect = [] {
		wivrn_ipc_socket_main_loop->send(to_monado::disconnect{});
	};

	callbacks.quit = [] {
		quitting_main_loop = true;
		update_fsm();
	};

	callbacks.revoke_key = [](std::string key) {
		wivrn::remove_known_key(key);
		expose_known_keys();
	};

	callbacks.rename_key = [](std::string key, std::string name) {
		wivrn::rename_known_key({.public_key = std::move(key), .name = std::move(name)});
		expose_known_keys();
	};

	callbacks.enable_pairing = [](int timeout_seconds) -> std::string {
		if (server_running())
		{
			std::cerr << "Cannot enable pairing while session is active." << std::endl;
			return {};
		}

		set_encryption_state(wivrn_connection::encryption_state::pairing);

		if (pairing_timeout)
			loop->remove_source(pairing_timeout);

		if (timeout_seconds > 0)
		{
			pairing_timeout = loop->add_timeout(timeout_seconds * 1000, [] {
				pairing_timeout = 0;
				set_encryption_state(wivrn_connection::encryption_state::enabled);
			});
		}

		return pin;
	};

	callbacks.disable_pairing = [] {
		set_encryption_state(wivrn_connection::encryption_state::enabled);
	};

	callbacks.set_client_tab = [](std::string tab) {
		for (auto [val, name]: magic_enum::enum_entries<stream_tab>())
		{
			if (tab == name)
			{
				wivrn_ipc_socket_main_loop->send(to_headset::stream_tab_change{.tab = val});
				return;
			}
		}
		std::cerr << "Invalid tab name " << tab << std::endl;
	};

	callbacks.json_configuration_changed = [](std::string json) {
		std::filesystem::path config = configuration::get_config_file();
		std::filesystem::path config_new = config;
		config_new += ".new";

		{
			std::ofstream file(config_new);
			file.write(json.data(), json.size());
		}

		std::error_code ec;
		std::filesystem::rename(config_new, config, ec);
		if (ec)
			std::cerr << "Failed to save configuration: " << ec.message() << std::endl;

		status.steam_command = steam_command();
		ui->apply(status);
	};

	callbacks.bitrate_changed = [](int bitrate) {
		status.bitrate = bitrate;
		if (bitrate > 0)
			wivrn_ipc_socket_main_loop->send(to_monado::set_bitrate{(uint32_t)bitrate});
	};

	callbacks.adb_connect = [] {
		if (adb_watcher)
			adb_watcher->connect();
	};

	callbacks.adb_refresh = [] {
		if (adb_watcher)
			adb_watcher->refresh();
	};

	return callbacks;
}

void poll_shutdown()
{
	if (platform::shutdown_requested())
	{
		std::cerr << "Shutting down" << std::endl;
		quitting_main_loop = true;
		update_fsm();
		return;
	}

	loop->add_timeout(100, poll_shutdown);
}

} // namespace

int inner_main(int argc, char * argv[], bool show_instructions)
{
	std::cerr << "WiVRn " << wivrn::display_version() << " starting" << std::endl;
	if (show_instructions)
	{
		if (auto command = steam_command(); not command.empty())
			std::cerr << "For Steam games, set command to " << command << std::endl;
	}

	std::filesystem::create_directories(ipc_socket_path().parent_path());

	if (do_active_runtime)
		active_runtime::cleanup_openxr();
	listen_socket = create_ipc_listen_socket();

	loop = make_event_loop();
	platform::install_shutdown_handler();

	auto [main_fd, monado_fd] = platform::create_socket_pair();
	control_pipe_fds[0] = main_fd;
	control_pipe_fds[1] = monado_fd;
	platform::set_close_on_exec(control_pipe_fds[0]);
	platform::set_close_on_exec(control_pipe_fds[1]);
	wivrn_ipc_socket_main_loop.emplace(control_pipe_fds[0]);
	wivrn_ipc_socket_monado.emplace(control_pipe_fds[1]);

	loop->add_fd(control_pipe_fds[0], control_received);

	std::string json_configuration;
	try
	{
		json_configuration = configuration::read_configuration().dump();
	}
	catch (std::exception & e)
	{
		std::cerr << "Invalid configuration: " << e.what() << std::endl;
	}
	if (publication != wivrn::service_publication::none)
		start_publishing();
	start_listening();

	ui = make_server_ui(loop.get());
	ui->start(status, json_configuration, make_callbacks(), on_name_acquired);
	ui->apply(status);

	if (use_adb)
	{
		status.adb_enabled = true;
		adb_watcher = std::make_unique<adb::service>();
		adb_watcher->start(configuration().port, [] {
			if (loop)
				loop->invoke(update_adb_status);
		});
		update_adb_status();
	}

	loop->add_timeout(100, poll_shutdown);
	loop->run();

	stop_publishing();
	stop_listening();
	if (adb_watcher)
	{
		adb_watcher->stop();
		adb_watcher.reset();
	}
	children.reset();
	ui.reset();

	std::error_code ec;
	std::filesystem::remove(ipc_socket_path(), ec);

	return wivrn_exit_code::success;
}

int main(int argc, char * argv[])
{
	CLI::App app;

	std::string config_file;
	app.add_option("-f", config_file, "configuration file")->option_text("FILE")->check(CLI::ExistingFile);
	auto version_flag = app.add_flag("--version")->description("print version and exit");
	auto no_active_runtime = app.add_flag("--no-manage-active-runtime")->description("don't set the active runtime on connection");
	auto early_active_runtime = app.add_flag("--early-active-runtime")->description("forcibly manages the active runtime even if no headset present");
	auto no_instructions = app.add_flag("--no-instructions")->group("");
	auto no_fork = app.add_flag("--no-fork")->description("disable fork to serve connection")->group("Debug");
	auto no_publish = app.add_flag("--no-publish-service")->description("disable publishing the service through DNS-SD");
	auto no_adb = app.add_flag("--no-adb")->description("don't forward the server port to USB-connected headsets with adb");
	auto no_encrypt = app.add_flag("--no-encrypt")->description("disable encryption")->group("Debug");
#if WIVRN_FEATURE_DEBUG_GUI
	auto debug_gui = app.add_flag("--debug-gui")->description("show the debug GUI")->group("Debug");
#endif

	CLI11_PARSE(app, argc, argv);

	if (*version_flag)
	{
		std::cout << "WiVRn version " << wivrn::display_version() << std::endl;
		return 0;
	}

	if (not config_file.empty())
		configuration::set_config_file(config_file);

	if (*early_active_runtime)
	{
		do_active_runtime = false;
		runtime_setter.emplace();
	}
	else
		do_active_runtime = not *no_active_runtime;

	do_fork = not *no_fork;
	if (*no_encrypt)
		enc_state = wivrn_connection::encryption_state::disabled;

	if (*no_publish)
		publication = wivrn::service_publication::none;
	else
		publication = configuration().publication;

	use_adb = not *no_adb;

#if WIVRN_FEATURE_DEBUG_GUI
	show_debug_gui = bool(*debug_gui);
#endif

	try
	{
		return inner_main(argc, argv, not *no_instructions);
	}
	catch (std::system_error & e)
	{
		std::cerr << e.what() << std::endl;
	}
	catch (std::exception & e)
	{
		std::cerr << e.what() << std::endl;
	}

	return wivrn_exit_code::unknown_error;
}
