// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace WindowsNetwork {

inline auto trim_copy(const std::string& value) -> std::string {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

inline auto normalize_selector(const std::string& selector) -> std::string {
	auto normalized = trim_copy(selector);
	for (auto& ch : normalized) {
		if (ch >= 'A' and ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
	}
	return normalized;
}

inline auto base_adapter_name(const std::string& friendly_name, const std::string& alias,
		const std::string& description, const uint32_t interface_index) -> std::string {
	for (const auto* candidate : {&friendly_name, &alias, &description}) {
		if (auto name = trim_copy(*candidate); not name.empty()) return name;
	}
	return "if" + std::to_string(interface_index);
}

inline auto display_adapter_name(const std::string& base_name, const uint32_t interface_index,
		const bool duplicate) -> std::string {
	return duplicate ? base_name + " (" + std::to_string(interface_index) + ')' : base_name;
}

inline void register_selector(std::unordered_map<std::string, std::string>& selectors,
		const std::string& selector, const std::string& display_name) {
	const auto normalized = normalize_selector(selector);
	if (normalized.empty()) return;
	const auto [it, inserted] = selectors.try_emplace(normalized, display_name);
	if (not inserted and it->second != display_name) it->second.clear();
}

inline auto resolve_selector(const std::unordered_map<std::string, std::string>& selectors,
		const std::string& selector) -> std::string {
	const auto it = selectors.find(normalize_selector(selector));
	return it == selectors.end() ? std::string{} : it->second;
}

inline auto ipv4_rank(const std::string& address) -> int {
	if (address.empty() or address == "0.0.0.0") return 0;
	if (address.starts_with("169.254.")) return 1;
	return 2;
}

inline auto ipv6_rank(const std::string& address) -> int {
	const auto normalized = normalize_selector(address);
	if (normalized.empty() or normalized == "::" or normalized == "::1") return 0;
	if (normalized.size() >= 3 and normalized.starts_with("fe")
			and (normalized[2] == '8' or normalized[2] == '9' or normalized[2] == 'a' or normalized[2] == 'b')) return 1;
	return 2;
}

inline void prefer_address(std::string& current, const std::string& candidate, const bool ipv6) {
	const int current_rank = ipv6 ? ipv6_rank(current) : ipv4_rank(current);
	const int candidate_rank = ipv6 ? ipv6_rank(candidate) : ipv4_rank(candidate);
	if (candidate_rank > current_rank) current = candidate;
}

} // namespace WindowsNetwork
