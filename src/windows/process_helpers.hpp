// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace WindowsProcess {

#if defined(_WIN32)
inline auto priority_class_to_nice(const DWORD priority_class) -> int {
	switch (priority_class) {
		case REALTIME_PRIORITY_CLASS: return -20;
		case HIGH_PRIORITY_CLASS: return -15;
		case ABOVE_NORMAL_PRIORITY_CLASS: return -5;
		case BELOW_NORMAL_PRIORITY_CLASS: return 5;
		case IDLE_PRIORITY_CLASS: return 19;
		case NORMAL_PRIORITY_CLASS:
		default: return 0;
	}
}

inline auto nice_to_priority_class(const int nice) -> DWORD {
	if (nice <= -20) return REALTIME_PRIORITY_CLASS;
	if (nice <= -15) return HIGH_PRIORITY_CLASS;
	if (nice <= -5) return ABOVE_NORMAL_PRIORITY_CLASS;
	if (nice >= 15) return IDLE_PRIORITY_CLASS;
	if (nice >= 5) return BELOW_NORMAL_PRIORITY_CLASS;
	return NORMAL_PRIORITY_CLASS;
}
#endif

struct cpu_sample {
	double percent{};
	double cumulative_percent{};
	uint64_t cpu_milliseconds{};
	uint64_t start_uptime_seconds{};
};

inline auto start_uptime_seconds(const uint64_t create_time_100ns, const uint64_t now_time_100ns,
		const double uptime_seconds) -> uint64_t {
	const double age = now_time_100ns > create_time_100ns
		? static_cast<double>(now_time_100ns - create_time_100ns) / 10000000.0
		: 0.0;
	return static_cast<uint64_t>(uptime_seconds > age ? uptime_seconds - age : 0.0);
}

inline auto calculate_cpu(const uint64_t cpu_time_100ns, const uint64_t previous_cpu_time_100ns,
		const uint64_t system_delta_100ns, const uint64_t create_time_100ns, const uint64_t now_time_100ns,
		const double uptime_seconds, const bool per_core, const int core_count) -> cpu_sample {
	const auto safe_cores = std::max(1, core_count);
	const auto delta = cpu_time_100ns >= previous_cpu_time_100ns ? cpu_time_100ns - previous_cpu_time_100ns : 0;
	const double multiplier = per_core ? safe_cores : 1;
	const double percent = system_delta_100ns > 0
		? std::clamp(delta * 100.0 / system_delta_100ns * multiplier, 0.0, 100.0 * safe_cores)
		: 0.0;
	const auto started = start_uptime_seconds(create_time_100ns, now_time_100ns, uptime_seconds);
	const double age = std::max(1.0, uptime_seconds - static_cast<double>(started));
	const double cumulative = std::clamp((cpu_time_100ns / 10000000.0) * 100.0 / age, 0.0, 100.0 * safe_cores);
	return {percent, cumulative, cpu_time_100ns / 10000, started};
}

} // namespace WindowsProcess
