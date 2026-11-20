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

#ifdef _WIN32
#include "utils/command_line.h"
#endif

#include <cassert>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <thread>

namespace wivrn
{

children_manager::~children_manager() {}

#ifndef _WIN32
/// POSIX-style unescaping, used for the .desktop exec strings on Linux.
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

#endif // !_WIN32

void children_manager::start_application(const std::string & exec, const std::optional<std::string> & path)
{
#ifdef _WIN32
	// Windows paths contain backslashes, which the POSIX unescaping above would
	// swallow; split with the Windows rules instead.
	start_application(split_windows_command_line(exec), path);
#else
	start_application(unescape_string(exec), path);
#endif
}

forked_children::forked_children(std::function<void()> state_changed_cb) :
        state_changed_cb(state_changed_cb) {}

void forked_children::start_application(const std::vector<std::string> & args, const std::optional<std::string> & path)
{
	if (args.empty())
		return;

	std::optional<std::filesystem::path> working_directory;
	if (path)
		working_directory = *path;

	std::cerr << "Launching " << args.front() << std::endl;
	std::cerr << "With args:" << std::endl;
	for (auto & i: args)
		std::cerr << "    " << std::quoted(i) << std::endl;

	process_id application_pid = platform::spawn_process(args, working_directory, true);

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
}

forked_children::~forked_children()
{
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
}

bool forked_children::running() const
{
	std::lock_guard lock(mutex);
	return not children.empty();
}

void forked_children::stop()
{
	std::lock_guard lock(mutex);
	for (auto & [pid, watches]: children)
	{
		(void)watches;
		platform::terminate_process_group(pid, true);
	}
}

void display_child_status(int wstatus, const std::string & name)
{
	std::cerr << name << " exited, exit status " << platform::process_exit_code(wstatus) << std::endl;
}

} // namespace wivrn
