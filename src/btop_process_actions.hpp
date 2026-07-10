// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include <string_view>

namespace ProcessActions {

struct action {
	int signal{};
	std::string_view label;
};

auto supported() -> std::span<const action>;
auto label(int signal) -> std::string_view;
auto send(int pid, int signal) -> int;

} // namespace ProcessActions
