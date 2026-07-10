// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

#include "btop_shared.hpp"

namespace {

auto make_process(const size_t pid, const uint64_t ppid) -> Proc::proc_info {
	Proc::proc_info process;
	process.pid = pid;
	process.ppid = ppid;
	return process;
}

} // namespace

TEST(process_tree, ignores_self_parenting_processes) {
	std::vector<Proc::proc_info> processes = {
		make_process(0, 0),
		make_process(1, 0),
		make_process(2, 1),
	};
	std::ranges::stable_sort(processes, std::ranges::less {}, &Proc::proc_info::ppid);

	std::vector<Proc::tree_proc> tree;
	for (auto& process : std::ranges::equal_range(processes, 0, std::ranges::less {}, &Proc::proc_info::ppid)) {
		Proc::_tree_gen(process, processes, tree, 0, false, "");
	}

	ASSERT_EQ(tree.size(), 2U);
	EXPECT_EQ(tree.at(0).entry.get().pid, 0U);
	EXPECT_TRUE(tree.at(0).children.empty());
	EXPECT_EQ(tree.at(1).entry.get().pid, 1U);
	ASSERT_EQ(tree.at(1).children.size(), 1U);
	EXPECT_EQ(tree.at(1).children.front().entry.get().pid, 2U);
}

TEST(process_tree, stops_at_multi_process_parent_cycles) {
	std::vector<Proc::proc_info> processes = {
		make_process(10, 11),
		make_process(11, 10),
	};
	std::ranges::stable_sort(processes, std::ranges::less {}, &Proc::proc_info::ppid);

	std::vector<Proc::tree_proc> tree;
	Proc::_tree_gen(processes.at(1), processes, tree, 0, false, "");

	ASSERT_EQ(tree.size(), 1U);
	ASSERT_EQ(tree.front().children.size(), 1U);
	EXPECT_TRUE(tree.front().children.front().children.empty());
}
