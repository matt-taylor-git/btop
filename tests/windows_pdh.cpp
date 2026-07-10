// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "windows/pdh_helpers.hpp"

TEST(windows_pdh, accepts_valid_and_new_counter_data) {
	EXPECT_TRUE(WindowsPdh::valid_counter_status(WindowsPdh::cstatus_valid_data));
	EXPECT_TRUE(WindowsPdh::valid_counter_status(WindowsPdh::cstatus_new_data));
	EXPECT_FALSE(WindowsPdh::valid_counter_status(-1));
	EXPECT_FALSE(WindowsPdh::valid_counter_status(2));
}

#endif
