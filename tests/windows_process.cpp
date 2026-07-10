// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "btop_shared.hpp"
#include "windows/process_helpers.hpp"

TEST(windows_process, maps_windows_priority_classes_to_representative_nice_values) {
	EXPECT_EQ(WindowsProcess::priority_class_to_nice(REALTIME_PRIORITY_CLASS), -20);
	EXPECT_EQ(WindowsProcess::priority_class_to_nice(HIGH_PRIORITY_CLASS), -15);
	EXPECT_EQ(WindowsProcess::priority_class_to_nice(ABOVE_NORMAL_PRIORITY_CLASS), -5);
	EXPECT_EQ(WindowsProcess::priority_class_to_nice(NORMAL_PRIORITY_CLASS), 0);
	EXPECT_EQ(WindowsProcess::priority_class_to_nice(BELOW_NORMAL_PRIORITY_CLASS), 5);
	EXPECT_EQ(WindowsProcess::priority_class_to_nice(IDLE_PRIORITY_CLASS), 19);
}

TEST(windows_process, maps_nice_buckets_back_to_windows_priority_classes) {
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(-20), REALTIME_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(-19), HIGH_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(-15), HIGH_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(-14), ABOVE_NORMAL_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(-5), ABOVE_NORMAL_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(-4), NORMAL_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(5), BELOW_NORMAL_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(14), BELOW_NORMAL_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(15), IDLE_PRIORITY_CLASS);
	EXPECT_EQ(WindowsProcess::nice_to_priority_class(19), IDLE_PRIORITY_CLASS);
}

TEST(windows_process, changes_and_restores_a_live_process_priority) {
	const DWORD original = GetPriorityClass(GetCurrentProcess());
	ASSERT_NE(original, 0U);
	const auto pid = static_cast<pid_t>(GetCurrentProcessId());
	const bool changed = Proc::set_priority(pid, 5);
	EXPECT_TRUE(changed);
	if (changed) {
		EXPECT_EQ(GetPriorityClass(GetCurrentProcess()), BELOW_NORMAL_PRIORITY_CLASS);
	}
	EXPECT_TRUE(Proc::set_priority(pid, WindowsProcess::priority_class_to_nice(original)));
}

TEST(windows_process, calculates_total_and_per_core_cpu) {
	const auto total = WindowsProcess::calculate_cpu(5000, 3000, 10000, 0, 0, 10.0, false, 8);
	const auto per_core = WindowsProcess::calculate_cpu(5000, 3000, 10000, 0, 0, 10.0, true, 8);
	EXPECT_DOUBLE_EQ(total.percent, 20.0);
	EXPECT_DOUBLE_EQ(per_core.percent, 160.0);
}

TEST(windows_process, handles_counter_reset_without_underflow) {
	const auto sample = WindowsProcess::calculate_cpu(1000, 2000, 10000, 0, 0, 10.0, false, 4);
	EXPECT_DOUBLE_EQ(sample.percent, 0.0);
	EXPECT_EQ(sample.cpu_milliseconds, 0U);
}

TEST(windows_process, supports_zero_first_sample_for_new_processes) {
	const auto sample = WindowsProcess::calculate_cpu(500000, 500000, 10000, 0, 0, 10.0, false, 4);
	EXPECT_DOUBLE_EQ(sample.percent, 0.0);
	EXPECT_GT(sample.cumulative_percent, 0.0);
}

TEST(windows_process, converts_creation_time_to_system_uptime) {
	constexpr uint64_t now = 5000000000ULL;
	constexpr uint64_t created = now - 250000000ULL;
	EXPECT_EQ(WindowsProcess::start_uptime_seconds(created, now, 1000.0), 975U);
	EXPECT_EQ(WindowsProcess::start_uptime_seconds(now + 1, now, 1000.0), 1000U);
}
#endif
