// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace WindowsDisk {

enum class drive_kind {
	unknown,
	removable,
	fixed,
	network,
	optical,
	ramdisk,
};

inline auto drive_kind_from_code(const unsigned int type) -> drive_kind {
	switch (type) {
		case 2: return drive_kind::removable;
		case 3: return drive_kind::fixed;
		case 4: return drive_kind::network;
		case 5: return drive_kind::optical;
		case 6: return drive_kind::ramdisk;
		default: return drive_kind::unknown;
	}
}

inline auto type_name(const drive_kind kind) -> std::string {
	switch (kind) {
		case drive_kind::removable: return "removable";
		case drive_kind::fixed: return "fixed";
		case drive_kind::network: return "network";
		case drive_kind::optical: return "optical";
		case drive_kind::ramdisk: return "ramdisk";
		default: return "windows";
	}
}

inline auto allowed(const drive_kind kind, const bool only_physical) -> bool {
	if (kind == drive_kind::fixed or kind == drive_kind::removable) return true;
	return not only_physical and (kind == drive_kind::network or kind == drive_kind::optical or kind == drive_kind::ramdisk);
}

inline auto supports_io(const drive_kind kind) -> bool {
	return kind == drive_kind::fixed or kind == drive_kind::removable;
}

struct io_delta {
	long long read_bytes{};
	long long write_bytes{};
	long long activity{};
};

inline auto normalize_mount(std::string mount) -> std::string {
	const auto first = mount.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = mount.find_last_not_of(" \t\r\n");
	mount = mount.substr(first, last - first + 1);
	std::ranges::transform(mount, mount.begin(), [](const unsigned char character) {
		return static_cast<char>(std::toupper(character));
	});
	std::ranges::replace(mount, '/', '\\');
	if (mount.size() == 1 and std::isalpha(static_cast<unsigned char>(mount.front()))) mount += ":\\";
	else if (mount.size() == 2 and mount.at(1) == ':') mount += '\\';
	return mount;
}

inline auto filter_allows(
		const std::string& mount,
		const std::vector<std::string>& filter,
		const bool exclude) -> bool {
	if (filter.empty()) return true;
	const auto normalized_mount = normalize_mount(mount);
	const bool matched = std::ranges::any_of(filter, [&](const std::string& item) {
		return normalize_mount(item) == normalized_mount;
	});
	return exclude ? not matched : matched;
}

inline auto activity_from_cumulative(
		const std::int64_t query_time,
		const std::int64_t idle_time,
		const std::int64_t busy_time,
		std::int64_t& previous_query_time,
		std::int64_t& previous_idle_time,
		std::int64_t& previous_busy_time) -> long long {
	long long activity = 0;
	if (previous_query_time > 0 and query_time > previous_query_time) {
		const auto query_delta = query_time - previous_query_time;
		const auto idle_delta = idle_time - previous_idle_time;
		const auto busy_delta = busy_time - previous_busy_time;
		if ((idle_time > 0 or previous_idle_time > 0) and idle_delta >= 0 and idle_delta <= query_delta) {
			activity = std::clamp(
				static_cast<long long>(std::llround((query_delta - idle_delta) * 100.0 / query_delta)),
				0LL, 100LL);
		}
		else if ((busy_time > 0 or previous_busy_time > 0) and busy_delta >= 0) {
			activity = std::clamp(
				static_cast<long long>(std::llround(busy_delta * 100.0 / query_delta)),
				0LL, 100LL);
		}
	}

	previous_query_time = query_time;
	previous_idle_time = idle_time;
	previous_busy_time = busy_time;
	return activity;
}

inline auto pdh_rates_to_delta(
		const double read_bytes_per_second,
		const double write_bytes_per_second,
		const double idle_percent,
		const double elapsed_seconds) -> std::optional<io_delta> {
	if (not std::isfinite(read_bytes_per_second) or not std::isfinite(write_bytes_per_second) or
		not std::isfinite(idle_percent) or not std::isfinite(elapsed_seconds) or elapsed_seconds <= 0.0) {
		return std::nullopt;
	}

	const auto to_bytes = [elapsed_seconds](const double rate) {
		const auto bytes = std::max(0.0, rate) * elapsed_seconds;
		return static_cast<long long>(std::llround(std::min(
			bytes, static_cast<double>(std::numeric_limits<long long>::max()))));
	};

	return io_delta {
		.read_bytes = to_bytes(read_bytes_per_second),
		.write_bytes = to_bytes(write_bytes_per_second),
		.activity = std::clamp(
			static_cast<long long>(std::llround(100.0 - std::clamp(idle_percent, 0.0, 100.0))),
			0LL, 100LL),
	};
}

} // namespace WindowsDisk
