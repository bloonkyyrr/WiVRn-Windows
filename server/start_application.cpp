/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "start_application.h"

#include "utils/flatpak.h"

#include <cassert>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <thread>

#if !defined(_WIN32)
#include <glib.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wivrn
{

children_manager::~children_manager() {}

static std::vector<std::string> unescape_string(const std::string & app_string)
{
	std::vector<std::string> app;
	app.emplace_back();

	bool seen_backslash = false;
	bool seen_single_quote = false;
	bool seen_double_quote = false;
	for (auto c: app_string)
	{
		if (seen_backslash)
		{
			app.back() += c;
			seen_backslash = false;
		}
		else if (seen_single_quote)
		{
			if (c == '\'')
				seen_single_quote = false;
			else if (c == '\\')
				seen_backslash = true;
			else
				app.back() += c;
		}
		else if (seen_double_quote)
		{
			if (c == '"')
				seen_double_quote = false;
			else if (c == '\\')
				seen_backslash = true;
			else
				app.back() += c;
		}
		else
		{
			switch (c)
			{
				case '\\':
					seen_backslash = true;
					break;
				case '\'':
					seen_single_quote = true;
					break;
				case '"':
					seen_double_quote = true;
					break;
				case ' ':
					if (app.back() != "")
						app.emplace_back();
					break;
				default:
					app.back() += c;
			}
		}
	}

	if (app.back() == "")
		app.pop_back();

	return app;
}

void children_manager::start_application(const std::string & exec, const std::optional<std::string> & path)
{
	start_application(unescape_string(exec), path);
}

forked_children::forked_children(std::function<void()> state_changed_cb) :
        state_changed_cb(state_changed_cb) {}

void forked_children::start_application(const std::vector<std::string> & args, const std::optional<std::string> & path)
{
	if (args.empty())
		return;

	std::vector<std::string> command;
	std::optional<std::filesystem::path> working_directory;

	if (wivrn::flatpak_key(wivrn::flatpak::section::session_bus_policy, "org.freedesktop.Flatpak") == "talk")
	{
		command.push_back("flatpak-spawn");
		command.push_back("--host");
		if (path)
			command.push_back("--directory=" + *path);
	}
	else if (path)
	{
		working_directory = *path;
	}

	command.insert(command.end(), args.begin(), args.end());

	std::cerr << "Launching " << command.front() << std::endl;
	std::cerr << "With args:" << std::endl;
	for (auto & i: command)
		std::cerr << "    " << std::quoted(i) << std::endl;

	process_id application_pid = platform::spawn_process(command, working_directory, true);

#if defined(_WIN32)
	{
		std::lock_guard lock(mutex);
		children.emplace(application_pid, watches{});
	}

	waiters.emplace_back([this, application_pid](std::stop_token) {
		int status = 0;
		if (platform::wait_process(application_pid, status))
			display_child_status(status, "Application");

		bool became_empty = false;
		{
			std::lock_guard lock(mutex);
			auto node = children.extract(application_pid);
			became_empty = not node.empty() and children.empty();
		}
		if (became_empty and state_changed_cb)
			state_changed_cb();
	});
#else
	guint app_watch = g_child_watch_add(
	        application_pid,
	        [](process_id pid, int status, void * self_) {
		        auto self = (forked_children *)self_;
		        bool was_empty;
		        bool is_empty;
		        unsigned int kill_watch = 0;
		        {
			        std::lock_guard lock(self->mutex);
			        was_empty = self->children.empty();
			        auto node = self->children.extract(pid);
			        if (not node)
			        {
				        std::cerr << "Failed to update child application information" << std::endl;
			        }
			        else
			        {
				        g_source_remove(node.mapped().child);
				        kill_watch = node.mapped().kill;
			        }
			        is_empty = self->children.empty();
		        }
		        display_child_status(status, "Application");
		        if (kill_watch)
			        g_source_remove(kill_watch);

		        if (self->state_changed_cb and is_empty and not was_empty)
			        self->state_changed_cb();
	        },
	        this);
	{
		std::lock_guard lock(mutex);
		children.emplace(application_pid, watches{.child = app_watch});
	}
#endif
}

forked_children::~forked_children()
{
#if defined(_WIN32)
	std::vector<process_id> pids;
	{
		std::lock_guard lock(mutex);
		state_changed_cb = nullptr;
		for (const auto & [pid, watches]: children)
		{
			(void)watches;
			pids.push_back(pid);
		}
	}
	for (process_id pid: pids)
		platform::terminate_process_group(pid, true);
	waiters.clear();
#else
	std::lock_guard lock(mutex);
	for (auto & [pid, watches]: children)
	{
		g_source_remove(watches.child);
		if (watches.kill)
			g_source_remove(watches.kill);
	}
#endif
}

bool forked_children::running() const
{
	std::lock_guard lock(mutex);
	return not children.empty();
}

void forked_children::stop()
{
#if defined(_WIN32)
	std::lock_guard lock(mutex);
	for (auto & [pid, watches]: children)
	{
		(void)watches;
		platform::terminate_process_group(pid, true);
	}
#else
	std::lock_guard lock(mutex);
	for (auto & [pid, watches]: children)
	{
		platform::terminate_process_group(pid, false);

		// Send SIGKILL after 1s if it is still running
		uintptr_t pid_ptr = pid;
		watches.kill = g_timeout_add(
		        1000,
		        [](void * pid_) {
			        auto pid = (process_id)(uintptr_t)pid_;
			        assert(pid > 0);
			        platform::terminate_process_group(pid, true);
			        return G_SOURCE_REMOVE;
		        },
		        (void *)pid_ptr);
	}
#endif
}

void display_child_status(int wstatus, const std::string & name)
{
	std::cerr << name << " exited, exit status " << platform::process_exit_code(wstatus);
#if !defined(_WIN32)
	if (platform::process_terminated_by_signal(wstatus))
	{
		std::cerr << ", received signal " << platform::process_signal_name(wstatus)
		          << (WCOREDUMP(wstatus) ? ", core dumped" : "");
	}
#endif
	std::cerr << std::endl;
}

} // namespace wivrn
