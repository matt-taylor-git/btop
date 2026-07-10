// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cmath>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "windows/cpu_helpers.hpp"

TEST(windows_cpu, counts_logical_processors_across_all_groups) {
	const DWORD count = WindowsCpu::logical_processor_count();
	EXPECT_GE(count, 1U);
	const WORD groups = WindowsCpu::processor_group_count();
	EXPECT_GE(groups, 1U);
	SYSTEM_INFO info{};
	GetNativeSystemInfo(&info);
	EXPECT_GE(count, std::max<DWORD>(1, info.dwNumberOfProcessors));
}

TEST(windows_cpu, smooths_queue_length_over_one_five_and_fifteen_minutes) {
	const std::array<double, 3> initial{};
	const auto averages = WindowsCpu::smooth_queue_averages(initial, 8.0, 60.0);

	EXPECT_NEAR(averages.at(0), 8.0 * (1.0 - std::exp(-1.0)), 1e-9);
	EXPECT_NEAR(averages.at(1), 8.0 * (1.0 - std::exp(-0.2)), 1e-9);
	EXPECT_NEAR(averages.at(2), 8.0 * (1.0 - std::exp(-1.0 / 15.0)), 1e-9);
	EXPECT_GT(averages.at(0), averages.at(1));
	EXPECT_GT(averages.at(1), averages.at(2));
}

TEST(windows_cpu, clamps_invalid_queue_samples_and_elapsed_time) {
	const std::array<double, 3> initial = {3.0, 2.0, 1.0};
	EXPECT_EQ(WindowsCpu::smooth_queue_averages(initial, 10.0, -5.0), initial);

	const auto averages = WindowsCpu::smooth_queue_averages(initial, -4.0, 60.0);
	EXPECT_LT(averages.at(0), initial.at(0));
	EXPECT_LT(averages.at(1), initial.at(1));
	EXPECT_LT(averages.at(2), initial.at(2));
}

#endif
