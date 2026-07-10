// SPDX-License-Identifier: Apache-2.0

#include <limits>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>

#include "windows/disk_helpers.hpp"

TEST(windows_disk, maps_windows_drive_type_codes) {
	EXPECT_EQ(WindowsDisk::drive_kind_from_code(DRIVE_REMOVABLE), WindowsDisk::drive_kind::removable);
	EXPECT_EQ(WindowsDisk::drive_kind_from_code(DRIVE_FIXED), WindowsDisk::drive_kind::fixed);
	EXPECT_EQ(WindowsDisk::drive_kind_from_code(DRIVE_REMOTE), WindowsDisk::drive_kind::network);
	EXPECT_EQ(WindowsDisk::drive_kind_from_code(DRIVE_CDROM), WindowsDisk::drive_kind::optical);
	EXPECT_EQ(WindowsDisk::drive_kind_from_code(DRIVE_RAMDISK), WindowsDisk::drive_kind::ramdisk);
	EXPECT_EQ(WindowsDisk::drive_kind_from_code(DRIVE_UNKNOWN), WindowsDisk::drive_kind::unknown);
}

TEST(windows_disk, applies_physical_drive_policy) {
	EXPECT_TRUE(WindowsDisk::allowed(WindowsDisk::drive_kind::fixed, true));
	EXPECT_TRUE(WindowsDisk::allowed(WindowsDisk::drive_kind::removable, true));
	EXPECT_FALSE(WindowsDisk::allowed(WindowsDisk::drive_kind::network, true));
	EXPECT_FALSE(WindowsDisk::allowed(WindowsDisk::drive_kind::optical, true));
	EXPECT_FALSE(WindowsDisk::allowed(WindowsDisk::drive_kind::ramdisk, true));
}

TEST(windows_disk, includes_supported_virtual_drives_when_requested) {
	EXPECT_TRUE(WindowsDisk::allowed(WindowsDisk::drive_kind::network, false));
	EXPECT_TRUE(WindowsDisk::allowed(WindowsDisk::drive_kind::optical, false));
	EXPECT_TRUE(WindowsDisk::allowed(WindowsDisk::drive_kind::ramdisk, false));
	EXPECT_FALSE(WindowsDisk::allowed(WindowsDisk::drive_kind::unknown, false));
}

TEST(windows_disk, limits_io_collectors_to_local_block_devices) {
	EXPECT_TRUE(WindowsDisk::supports_io(WindowsDisk::drive_kind::fixed));
	EXPECT_TRUE(WindowsDisk::supports_io(WindowsDisk::drive_kind::removable));
	EXPECT_FALSE(WindowsDisk::supports_io(WindowsDisk::drive_kind::network));
	EXPECT_FALSE(WindowsDisk::supports_io(WindowsDisk::drive_kind::optical));
}

TEST(windows_disk, cumulative_activity_prefers_idle_time) {
	std::int64_t query = 1000;
	std::int64_t idle = 400;
	std::int64_t busy = 600;

	EXPECT_EQ(WindowsDisk::activity_from_cumulative(2000, 700, 1600, query, idle, busy), 70);
	EXPECT_EQ(query, 2000);
	EXPECT_EQ(idle, 700);
	EXPECT_EQ(busy, 1600);
}

TEST(windows_disk, cumulative_activity_falls_back_to_busy_time) {
	std::int64_t query = 1000;
	std::int64_t idle = 0;
	std::int64_t busy = 300;

	EXPECT_EQ(WindowsDisk::activity_from_cumulative(2000, 0, 800, query, idle, busy), 50);
}

TEST(windows_disk, cumulative_activity_resets_invalid_deltas) {
	std::int64_t query = 2000;
	std::int64_t idle = 900;
	std::int64_t busy = 1100;

	EXPECT_EQ(WindowsDisk::activity_from_cumulative(100, 50, 50, query, idle, busy), 0);
	EXPECT_EQ(query, 100);
	EXPECT_EQ(idle, 50);
	EXPECT_EQ(busy, 50);
}

TEST(windows_disk, converts_pdh_rates_and_idle_percent) {
	const auto delta = WindowsDisk::pdh_rates_to_delta(1000.0, 500.0, 75.0, 2.0);
	ASSERT_TRUE(delta.has_value());
	EXPECT_EQ(delta->read_bytes, 2000);
	EXPECT_EQ(delta->write_bytes, 1000);
	EXPECT_EQ(delta->activity, 25);

	EXPECT_FALSE(WindowsDisk::pdh_rates_to_delta(
		std::numeric_limits<double>::quiet_NaN(), 0.0, 100.0, 1.0).has_value());
}

TEST(windows_disk, normalizes_windows_drive_mounts) {
	EXPECT_EQ(WindowsDisk::normalize_mount("C"), "C:\\");
	EXPECT_EQ(WindowsDisk::normalize_mount(" c: "), "C:\\");
	EXPECT_EQ(WindowsDisk::normalize_mount("c:/"), "C:\\");
	EXPECT_EQ(WindowsDisk::normalize_mount("C:\\"), "C:\\");
}

TEST(windows_disk, applies_include_and_exclude_filters) {
	const std::vector<std::string> filter = {"c", "D:/"};
	EXPECT_TRUE(WindowsDisk::filter_allows("C:\\", filter, false));
	EXPECT_FALSE(WindowsDisk::filter_allows("E:\\", filter, false));
	EXPECT_FALSE(WindowsDisk::filter_allows("C:\\", filter, true));
	EXPECT_TRUE(WindowsDisk::filter_allows("E:\\", filter, true));
}
#endif
