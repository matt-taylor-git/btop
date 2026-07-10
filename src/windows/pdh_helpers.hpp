// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace WindowsPdh {

	constexpr long cstatus_valid_data = 0;
	constexpr long cstatus_new_data = 1;

	inline bool valid_counter_status(const long status) {
		return status == cstatus_valid_data or status == cstatus_new_data;
	}

}
