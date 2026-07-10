// SPDX-License-Identifier: Apache-2.0

#include <limits>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "windows/memory_helpers.hpp"

TEST(windows_memory, separates_free_from_available_memory) {
	const auto stats = WindowsMemory::physical(16'000, 6'000, 2'000, 3'000, 4'000);
	EXPECT_EQ(stats.used, 10'000U);
	EXPECT_EQ(stats.available, 6'000U);
	EXPECT_EQ(stats.free, 2'000U);
	EXPECT_EQ(stats.cached, 4'000U);
}

TEST(windows_memory, clamps_inconsistent_physical_counters) {
	const auto stats = WindowsMemory::physical(100, 120, 110, 200, 150);
	EXPECT_EQ(stats.used, 0U);
	EXPECT_EQ(stats.available, 100U);
	EXPECT_EQ(stats.free, 100U);
	EXPECT_EQ(stats.cached, 100U);
}

TEST(windows_memory, converts_actual_pagefile_usage_from_mebibytes) {
	const auto stats = WindowsMemory::pagefiles_from_mebibytes(22'535, 2'528, 2);
	EXPECT_EQ(stats.total, 22'535ULL << 20);
	EXPECT_EQ(stats.used, 2'528ULL << 20);
	EXPECT_EQ(stats.free, 20'007ULL << 20);
	EXPECT_EQ(stats.files, 2U);
}

TEST(windows_memory, clamps_pagefile_usage_and_conversion_overflow) {
	const auto clamped = WindowsMemory::pagefiles_from_mebibytes(10, 12, 1);
	EXPECT_EQ(clamped.used, clamped.total);
	EXPECT_EQ(clamped.free, 0U);
	const auto overflow = WindowsMemory::pagefiles_from_mebibytes(std::numeric_limits<uint64_t>::max(), 1, 1);
	EXPECT_EQ(overflow.total, std::numeric_limits<uint64_t>::max());
}

TEST(windows_memory, estimates_pagefile_only_when_provider_is_unavailable) {
	const auto stats = WindowsMemory::estimated_pagefiles(40'000, 30'000, 16'000);
	EXPECT_EQ(stats.total, 24'000U);
	EXPECT_EQ(stats.used, 14'000U);
	EXPECT_EQ(stats.free, 10'000U);
}
#endif
