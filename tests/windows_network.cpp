// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "windows/network_helpers.hpp"

TEST(windows_network, normalizes_adapter_selectors) {
	EXPECT_EQ(WindowsNetwork::normalize_selector("  Wi-Fi  "), "wi-fi");
	EXPECT_EQ(WindowsNetwork::normalize_selector("IF12"), "if12");
}

TEST(windows_network, builds_stable_friendly_and_fallback_names) {
	EXPECT_EQ(WindowsNetwork::base_adapter_name(" Ethernet ", "alias", "description", 7), "Ethernet");
	EXPECT_EQ(WindowsNetwork::base_adapter_name("", "Adapter Alias", "description", 7), "Adapter Alias");
	EXPECT_EQ(WindowsNetwork::base_adapter_name("", "", "", 7), "if7");
	EXPECT_EQ(WindowsNetwork::display_adapter_name("Ethernet", 7, true), "Ethernet (7)");
	EXPECT_EQ(WindowsNetwork::display_adapter_name("Ethernet", 7, false), "Ethernet");
}

TEST(windows_network, rejects_ambiguous_aliases_but_resolves_unique_names) {
	std::unordered_map<std::string, std::string> selectors;
	WindowsNetwork::register_selector(selectors, "Ethernet (7)", "Ethernet (7)");
	WindowsNetwork::register_selector(selectors, "Ethernet", "Ethernet (7)");
	WindowsNetwork::register_selector(selectors, "Ethernet (9)", "Ethernet (9)");
	WindowsNetwork::register_selector(selectors, "ethernet", "Ethernet (9)");

	EXPECT_EQ(WindowsNetwork::resolve_selector(selectors, "ethernet (7)"), "Ethernet (7)");
	EXPECT_EQ(WindowsNetwork::resolve_selector(selectors, " ETHERNET (9) "), "Ethernet (9)");
	EXPECT_TRUE(WindowsNetwork::resolve_selector(selectors, "Ethernet").empty());
}

TEST(windows_network, prefers_routable_ipv4_addresses) {
	std::string selected;
	WindowsNetwork::prefer_address(selected, "169.254.4.2", false);
	EXPECT_EQ(selected, "169.254.4.2");
	WindowsNetwork::prefer_address(selected, "192.168.1.20", false);
	EXPECT_EQ(selected, "192.168.1.20");
	WindowsNetwork::prefer_address(selected, "10.0.0.5", false);
	EXPECT_EQ(selected, "192.168.1.20");
}

TEST(windows_network, prefers_routable_ipv6_addresses) {
	std::string selected;
	WindowsNetwork::prefer_address(selected, "fe8f::1234%7", true);
	EXPECT_EQ(selected, "fe8f::1234%7");
	WindowsNetwork::prefer_address(selected, "2001:db8::20", true);
	EXPECT_EQ(selected, "2001:db8::20");
	WindowsNetwork::prefer_address(selected, "fe80::5678%7", true);
	EXPECT_EQ(selected, "2001:db8::20");
}
#endif
