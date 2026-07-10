// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "windows/gpu_helpers.hpp"

TEST(windows_gpu, prefers_dedicated_memory_for_discrete_adapters) {
	const auto memory = WindowsGpu::select_memory(8'000, 16'000, 3'000, 5'000);
	EXPECT_EQ(memory.total, 8'000U);
	EXPECT_EQ(memory.used, 3'000U);
	EXPECT_FALSE(memory.shared);
	EXPECT_TRUE(memory.available);
}

TEST(windows_gpu, uses_shared_memory_for_integrated_adapters) {
	const auto memory = WindowsGpu::select_memory(0, 8'000, -1, 2'500);
	EXPECT_EQ(memory.total, 8'000U);
	EXPECT_EQ(memory.used, 2'500U);
	EXPECT_TRUE(memory.shared);
	EXPECT_TRUE(memory.available);
}

TEST(windows_gpu, clamps_usage_to_the_selected_capacity) {
	EXPECT_EQ(WindowsGpu::select_memory(4'000, 8'000, 5'000, 2'000).used, 4'000U);
	EXPECT_FALSE(WindowsGpu::select_memory(4'000, 8'000, -1, 2'000).available);
	EXPECT_FALSE(WindowsGpu::select_memory(0, 8'000, -1, -1).available);
	EXPECT_EQ(WindowsGpu::select_memory(0, 0, -1, 2'000).used, 0U);
}

#endif
