// SPDX-License-Identifier: Apache-2.0

#include <cerrno>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>

#include "btop_process_actions.hpp"
#include "windows/signal_compat.hpp"

TEST(windows_process_actions, exposes_supported_actions) {
	const auto actions = ProcessActions::supported();
	ASSERT_EQ(actions.size(), 4U);
	EXPECT_EQ(actions[0].signal, SIGTERM);
	EXPECT_EQ(actions[1].signal, SIGKILL);
	EXPECT_EQ(actions[2].signal, SIGSTOP);
	EXPECT_EQ(actions[3].signal, SIGCONT);
	EXPECT_EQ(ProcessActions::label(SIGSTOP), "Suspend (SIGSTOP)");
	EXPECT_EQ(ProcessActions::label(12345), "Unsupported");
}

TEST(windows_process_actions, probes_process_existence_without_mutation) {
	EXPECT_EQ(ProcessActions::send(static_cast<int>(GetCurrentProcessId()), 0), 0);
}

TEST(windows_process_actions, rejects_unsupported_actions) {
	errno = 0;
	EXPECT_EQ(ProcessActions::send(static_cast<int>(GetCurrentProcessId()), 12345), -1);
	EXPECT_EQ(errno, EINVAL);
}
#endif
