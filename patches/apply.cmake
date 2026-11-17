# Applies WiVRn's patch set to a FetchContent source directory.
#
# Windows-native replacement for the former patches/apply.sh, which needed a
# POSIX shell. Prefers `git apply` (always available: FetchContent needs git
# anyway) and falls back to `patch.exe`. Patches that are already applied are
# skipped so that re-running the configure step is harmless.
#
# Usage: -DPATCHES=<list of patch files>
#
# The working directory is the source tree to patch: FetchContent runs
# PATCH_COMMAND from there, and in `cmake -P` script mode
# CMAKE_CURRENT_SOURCE_DIR is that same directory.

if(NOT DEFINED PATCHES OR "${PATCHES}" STREQUAL "")
	message(FATAL_ERROR "apply.cmake: PATCHES is empty")
endif()

find_program(GIT_EXECUTABLE NAMES git git.exe)
find_program(PATCH_EXECUTABLE NAMES patch patch.exe)

set(source_dir "${CMAKE_CURRENT_SOURCE_DIR}")

set(is_repository FALSE)
if(EXISTS "${source_dir}/.git")
	set(is_repository TRUE)
endif()

# FetchContent reuses a cached source tree, which may be left partially patched
# by an earlier (possibly failed) run: a half-patched tree makes patches report
# "already applied" without changing anything, or apply with fuzz. Reset git
# checkouts to their pristine state first so every run starts from a clean tree.
if(is_repository AND GIT_EXECUTABLE)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" reset --hard
		WORKING_DIRECTORY "${source_dir}"
		OUTPUT_QUIET ERROR_QUIET)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" clean -fdx
		WORKING_DIRECTORY "${source_dir}"
		OUTPUT_QUIET ERROR_QUIET)
endif()

# Somewhere outside the source tree for normalised patch copies.
set(temp_dir "$ENV{TEMP}")
if("${temp_dir}" STREQUAL "")
	set(temp_dir "$ENV{TMP}")
endif()
if("${temp_dir}" STREQUAL "")
	set(temp_dir "${source_dir}")
endif()

foreach(patch_file IN LISTS PATCHES)
	if(NOT EXISTS "${patch_file}")
		message(FATAL_ERROR "apply.cmake: ${patch_file} does not exist")
	endif()

	# A patch stored with CRLF line endings (git core.autocrlf) never applies to
	# sources checked out with LF: the context lines do not match. git gets a
	# normalised copy; patch.exe strips the CRs by itself.
	get_filename_component(patch_name "${patch_file}" NAME)
	set(git_patch "${patch_file}")
	if(GIT_EXECUTABLE)
		file(READ "${patch_file}" patch_content)
		string(FIND "${patch_content}" "\r\n" crlf_position)
		if(NOT crlf_position EQUAL -1)
			string(REPLACE "\r\n" "\n" patch_content "${patch_content}")
			set(git_patch "${temp_dir}/wivrn-${patch_name}")
			file(WRITE "${git_patch}" "${patch_content}")
		endif()
	endif()

	set(applied FALSE)
	set(apply_error "")

	if(GIT_EXECUTABLE)
		# Three ways to apply, tried in order. The plain one is exact. The second
		# one covers sources checked out with CRLF line endings (git core.autocrlf
		# on Windows): there a LF patch matches no context line at all, and only a
		# git that knows about the conversion can apply it. The third one is a
		# three-way merge, for patches whose context an earlier patch of the set
		# rewrote; it needs a repository, tarball sources only accept a plain apply.
		set(attempts 2)
		if(is_repository)
			math(EXPR attempts "${attempts} + 1")
		endif()

		set(attempt 0)
		while(attempt LESS attempts AND NOT applied)
			set(attempt_args apply --whitespace=nowarn)
			if(attempt EQUAL 1)
				list(PREPEND attempt_args -c core.autocrlf=true)
			elseif(attempt EQUAL 2)
				list(APPEND attempt_args --3way)
			endif()

			execute_process(
				COMMAND "${GIT_EXECUTABLE}" ${attempt_args} "${git_patch}"
				WORKING_DIRECTORY "${source_dir}"
				RESULT_VARIABLE result
				OUTPUT_VARIABLE output
				ERROR_VARIABLE error_output)

			if(result EQUAL 0)
				set(applied TRUE)
			else()
				set(apply_error "${error_output}")
			endif()

			math(EXPR attempt "${attempt} + 1")
		endwhile()

		if(NOT applied)
			# Already applied patches are recognised in reverse; the CRLF variant is
			# needed for the same reason as above.
			set(reverse_attempt 0)
			while(reverse_attempt LESS 2 AND NOT applied)
				set(reverse_args apply --reverse --check)
				if(reverse_attempt EQUAL 1)
					list(PREPEND reverse_args -c core.autocrlf=true)
				endif()

				execute_process(
					COMMAND "${GIT_EXECUTABLE}" ${reverse_args} "${git_patch}"
					WORKING_DIRECTORY "${source_dir}"
					RESULT_VARIABLE reverse_result
					OUTPUT_QUIET
					ERROR_QUIET)
				if(reverse_result EQUAL 0)
					message(STATUS "apply.cmake: already applied: ${patch_name}")
					set(applied TRUE)
				endif()

				math(EXPR reverse_attempt "${reverse_attempt} + 1")
			endwhile()
		endif()
	endif()

	if(NOT applied AND PATCH_EXECUTABLE)
		execute_process(
			COMMAND "${PATCH_EXECUTABLE}" -p1 --forward "--input=${patch_file}"
			WORKING_DIRECTORY "${source_dir}"
			RESULT_VARIABLE result
			OUTPUT_VARIABLE output
			ERROR_VARIABLE error_output)
		if(result EQUAL 0)
			set(applied TRUE)
		elseif(result EQUAL 1)
			# `patch --forward` returns 1 for "already applied" as well as for
			# rejects, and says so on stdout.
			if("${output}${error_output}" MATCHES "Reversed")
				message(STATUS "apply.cmake: already applied: ${patch_name}")
				set(applied TRUE)
			endif()
		endif()
		if(NOT applied)
			set(apply_error "${output}${error_output}")
		endif()
	endif()

	# A patch that silently does not apply produces compiler errors in code that
	# looks unrelated to the patch, so stop here instead of building an unpatched
	# tree.
	if(NOT applied)
		message(FATAL_ERROR "apply.cmake: could not apply ${patch_name}: ${apply_error}")
	endif()
endforeach()
