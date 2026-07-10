// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace WindowsSensors {
	struct sensor {
		std::string identifier;
		std::string parent;
		std::string name;
		std::string type;
		double value{};
	};

	struct cpu_sample {
		bool available{};
		std::optional<long long> package_temp;
		std::vector<std::pair<std::string, long long>> core_temps;
		std::optional<double> watts;
	};

	struct gpu_sample {
		uint32_t vendor_id{};
		int device_index{};
		std::optional<long long> temperature;
		std::optional<long long> power_mw;
		std::optional<unsigned int> gpu_clock_mhz;
		std::optional<unsigned int> memory_clock_mhz;
		std::optional<long long> utilization;
		std::optional<long long> memory_utilization;
	};

	inline std::string lower_copy(std::string value) {
		std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	}

	inline int core_number(const std::string_view name) {
		const auto marker = name.find('#');
		if (marker == std::string_view::npos) return std::numeric_limits<int>::max();
		int number = 0;
		bool found = false;
		for (size_t i = marker + 1; i < name.size() and name.at(i) >= '0' and name.at(i) <= '9'; ++i) {
			found = true;
			number = number * 10 + (name.at(i) - '0');
		}
		return found ? number : std::numeric_limits<int>::max();
	}

	inline cpu_sample aggregate_cpu(const std::vector<sensor>& sensors, const bool available) {
		cpu_sample sample;
		sample.available = available;
		int package_score = -1;
		int power_score = -1;
		for (const auto& item : sensors) {
			if (!std::isfinite(item.value)) continue;
			const auto identifier = lower_copy(item.identifier);
			const auto parent = lower_copy(item.parent);
			if (!identifier.starts_with("/intelcpu/") and !identifier.starts_with("/amdcpu/")
			and !parent.starts_with("/intelcpu/") and !parent.starts_with("/amdcpu/")) continue;

			const auto type = lower_copy(item.type);
			const auto name = lower_copy(item.name);
			if (type == "temperature" and item.value >= -20.0 and item.value <= 125.0) {
				const auto temperature = static_cast<long long>(std::round(item.value));
				if (name.contains("core #") and !name.contains("max") and !name.contains("average")) {
					sample.core_temps.emplace_back(item.name, temperature);
				}
				int score = 10;
				if (name.contains("package")) score = 100;
				else if (name.contains("tctl") or name.contains("tdie")) score = 90;
				else if (name.contains("core max")) score = 80;
				else if (name.contains("cpu")) score = 70;
				if (score > package_score or (score == package_score and (!sample.package_temp or temperature > *sample.package_temp))) {
					package_score = score;
					sample.package_temp = temperature;
				}
			}
			else if (type == "power" and item.value > 0.0 and item.value < 2000.0) {
				int score = 10;
				if (name.contains("package")) score = 100;
				else if (name.contains("total")) score = 90;
				else if (name.contains("cpu")) score = 80;
				if (score > power_score or (score == power_score and (!sample.watts or item.value > *sample.watts))) {
					power_score = score;
					sample.watts = item.value;
				}
			}
		}
		std::ranges::sort(sample.core_temps, [](const auto& left, const auto& right) {
			const int left_number = core_number(left.first);
			const int right_number = core_number(right.first);
			return left_number != right_number ? left_number < right_number : left.first < right.first;
		});
		return sample;
	}

	inline std::optional<std::pair<uint32_t, int>> gpu_vendor_and_index(const sensor& item) {
		const auto path = lower_copy(item.identifier + " " + item.parent);
		for (const auto& [marker, vendor] : std::vector<std::pair<std::string_view, uint32_t>>{
			{"/nvidiagpu/", 0x10DE}, {"/atigpu/", 0x1002}, {"/amdgpu/", 0x1002}, {"/intelgpu/", 0x8086}}) {
			const auto position = path.find(marker);
			if (position == std::string::npos) continue;
			const auto first = position + marker.size();
			int index = 0;
			bool found = false;
			for (size_t i = first; i < path.size() and path.at(i) >= '0' and path.at(i) <= '9'; ++i) {
				found = true;
				index = index * 10 + (path.at(i) - '0');
			}
			if (found) return std::pair<uint32_t, int>{vendor, index};
		}
		return std::nullopt;
	}

	inline std::vector<gpu_sample> aggregate_gpus(const std::vector<sensor>& sensors) {
		struct scored_sample {
			gpu_sample sample;
			int temperature_score = -1;
			int power_score = -1;
		};
		std::map<std::pair<uint32_t, int>, scored_sample> grouped;
		for (const auto& item : sensors) {
			if (!std::isfinite(item.value)) continue;
			const auto identity = gpu_vendor_and_index(item);
			if (!identity) continue;
			auto& target = grouped[*identity];
			target.sample.vendor_id = identity->first;
			target.sample.device_index = identity->second;
			const auto type = lower_copy(item.type);
			const auto name = lower_copy(item.name);

			if (type == "temperature" and item.value >= -20.0 and item.value <= 125.0) {
				int score = 10;
				if (name.contains("core")) score = 100;
				else if (name.contains("gpu") and !name.contains("hot")) score = 90;
				else if (name.contains("hot spot") or name.contains("hotspot")) score = 50;
				const auto temperature = static_cast<long long>(std::round(item.value));
				if (score > target.temperature_score or (score == target.temperature_score and (!target.sample.temperature or temperature > *target.sample.temperature))) {
					target.temperature_score = score;
					target.sample.temperature = temperature;
				}
			}
			else if (type == "power" and item.value > 0.0 and item.value < 2000.0) {
				int score = 10;
				if (name.contains("package") or name.contains("total") or name.contains("board")) score = 100;
				else if (name.contains("gpu")) score = 90;
				const auto power = static_cast<long long>(std::round(item.value * 1000.0));
				if (score > target.power_score or (score == target.power_score and (!target.sample.power_mw or power > *target.sample.power_mw))) {
					target.power_score = score;
					target.sample.power_mw = power;
				}
			}
			else if (type == "clock" and item.value > 0.0 and item.value < 100000.0) {
				const auto clock = static_cast<unsigned int>(std::round(item.value));
				if (name.contains("memory")) target.sample.memory_clock_mhz = clock;
				else if (name.contains("core") or name.contains("graphics") or name.contains("gpu")) target.sample.gpu_clock_mhz = clock;
			}
			else if (type == "load" and item.value >= 0.0 and item.value <= 100.0) {
				const auto load = static_cast<long long>(std::round(item.value));
				if (name.contains("memory")) target.sample.memory_utilization = load;
				else if (name.contains("core") or name.contains("3d") or name.contains("gpu")) {
					target.sample.utilization = std::max(target.sample.utilization.value_or(0), load);
				}
			}
		}

		std::vector<gpu_sample> result;
		result.reserve(grouped.size());
		for (auto& [_, item] : grouped) result.push_back(std::move(item.sample));
		return result;
	}
}
