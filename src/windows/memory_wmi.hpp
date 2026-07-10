// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>

#include "memory_helpers.hpp"

namespace WindowsMemory {

auto read_pagefiles() -> std::optional<pagefile_stats>;

} // namespace WindowsMemory
