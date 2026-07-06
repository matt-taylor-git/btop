# Windows Port Status

Date: 2026-07-06
Branch: windows-port
Workspace: D:\codeprojects\windowsbtop
Upstream base: aristocratos/btop main cloned on 2026-07-06

## Summary

This workspace now contains current upstream btop with a first Windows CMake path and an isolated Windows backend under `src/windows`.

CMake no longer rejects Windows as unsupported. The Windows configure path completes with MinGW GCC 14.2.0, the full `btop.exe` target links, the repository unit tests pass, and a non-interactive collector diagnostic executable builds and runs successfully.

This is a first working Windows port, not a polished release. Core collectors are present for CPU, memory/pagefile, disks including I/O deltas, network interfaces/addresses/counters/link state, process list, and battery status. Some metrics are approximate or intentionally deferred, and the interactive terminal UI has had a basic PTY smoke validation pass. Native PowerShell startup now works without requiring POSIX `HOME`/`XDG_*` variables or `--force-utf`.

## Implemented Files

- `src/windows/btop_collect.cpp`: Windows platform backend for btop collector interfaces.
- `src/windows/btop_collect_diag.cpp`: standalone diagnostic executable for non-interactive collector validation.
- `scripts/build-windows.ps1`: non-interactive configure/build script using `MinGW Makefiles` by default.
- `CMakeLists.txt`: adds WIN32 platform source selection, Windows system libraries, and diagnostic target.
- Shared compatibility edits:
  - `src/btop.cpp`
  - `src/btop_shared.hpp`
  - `src/btop_shared.cpp`
  - `src/btop_tools.cpp`
  - `src/btop_input.hpp`
  - `src/btop_input.cpp`
  - `src/btop_config.cpp`
  - `src/btop_log.cpp`
  - `src/btop_menu.cpp`
  - `src/btop_theme.cpp`

## Windows APIs Used

- CPU total usage: `GetSystemTimes`.
- CPU per-core usage: `NtQuerySystemInformation(SystemProcessorPerformanceInformation)` when available, with fallback to total CPU replicated per core.
- CPU name/frequency: registry keys under `HARDWARE\DESCRIPTION\System\CentralProcessor\0`.
- Uptime: `GetTickCount64`.
- Memory/pagefile: `GlobalMemoryStatusEx`.
- Disks: `GetLogicalDriveStringsW`, `GetDriveTypeW`, `GetDiskFreeSpaceExW`, `IOCTL_DISK_PERFORMANCE`.
- Network interfaces, addresses, and counters: `GetAdaptersAddresses`, `GetIfTable`.
- Network diagnostic count: `GetNumberOfInterfaces`.
- Process list: `CreateToolhelp32Snapshot`, `Process32FirstW`, `Process32NextW`.
- Process memory/path/times/user: `GetProcessMemoryInfo`, `QueryFullProcessImageNameW`, `GetProcessTimes`, `OpenProcessToken`, `GetTokenInformation`, `LookupAccountSidW`.
- Process terminate action: `OpenProcess(PROCESS_TERMINATE)` and `TerminateProcess`.
- Battery: `GetSystemPowerStatus`.
- Terminal shim: Windows console buffer APIs, `_kbhit`/`_getch`, and virtual terminal mode.
- Windows startup defaults: config under `%APPDATA%\btop`, state/log path under `%LOCALAPPDATA%\btop`, and UTF-8 console code pages on launch.

## Implemented Metrics and Approximations

- CPU total usage: implemented.
- CPU per-core usage: implemented through NT performance info; fallback is approximate. Per-core `KernelTime` already includes idle time on Windows, so totals are calculated as `KernelTime + UserTime` and idle is subtracted only once.
- Load average: stubbed to `0,0,0` because Windows has no native Unix load average equivalent.
- CPU temperature/watts: unsupported for milestone 1; hardware temperature and LibreHardwareMonitor are milestone 2.
- Memory: physical used/free/available implemented. Cache is currently `0` because Windows cache accounting needs a separate API pass.
- Pagefile/swap: implemented from total/available page file in `GlobalMemoryStatusEx`.
- Disk capacity/free/used: implemented for fixed, removable, and network drives.
- Disk I/O: implemented for fixed drives through `IOCTL_DISK_PERFORMANCE`; unavailable drives fail gracefully and simply omit I/O graphs.
- Network interfaces/counters/link state: implemented using `GetIfTable`.
- Network addresses: implemented using `GetAdaptersAddresses`; IPv4 and IPv6 are populated when Windows reports them.
- Processes: PID, PPID, name, executable path where permitted, user where permitted, working set memory, CPU delta, thread count, sorting, basic filtering, basic tree ordering, and terminate action are implemented. Command line arguments are currently approximated by executable path. Privileged/unreadable process fields fall back gracefully.
- Battery: present/percent/status/seconds when available. Watts are unavailable and reported as `-1`.

## Validation Commands and Results

### Clone and Branch

- `git clone https://github.com/aristocratos/btop.git .`
  - Result: success after removing a broken partial clone caused by sandboxed `.git` write restrictions.
- `git switch -c windows-port`
  - Result: success.

### Configure

- `cmake -S . -B build-windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBTOP_GPU=OFF`
  - Result: success.
  - Compiler: `D:/msys64/ucrt64/bin/c++.exe`, GNU 14.2.0.
  - CMake generated Windows build files instead of failing with `Windows unsupported`.

### Full Build, Tests, Diagnostic, Smoke

Latest validation command:

```powershell
cmake --build build-windows --target btop btop_windows_collect_diag btop_test --config RelWithDebInfo --parallel
ctest --test-dir build-windows --output-on-failure
.\build-windows\btop_windows_collect_diag.exe
.\build-windows\btop.exe --version
```

Result: success.

Build output included:

```text
[100%] Built target btop
[100%] Built target btop_windows_collect_diag
[100%] Built target btop_test
```

CTest output:

```text
100% tests passed, 0 tests failed out of 3
```

Tests:

- `tools.string_split`
- `cpu_names.amd`
- `cpu_names.intel`

Diagnostic output from the latest run after the per-core CPU accounting fix:

```text
windows collector diagnostics
cpu.logical_processors=8
cpu.system_time_100ns=40851900468750
cpu.sample_total_percent=38
cpu.sample_core_avg_percent=38
cpu.sample_core_min_percent=23
cpu.sample_core_max_percent=48
memory.total=17110953984
memory.available=5353836544
disk.count=3
network.adapters=27
process.count=313
battery.present=0
```

Earlier diagnostic output before the CPU sampling fields were added:

```text
windows collector diagnostics
cpu.logical_processors=8
cpu.system_time_100ns=40546546718750
memory.total=17110953984
memory.available=6015676416
disk.count=3
network.adapters=27
process.count=305
battery.present=0
```

Version smoke output:

```text
btop version: 1.4.7+9527231
Compiled with: c++.exe (14.2.0)
Configured with: cmake -DBTOP_STATIC=OFF -DBTOP_GPU=OFF
```

Earlier `./build-windows/btop.exe --help` also succeeded and printed CLI usage/options.

## Toolchain and Environment Notes

- CMake is available: 3.26.4.
- GCC is available: `D:\msys64\ucrt64\bin\g++.exe`, version 14.2.0.
- `cl.exe` and `clang++` were not found on PATH.
- Initial sandboxed configure failed because CMake could not remove scratch files in `build-windows`; rerunning with escalation fixed that environment issue.

## Visual Validation

A basic PTY smoke test was performed with:

```powershell
.\build-windows\btop.exe --force-utf
```

Result: success. The process started, rendered recognizable btop boxes/text, showed live CPU, memory, disk, network, and process data, accepted `q`, and exited with code `0`. A follow-up plain `btop.exe` PowerShell smoke test also started without `--force-utf` and without POSIX `HOME`/`XDG_*` variables when `%APPDATA%` and `%LOCALAPPDATA%` were available. After the per-core CPU accounting fix, another PTY smoke showed aggregate CPU around 16-20% with per-core values spread around the same range instead of uniformly around 50%.

Notes:

- The captured PTY output showed mojibake for some box drawing glyphs, but the layout was recognizable.
- Because `$XDG_CONFIG_HOME` and `$HOME` were not set in the test environment, btop warned that config and logging were disabled for that run.
- A final polish pass should still be done in Windows Terminal to tune fonts, glyphs, and input behavior.

## Remaining Limitations and Next Steps

1. Run a final Windows Terminal visual pass and record screenshot/notes.
2. Replace the minimal Windows input shim with richer key/mouse handling if needed after visual testing.
3. Retrieve full process command lines where available, likely through WMI/NT query APIs with graceful privilege fallback.
4. Improve memory cache accounting beyond the current `0` placeholder.
5. Make disk I/O graph scaling more faithful if `IOCTL_DISK_PERFORMANCE` is unavailable or disabled on a system.
6. Review shared Windows compatibility shims for upstream-quality style before opening a PR.

## Milestone 2 Items

- GPU collectors.
- Hardware temperatures.
- LibreHardwareMonitor integration.
- Windows service management.
- Richer admin-mode process details.
- Packaging/installer work.