// SPDX-License-Identifier: Apache-2.0

#include "../btop_process_actions.hpp"

#include <signal.h>

namespace ProcessActions {

auto supported() -> std::span<const action> { return {}; }
auto label(int) -> std::string_view { return {}; }
auto send(const int pid, const int signal) -> int { return ::kill(pid, signal); }

} // namespace ProcessActions
