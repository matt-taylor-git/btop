// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace WindowsCpu {

#if defined(_WIN32)
	template <typename Fn>
	inline Fn kernel_function(const char* name) {
		const FARPROC proc = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
		Fn fn{};
		static_assert(sizeof(fn) == sizeof(proc));
		std::memcpy(&fn, &proc, sizeof(fn));
		return fn;
	}

	inline DWORD logical_processor_count() {
		using GetActiveProcessorCountFn = DWORD (WINAPI *)(WORD);
		constexpr WORD all_processor_groups = 0xffff;
		if (const auto get_count = kernel_function<GetActiveProcessorCountFn>("GetActiveProcessorCount")) {
			if (const DWORD all_groups = get_count(all_processor_groups); all_groups > 0) return all_groups;
		}
		SYSTEM_INFO info{};
		GetNativeSystemInfo(&info);
		return std::max<DWORD>(1, info.dwNumberOfProcessors);
	}

	inline WORD processor_group_count() {
		using GetActiveProcessorGroupCountFn = WORD (WINAPI *)();
		if (const auto get_count = kernel_function<GetActiveProcessorGroupCountFn>("GetActiveProcessorGroupCount")) {
			if (const WORD groups = get_count(); groups > 0) return groups;
		}
		return 1;
	}
#endif

	inline std::array<double, 3> smooth_queue_averages(
		std::array<double, 3> current, const double queue_length, const double elapsed_seconds) {
		const double sample = std::max(0.0, queue_length);
		const double elapsed = std::max(0.0, elapsed_seconds);
		constexpr std::array<double, 3> windows = {60.0, 300.0, 900.0};
		for (std::size_t i = 0; i < windows.size(); ++i) {
			const double alpha = 1.0 - std::exp(-elapsed / windows.at(i));
			current.at(i) += alpha * (sample - current.at(i));
		}
		return current;
	}

}
