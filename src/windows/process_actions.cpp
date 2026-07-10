// SPDX-License-Identifier: Apache-2.0

#include "../btop_process_actions.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <windows.h>

#include "signal_compat.hpp"

namespace ProcessActions {
namespace {

constexpr DWORD process_suspend_resume = 0x0800;
constexpr std::array actions = {
	action {SIGTERM, "Terminate (SIGTERM)"},
	action {SIGKILL, "Kill (SIGKILL)"},
	action {SIGSTOP, "Suspend (SIGSTOP)"},
	action {SIGCONT, "Resume (SIGCONT)"},
};

auto last_error_to_errno(const DWORD error) -> int {
	if (error == ERROR_INVALID_PARAMETER or error == ERROR_NOT_FOUND) return ESRCH;
	return EPERM;
}

auto ntstatus_to_errno(const LONG status) -> int {
	constexpr LONG status_access_denied = static_cast<LONG>(0xC0000022);
	constexpr LONG status_invalid_cid = static_cast<LONG>(0xC000000B);
	constexpr LONG status_process_is_terminating = static_cast<LONG>(0xC000010A);
	if (status == status_invalid_cid or status == status_process_is_terminating) return ESRCH;
	if (status == status_access_denied) return EPERM;
	return EPERM;
}

template <typename Fn>
auto ntdll_function(const char* name) -> Fn {
	const FARPROC proc = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name);
	Fn fn{};
	static_assert(sizeof(fn) == sizeof(proc));
	std::memcpy(&fn, &proc, sizeof(fn));
	return fn;
}

auto suspend_or_resume(const int pid, const bool resume) -> int {
	using NtProcessAction = LONG (WINAPI *)(HANDLE);
	const auto operation = ntdll_function<NtProcessAction>(resume ? "NtResumeProcess" : "NtSuspendProcess");
	if (operation == nullptr) {
		errno = EINVAL;
		return -1;
	}

	HANDLE process = OpenProcess(process_suspend_resume | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (process == nullptr) {
		errno = last_error_to_errno(GetLastError());
		return -1;
	}
	const LONG status = operation(process);
	CloseHandle(process);
	if (status < 0) {
		errno = ntstatus_to_errno(status);
		return -1;
	}
	return 0;
}

} // namespace

auto supported() -> std::span<const action> { return actions; }

auto label(const int signal) -> std::string_view {
	for (const auto& item : actions) {
		if (item.signal == signal) return item.label;
	}
	return "Unsupported";
}

auto send(const int pid, const int signal) -> int {
	if (signal == 0) {
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
		if (process == nullptr) {
			errno = last_error_to_errno(GetLastError());
			return -1;
		}
		CloseHandle(process);
		return 0;
	}
	if (signal == SIGSTOP) return suspend_or_resume(pid, false);
	if (signal == SIGCONT) return suspend_or_resume(pid, true);
	if (signal != SIGTERM and signal != SIGKILL) {
		errno = EINVAL;
		return -1;
	}

	HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
	if (process == nullptr) {
		errno = last_error_to_errno(GetLastError());
		return -1;
	}
	const bool ok = TerminateProcess(process, static_cast<UINT>(signal)) != 0;
	const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
	CloseHandle(process);
	if (not ok) {
		errno = last_error_to_errno(error);
		return -1;
	}
	return 0;
}

} // namespace ProcessActions
