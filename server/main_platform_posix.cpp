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

#include "avahi_publisher.h"
#include "start_systemd_unit.h"
#include "utils/overloaded.h"
#include "wivrn_server_dbus.h"

#include <avahi-glib/glib-watch.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <libnotify/notify.h>
#include <util/u_file.h>
#include <xrt/xrt_config_build.h>

#include <shared/ipc_protocol.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

namespace wivrn
{

namespace
{

class event_loop_posix;

class publisher_posix : public service_publisher
{
public:
	publisher_posix(const AvahiPoll * poll_api,
	                const std::string & name,
	                const std::string & type,
	                int port,
	                const std::map<std::string, std::string> & txt) :
	        publisher(poll_api, name, type, port, txt)
	{}

private:
	avahi_publisher publisher;
};

struct invoke_data
{
	std::function<void()> callback;
};

gboolean run_invoked(gpointer data)
{
	auto * d = static_cast<invoke_data *>(data);
	d->callback();
	delete d;
	return G_SOURCE_REMOVE;
}

gboolean fd_source_callback(gint, GIOCondition, gpointer user_data)
{
	static_cast<invoke_data *>(user_data)->callback();
	return G_SOURCE_CONTINUE;
}

class event_loop_posix : public event_loop
{
	GMainLoop * loop;
	AvahiGLibPoll * avahi_poll;

public:
	event_loop_posix()
	{
		loop = g_main_loop_new(nullptr, false);
		avahi_poll = avahi_glib_poll_new(g_main_loop_get_context(loop), G_PRIORITY_DEFAULT);
	}

	~event_loop_posix() override
	{
		if (avahi_poll)
			avahi_glib_poll_free(avahi_poll);
		if (loop)
			g_main_loop_unref(loop);
	}

	void run() override { g_main_loop_run(loop); }
	void quit() override { g_main_loop_quit(loop); }

	void invoke(std::function<void()> callback) override
	{
		g_main_context_invoke(nullptr, run_invoked, new invoke_data{std::move(callback)});
	}

	source_id add_fd(int fd, std::function<void()> callback) override
	{
		auto * data = new invoke_data{std::move(callback)};
		GSource * source = g_unix_fd_source_new(fd, G_IO_IN);
		g_source_set_callback(
		        source,
		        G_SOURCE_FUNC(&fd_source_callback),
		        data,
		        [](gpointer user_data) { delete static_cast<invoke_data *>(user_data); });
		source_id id = g_source_attach(source, nullptr);
		g_source_unref(source);
		return id;
	}

	source_id add_timeout(int milliseconds, std::function<void()> callback) override
	{
		auto * data = new invoke_data{std::move(callback)};
		return g_timeout_add_full(
		        G_PRIORITY_DEFAULT,
		        milliseconds,
		        [](gpointer user_data) -> gboolean {
			        static_cast<invoke_data *>(user_data)->callback();
			        return G_SOURCE_REMOVE;
		        },
		        data,
		        [](gpointer user_data) { delete static_cast<invoke_data *>(user_data); });
	}

	source_id add_process_watch(process_id pid, std::function<void(int)> callback) override
	{
		struct watch_data
		{
			std::function<void(int)> callback;
		};
		auto * data = new watch_data{std::move(callback)};
		return g_child_watch_add_full(
		        G_PRIORITY_DEFAULT,
		        pid,
		        [](pid_t, int status, gpointer user_data) {
			        auto * watch = static_cast<watch_data *>(user_data);
			        watch->callback(status);
		        },
		        data,
		        [](gpointer user_data) { delete static_cast<watch_data *>(user_data); });
	}

	void remove_source(source_id id) override
	{
		if (id)
			g_source_remove(id);
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
		return std::make_unique<publisher_posix>(avahi_glib_poll_get(avahi_poll), name, type, port, txt);
	}
};

class server_ui_posix : public server_ui
{
	GDBusConnection * connection = nullptr;
	WivrnServer * dbus_server = nullptr;
	ui_callbacks callbacks;
	NotifyNotification * pin_notification = nullptr;
	std::string json_configuration;
	int bitrate = 0;
	std::function<void()> on_ready;

	static GDBusConnection * create_dbus_connection()
	{
		GError * error = nullptr;
		if (const char * bus_address = getenv("DBUS_SESSION_BUS_ADDRESS"))
		{
			auto connection = g_dbus_connection_new_for_address_sync(
			        bus_address,
			        GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION | G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
			        nullptr,
			        nullptr,
			        &error);
			if (error)
			{
				auto msg = std::string("Failed to connect to dbus at ") + bus_address + ": " + error->message;
				g_error_free(error);
				throw std::runtime_error(msg);
			}
			return connection;
		}
		auto connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
		if (error)
		{
			auto msg = std::string("Failed to connect to session bus: ") + error->message;
			g_error_free(error);
			throw std::runtime_error(msg);
		}
		return connection;
	}

	static void on_name_acquired(GDBusConnection *, const gchar *, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);

		g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(self->dbus_server),
		                                 self->connection,
		                                 "/io/github/wivrn/Server",
		                                 nullptr);

		if (not self->json_configuration.empty())
			wivrn_server_set_json_configuration(self->dbus_server, self->json_configuration.c_str());
		if (self->bitrate > 0)
			wivrn_server_set_bitrate(self->dbus_server, self->bitrate);

		auto ready = std::move(self->on_ready);
		if (ready)
			ready();
	}

	static gboolean handle_disconnect(WivrnServer *, GDBusMethodInvocation * invocation, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		if (self->callbacks.disconnect)
			self->callbacks.disconnect();
		g_dbus_method_invocation_return_value(invocation, nullptr);
		return G_SOURCE_CONTINUE;
	}

	static gboolean handle_quit(WivrnServer *, GDBusMethodInvocation * invocation, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		if (self->callbacks.quit)
			self->callbacks.quit();
		g_dbus_method_invocation_return_value(invocation, nullptr);
		return G_SOURCE_CONTINUE;
	}

	static gboolean handle_revoke_key(WivrnServer *, GDBusMethodInvocation * invocation, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		GVariant * args = g_dbus_method_invocation_get_parameters(invocation);
		char * public_key = nullptr;
		g_variant_get_child(args, 0, "s", &public_key);
		if (self->callbacks.revoke_key)
			self->callbacks.revoke_key(public_key);
		g_free(public_key);
		g_dbus_method_invocation_return_value(invocation, nullptr);
		return G_SOURCE_CONTINUE;
	}

	static gboolean handle_rename_key(WivrnServer *, GDBusMethodInvocation * invocation, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		GVariant * args = g_dbus_method_invocation_get_parameters(invocation);
		char * public_key = nullptr;
		char * name = nullptr;
		g_variant_get_child(args, 0, "s", &public_key);
		g_variant_get_child(args, 1, "s", &name);
		if (self->callbacks.rename_key)
			self->callbacks.rename_key(public_key, name);
		g_free(public_key);
		g_free(name);
		g_dbus_method_invocation_return_value(invocation, nullptr);
		return G_SOURCE_CONTINUE;
	}

	static gboolean handle_enable_pairing(WivrnServer *, GDBusMethodInvocation * invocation, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		int timeout = 0;
		GVariant * args = g_dbus_method_invocation_get_parameters(invocation);
		g_variant_get_child(args, 0, "i", &timeout);

		std::string pin;
		if (self->callbacks.enable_pairing)
			pin = self->callbacks.enable_pairing(timeout);

		g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", pin.c_str()));
		return G_SOURCE_CONTINUE;
	}

	static gboolean handle_disable_pairing(WivrnServer *, GDBusMethodInvocation * invocation, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		if (self->callbacks.disable_pairing)
			self->callbacks.disable_pairing();
		g_dbus_method_invocation_return_value(invocation, nullptr);
		return G_SOURCE_CONTINUE;
	}

	static gboolean handle_set_client_tab(WivrnServer *, GDBusMethodInvocation * invocation, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		GVariant * args = g_dbus_method_invocation_get_parameters(invocation);
		char * tab = nullptr;
		g_variant_get_child(args, 0, "s", &tab);
		if (self->callbacks.set_client_tab)
			self->callbacks.set_client_tab(tab);
		g_free(tab);
		g_dbus_method_invocation_return_value(invocation, nullptr);
		return G_SOURCE_CONTINUE;
	}

	static void on_json_configuration(WivrnServer * server, const GParamSpec *, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		const char * json = wivrn_server_get_json_configuration(server);
		if (json and self->json_configuration != json)
		{
			self->json_configuration = json;
			if (self->callbacks.json_configuration_changed)
				self->callbacks.json_configuration_changed(json);
		}
	}

	static void on_bitrate(WivrnServer * server, const GParamSpec *, gpointer user_data)
	{
		auto * self = static_cast<server_ui_posix *>(user_data);
		int value = wivrn_server_get_bitrate(server);
		if (value != self->bitrate)
		{
			self->bitrate = value;
			if (self->callbacks.bitrate_changed)
				self->callbacks.bitrate_changed(value);
		}
	}

public:
	~server_ui_posix() override
	{
		clear_pin();
		if (dbus_server)
		{
			g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(dbus_server));
			g_object_unref(dbus_server);
		}
		if (connection)
			g_object_unref(connection);
	}

	void start(const server_status &, const std::string & initial_json, ui_callbacks cb, std::function<void()> ready) override
	{
		callbacks = std::move(cb);
		json_configuration = initial_json;
		on_ready = std::move(ready);

		connection = create_dbus_connection();
		dbus_server = wivrn_server_skeleton_new();

		g_signal_connect(dbus_server, "handle-disconnect", G_CALLBACK(&handle_disconnect), this);
		g_signal_connect(dbus_server, "handle-quit", G_CALLBACK(&handle_quit), this);
		g_signal_connect(dbus_server, "handle-revoke-key", G_CALLBACK(&handle_revoke_key), this);
		g_signal_connect(dbus_server, "handle-rename-key", G_CALLBACK(&handle_rename_key), this);
		g_signal_connect(dbus_server, "handle-enable-pairing", G_CALLBACK(&handle_enable_pairing), this);
		g_signal_connect(dbus_server, "handle-disable-pairing", G_CALLBACK(&handle_disable_pairing), this);
		g_signal_connect(dbus_server, "handle-set-client-tab", G_CALLBACK(&handle_set_client_tab), this);
		g_signal_connect(dbus_server, "notify::json-configuration", G_CALLBACK(&on_json_configuration), this);
		g_signal_connect(dbus_server, "notify::bitrate", G_CALLBACK(&on_bitrate), this);

		g_bus_own_name_on_connection(connection,
		                             "io.github.wivrn.Server",
		                             G_BUS_NAME_OWNER_FLAGS_NONE,
		                             &on_name_acquired,
		                             nullptr,
		                             this,
		                             nullptr);
	}

	void apply(const server_status & status) override
	{
		if (not dbus_server)
			return;

		wivrn_server_set_headset_connected(dbus_server, status.headset_connected);
		wivrn_server_set_session_running(dbus_server, status.session_running);
		wivrn_server_set_pairing_enabled(dbus_server, status.pairing_enabled);
		wivrn_server_set_encryption_enabled(dbus_server, status.encryption_enabled);
		wivrn_server_set_pin(dbus_server, status.pin.c_str());

		GVariantBuilder * rates = g_variant_builder_new(G_VARIANT_TYPE("ad"));
		for (double rate: status.available_refresh_rates)
			g_variant_builder_add(rates, "d", rate);
		GVariant * rates_value = g_variant_new("ad", rates);
		wivrn_server_set_available_refresh_rates(dbus_server, rates_value);

		wivrn_server_set_preferred_refresh_rate(dbus_server, status.preferred_refresh_rate);
		bitrate = status.bitrate;
		wivrn_server_set_bitrate(dbus_server, status.bitrate);

		wivrn_server_set_speaker_channels(dbus_server, status.speaker_channels);
		wivrn_server_set_speaker_sample_rate(dbus_server, status.speaker_sample_rate);
		wivrn_server_set_mic_channels(dbus_server, status.mic_channels);
		wivrn_server_set_mic_sample_rate(dbus_server, status.mic_sample_rate);

		GVariantBuilder * fovs = g_variant_builder_new(G_VARIANT_TYPE("a(dddd)"));
		for (const XrFovf & fov: status.field_of_view)
			g_variant_builder_add(fovs, "(dddd)", fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown);
		GVariant * fov_value = g_variant_new("a(dddd)", fovs);
		wivrn_server_set_field_of_view(dbus_server, fov_value);

		wivrn_server_set_hand_tracking(dbus_server, status.hand_tracking);
		wivrn_server_set_eye_gaze(dbus_server, status.eye_gaze);
		wivrn_server_set_face_tracking(dbus_server, status.face_tracking);

		static const std::map<video_codec, const char *> codec_names = {
		        {h264, "h264"},
		        {h265, "h265"},
		        {av1, "av1"}};
		std::vector<const char *> codecs;
		for (video_codec codec: status.supported_codecs)
		{
			auto it = codec_names.find(codec);
			if (it != codec_names.end())
				codecs.push_back(it->second);
		}
		codecs.push_back(nullptr);
		wivrn_server_set_supported_codecs(dbus_server, codecs.data());

		wivrn_server_set_system_name(dbus_server, status.system_name.c_str());
		wivrn_server_set_client_tab(dbus_server, status.client_tab.c_str());
		wivrn_server_set_steam_command(dbus_server, status.steam_command.c_str());

		GVariantBuilder * keys = g_variant_builder_new(G_VARIANT_TYPE("a(ssx)"));
		for (const auto & key: status.known_keys)
		{
			g_variant_builder_add(keys,
			                      "(ssx)",
			                      key.name.c_str(),
			                      key.public_key.c_str(),
			                      key.last_connection ? std::chrono::duration_cast<std::chrono::seconds>((*key.last_connection).time_since_epoch()).count() : 0);
		}
		GVariant * keys_value = g_variant_new("a(ssx)", keys);
		g_variant_builder_unref(keys);
		wivrn_server_set_known_keys(dbus_server, keys_value);
	}

	void emit_error(const std::string & where, const std::string & message) override
	{
		if (dbus_server)
			wivrn_server_emit_server_error(dbus_server, where.c_str(), message.c_str());
	}

	void show_pin(const std::string & pin) override
	{
		clear_pin();
		pin_notification = notify_notification_new("PIN", pin.c_str(), "dialog-password");
		notify_notification_set_timeout(pin_notification, NOTIFY_EXPIRES_NEVER);
		notify_notification_show(pin_notification, nullptr);
	}

	void clear_pin() override
	{
		if (pin_notification)
		{
			notify_notification_close(pin_notification, nullptr);
			g_object_unref(G_OBJECT(pin_notification));
			pin_notification = nullptr;
		}
	}

	std::unique_ptr<children_manager> make_children_manager(std::function<void()> state_changed) override
	{
		if (connection)
		{
			try
			{
				return std::make_unique<systemd_units_manager>(connection, state_changed);
			}
			catch (...)
			{
			}
		}
		return std::make_unique<forked_children>(std::move(state_changed));
	}
};

} // namespace

std::unique_ptr<server_ui> make_server_ui()
{
	return std::make_unique<server_ui_posix>();
}

std::unique_ptr<event_loop> make_event_loop()
{
	return std::make_unique<event_loop_posix>();
}

std::filesystem::path ipc_socket_path()
{
	char sock_file[PATH_MAX];
	size_t size = u_file_get_path_in_runtime_dir(XRT_IPC_MSG_SOCK_FILENAME, sock_file, PATH_MAX);
	return {sock_file, sock_file + size};
}

int create_ipc_listen_socket()
{
	sockaddr_un addr{};
	auto sock_file = ipc_socket_path();

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, sock_file.c_str());

	int fd = socket(PF_UNIX, SOCK_STREAM, 0);
	platform::set_close_on_exec(fd);
	int ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));

	if (ret < 0 && errno == EADDRINUSE)
	{
		std::cerr << "Removing stale socket file " << sock_file << std::endl;
		ret = unlink(sock_file.c_str());
		if (ret < 0)
		{
			std::cerr << "Failed to remove stale socket file " << sock_file << ": " << strerror(errno) << std::endl;
			platform::close_socket(fd);
			throw std::system_error(errno, std::system_category());
		}
		ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	}

	if (ret < 0)
	{
		std::cerr << "Could not bind socket to path " << sock_file << ": " << strerror(errno) << ". Is the service running already?" << std::endl;
		if (errno == EADDRINUSE)
			std::cerr << "If WiVRn is not running, delete " << sock_file << " before starting a new instance" << std::endl;
		platform::close_socket(fd);
		throw std::system_error(errno, std::system_category());
	}

	ret = listen(fd, IPC_MAX_CLIENTS);
	if (ret < 0)
	{
		platform::close_socket(fd);
		throw std::system_error(errno, std::system_category());
	}

	return fd;
}

} // namespace wivrn
