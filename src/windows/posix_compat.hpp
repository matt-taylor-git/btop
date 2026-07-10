// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdlib>
#include <signal.h>
#include <sys/types.h>
#include <windows.h>

#include "signal_compat.hpp"

using uid_t = unsigned int;
using sigset_t = _sigset_t;

static inline auto getuid() -> uid_t { return 0; }
static inline auto geteuid() -> uid_t { return 0; }
static inline auto seteuid(uid_t) -> int { return 0; }
[[maybe_unused]] static inline auto setenv(const char* name, const char* value, const int overwrite) -> int {
	return (overwrite or std::getenv(name) == nullptr) ? _putenv_s(name, value) : 0;
}
static inline auto sigemptyset(sigset_t*) -> int { return 0; }
static inline auto sigaddset(sigset_t*, int) -> int { return 0; }

#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#ifndef SIG_SETMASK
#define SIG_SETMASK 0
#endif
