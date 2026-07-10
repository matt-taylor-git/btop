// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace WindowsMemory {

struct physical_stats {
	uint64_t used{};
	uint64_t available{};
	uint64_t cached{};
	uint64_t free{};
};

struct pagefile_stats {
	uint64_t total{};
	uint64_t used{};
	uint64_t free{};
	size_t files{};
};

inline auto saturating_add(const uint64_t left, const uint64_t right) -> uint64_t {
	return right > std::numeric_limits<uint64_t>::max() - left ? std::numeric_limits<uint64_t>::max() : left + right;
}

inline auto physical(const uint64_t total, const uint64_t available, const uint64_t free,
		const uint64_t system_cache, const uint64_t standby_cache) -> physical_stats {
	const auto safe_available = std::min(available, total);
	return {
		total - safe_available,
		safe_available,
		std::min(std::max(system_cache, standby_cache), total),
		std::min(free, safe_available),
	};
}

inline auto pagefiles_from_mebibytes(const uint64_t allocated_mib, const uint64_t used_mib,
		const size_t files) -> pagefile_stats {
	constexpr uint64_t mib = 1ULL << 20;
	const uint64_t total = allocated_mib > std::numeric_limits<uint64_t>::max() / mib
		? std::numeric_limits<uint64_t>::max()
		: allocated_mib * mib;
	const uint64_t raw_used = used_mib > std::numeric_limits<uint64_t>::max() / mib
		? std::numeric_limits<uint64_t>::max()
		: used_mib * mib;
	const uint64_t used = std::min(raw_used, total);
	return {total, used, total - used, files};
}

inline auto estimated_pagefiles(const uint64_t commit_limit, const uint64_t commit_used,
		const uint64_t physical_total) -> pagefile_stats {
	const uint64_t total = commit_limit > physical_total ? commit_limit - physical_total : 0;
	const uint64_t used = std::min(commit_used > physical_total ? commit_used - physical_total : 0, total);
	return {total, used, total - used, total > 0 ? 1U : 0U};
}

} // namespace WindowsMemory
