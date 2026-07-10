// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>

namespace WindowsGpu {

	struct memory_sample {
		std::uint64_t total{};
		std::uint64_t used{};
		bool shared{};
		bool available{};
	};

	inline memory_sample select_memory(const std::uint64_t dedicated_total, const std::uint64_t shared_total,
		const long long dedicated_used, const long long shared_used) {
		const bool use_shared = dedicated_total == 0 and shared_total > 0;
		const std::uint64_t total = use_shared ? shared_total : dedicated_total;
		const long long selected_used = use_shared ? shared_used : dedicated_used;
		const bool available = total > 0 and selected_used >= 0;
		return {
			total,
			available and selected_used > 0 ? std::min<std::uint64_t>(static_cast<std::uint64_t>(selected_used), total) : 0,
			use_shared,
			available,
		};
	}

}
