# Windows Port Status

Date: 2026-07-09
Branch: windows-port
Workspace: D:\codeprojects\windowsbtop
Upstream base: aristocratos/btop main cloned on 2026-07-06

## Summary

This workspace now contains current upstream btop with a Windows CMake path, Windows CI workflow, portable zip packaging, and an isolated Windows backend under `src/windows`.

CMake no longer rejects Windows as unsupported. The Windows configure path completes with MinGW GCC 14.2.0, the GPU-enabled `btop.exe` target links and runs, the repository unit tests pass, and a non-interactive collector diagnostic executable builds and runs successfully.

The requested Linux-parity baseline is implemented: full console keyboard/mouse input and menus, process controls/details, CPU/memory/disk/network/GPU collectors, Windows CI, and portable packaging. Windows-specific approximations and optional hardware providers are identified below. Native PowerShell startup works without requiring POSIX `HOME`/`XDG_*` variables or `--force-utf`.

## Feature Parity Completion Audit

| Area | Completion evidence |
| --- | --- |
| Input | Shared `Input::process()` plus `ReadConsoleInputW`; navigation, function, control, text, UTF-16, and resize events are covered by tests and real-console interaction. |
| Mouse | Click, wheel, drag, release, menu controls, process selection, and scrollbar behavior are covered by translation tests and physical Windows Terminal validation. |
| Menus/options | Help/options, editing, text fields, themes, presets, config persistence, and runtime layout changes were exercised against disposable config roots. |
| Processes | Full command lines with path fallback, NT protected-process snapshots, details/tree/filter/follow/sort/pause/per-core CPU, terminate/kill/suspend/resume, and tested priority mapping are implemented. |
| CPU | Total/per-core usage across processor groups, queue-length averages, power/frequency providers, and package/core temperature providers are implemented with deterministic fallback behavior. |
| Memory | Free/available/cache/commit accounting and actual WMI pagefile allocation/usage are implemented with tested clamping and fallback semantics. |
| Disks | Capacity/filter policy, fixed/removable IOCTL I/O, PDH fallback throughput/busy accounting, and network-volume presentation are implemented and tested. |
| Network | Friendly selection, addresses/link speed, switching, reset, auto/sync scaling, counter-reset handling, and config persistence are implemented and validated live. |
| GPUs | DXGI/PDH cover NVIDIA/AMD/Intel adapter memory/utilization/media engines, NVML enriches NVIDIA, and optional LibreHardwareMonitor fills vendor sensor metrics with AMD/Intel parser tests. |
| Release | MinGW CMake CI, diagnostics/tests, recursive runtime-DLL discovery, portable zip, and Windows Terminal/font/config guidance are present. |

## Implemented Files

- `src/windows/btop_collect.cpp`: Windows platform backend for btop collector interfaces.
- `src/windows/btop_collect_diag.cpp`: standalone diagnostic executable for non-interactive collector validation.
- `src/windows/hardware_sensors.hpp`: deterministic LibreHardwareMonitor sensor classification shared with Windows unit tests.
- `src/windows/memory_helpers.hpp` and `src/windows/memory_wmi.cpp`: tested physical/commit/pagefile mappings and the shared `Win32_PageFileUsage` provider used by btop and diagnostics.
- `src/windows/network_helpers.hpp`: deterministic friendly-name disambiguation, selector resolution, and display-address preference shared with Windows unit tests.
- `src/windows/process_helpers.hpp`: tested Windows process CPU-delta, cumulative-CPU, and creation-time conversion helpers.
- `src/windows/signal_compat.hpp` and `src/windows/posix_compat.hpp`: centralized Windows definitions for the small POSIX surface used by shared startup and input code.
- `src/btop_process_actions.hpp`, `src/windows/process_actions.cpp`, and `src/posix/process_actions.cpp`: platform boundary for process existence probes, signal delivery, termination, suspend, and resume actions.
- `.github/workflows/cmake-windows.yml`: Windows CMake CI, collector diagnostics, tests, and portable zip artifact packaging.
- `scripts/build-windows.ps1`: non-interactive Windows configure/build/test/diagnostic/package script using `MinGW Makefiles`, `RelWithDebInfo`, and GPU support by default; builds `btop`, `btop_windows_collect_diag`, and `btop_test` one target at a time and clears stale generated `objects.a` archives before each target build. `-Gpu OFF` remains available for diagnosis.
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
- CPU name/frequency: registry keys under `HARDWARE\DESCRIPTION\System\CentralProcessor\0`, with richer frequency samples from `CallNtPowerInformation(ProcessorInformation)` when available.
- Uptime: `GetTickCount64`.
- Memory/pagefile/cache/commit: `GlobalMemoryStatusEx`, `GetPerformanceInfo`, `NtQuerySystemInformation(SystemMemoryListInformation)` free/standby-list accounting, and `Win32_PageFileUsage` for actual allocated/current pagefile MiB.
- Disks: `GetLogicalDriveStringsW`, `GetDriveTypeW`, `GetDiskFreeSpaceExW`, `IOCTL_DISK_PERFORMANCE`, and PDH LogicalDisk counters as the I/O fallback.
- Network interfaces, addresses, and counters: `GetAdaptersAddresses`, `GetIfTable2` 64-bit octet counters, and `GetNumberOfInterfaces` diagnostics.
- Network diagnostic count: `GetNumberOfInterfaces`.
- Process list: `CreateToolhelp32Snapshot`, `Process32FirstW`, `Process32NextW`.
- Process command line/memory/path/times/user/priority/state: `NtQueryInformationProcess(ProcessCommandLineInformation)`, `NtQuerySystemInformation(SystemProcessInformation)`, `GetProcessMemoryInfo`, `QueryFullProcessImageNameW`, `GetProcessTimes`, `OpenProcessToken`, `GetTokenInformation`, `LookupAccountSidW`, and `GetPriorityClass`.
- Process actions: `SIGTERM`/`SIGKILL` map to `OpenProcess(PROCESS_TERMINATE)` and `TerminateProcess`; `SIGSTOP`/`SIGCONT` map to dynamic `NtSuspendProcess`/`NtResumeProcess` when available. The Windows process action menu exposes only Terminate, Kill, Suspend, and Resume so unsupported Unix signals are not advertised; other Unix signals remain unsupported and return `EINVAL`.
- Battery: `GetSystemPowerStatus`.
- GPU runtime/diagnostics: dynamic dxgi.dll adapter enumeration seeds Windows GPU names, LUID mapping, dedicated VRAM, and shared-system-memory capacity; dynamic NVML probing fills NVIDIA utilization/VRAM/power/clocks/temperature/encoder/decoder metrics; PDH GPU Engine/GPU Adapter Memory counters fill runtime utilization, encoder/decoder, and memory gaps where Windows exposes matching adapter LUIDs; and optional LibreHardwareMonitor WMI sensors fill missing NVIDIA/AMD/Intel temperature, power, graphics/memory clocks, and core/memory load without overriding available NVML or PDH samples. Discrete adapters retain dedicated-VRAM semantics and `vram` labels, while integrated adapters with no dedicated capacity use DXGI shared capacity plus PDH shared usage and are labeled `shared` in the detailed GPU view.
- Input and terminal shims: Windows console buffer APIs, virtual terminal mode, and `ReadConsoleInputW` key/mouse event handling. The old `_kbhit`/`_getch` path has been replaced, Windows disables processed console input so Ctrl+C reaches the input backend, raw Windows control bytes such as Ctrl+C and Ctrl+R normalize to the same quit/reload actions as the Unix input path, and Windows help text omits POSIX-only Ctrl+Z background suspend.
- Windows startup defaults: config under `%APPDATA%\btop`, state/log path under `%LOCALAPPDATA%\btop`, and UTF-8 console code pages on launch.

## Implemented Metrics and Approximations

- CPU total usage: implemented.
- CPU per-core usage: implemented through NT performance info; fallback is approximate. Logical CPU topology uses `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` instead of the current-group-only `GetSystemInfo` count, and per-core sample storage is dynamically sized rather than capped at 256 entries. Per-core `KernelTime` already includes idle time on Windows, so totals are calculated as `KernelTime + UserTime` and idle is subtracted only once.
- Load average equivalent: implemented as 1/5/15-minute exponential averages of PDH's `System\\Processor Queue Length`. Both `PDH_CSTATUS_VALID_DATA` and `PDH_CSTATUS_NEW_DATA` are accepted, and the Windows UI labels the row `Queue avg` because this is intentionally a Windows approximation, not a Unix load average.
- CPU temperature and power: a cached, optional `ROOT\LibreHardwareMonitor` WMI snapshot provides package temperature, naturally ordered per-core temperatures, and package watts when LibreHardwareMonitor is already running. `Auto` prefers those richer readings, while the options menu can explicitly select `LibreHardwareMonitor` or `Windows Thermal Zone`. Native package-style thermal-zone fallback is attempted through `MSAcpi_ThermalZoneTemperature`, and native watts are probed through `Win32_PerfFormattedData_Counters_PowerMeter`. LibreHardwareMonitor is not bundled or launched by btop; unavailable providers are retried periodically and fail back cleanly.
- Memory: physical used/available and commit totals are implemented through `GetPerformanceInfo`. Free now comes from zero plus free page lists instead of duplicating Available; Cached uses the larger of `SystemCache` and standby/modified page-list accounting. Values are clamped to physical invariants when optional NT counters are unavailable or inconsistent.
- Pagefile/swap: allocated, used, and free values come from a cached `Win32_PageFileUsage` WMI query across every configured pagefile. If that provider is unavailable, btop explicitly falls back to the previous commit-limit/physical-memory estimate. Commit totals remain separate diagnostic values rather than being mislabeled as pagefile occupancy.
- Disk capacity/free/used: implemented for fixed, removable, network, optical, and RAM-disk volumes where Windows reports readable capacity. Fixed/removable volumes remain visible with `only_physical=true`; network/optical/RAM-disk volumes are included only when it is false. Windows disk filters normalize `C`, `C:`, `C:/`, and `C:\\` to the same root mount form for friendlier drive-letter filtering.
- Disk I/O: implemented for fixed and removable drives with `IOCTL_DISK_PERFORMANCE` as the primary source. Busy percentage uses cumulative idle time against query time so overlapping reads and writes are not double-counted, with read/write time as a fallback for drivers that omit idle time. If the IOCTL cannot be opened or queried, per-volume PDH `LogicalDisk` read/write rates and `% Idle Time` keep throughput and busy graphs populated. Provider changes reset baselines and force graph reconstruction; only drives where both providers fail clear/omit I/O graphs. Removed or filtered drives close PDH queries and drop cached timing state so reappearing drives start from a fresh baseline.
- Network interfaces/counters/link state/link speed/friendly names: implemented using `GetIfTable2` 64-bit octet counters and `GetAdaptersAddresses`; interface selection accepts friendly names, MIB aliases/descriptions, adapter GUID names, and interface indexes case-insensitively, and manual previous/next interface changes persist back to `net_iface`. Total reset ignores vanished adapters safely, and lower 64-bit counter samples are treated as adapter resets instead of rollover.
- Network addresses: implemented using `GetAdaptersAddresses`; IPv4 and IPv6 are populated when Windows reports them.
- Processes: PID, PPID, name, full command line where permitted, executable path fallback, user fallback, working set memory, CPU delta and cumulative CPU, per-core CPU mode, priority class/nice mapping, thread count, sorting, filtering, tree ordering, follow/pause behavior, detailed process fields including command line, parent, state, memory, I/O totals, CPU graph scaling, dead-process elapsed time, terminate/kill, and suspend/resume signal mappings are implemented. A single `NtQuerySystemInformation(SystemProcessInformation)` snapshot now supplies state, parent, threads, working set, CPU/create times, and detailed I/O even for protected processes that reject `OpenProcess`; query-limited handles enrich priority, command line/path, user, and any missing fields. New PIDs begin with zero instantaneous CPU instead of a lifetime-total spike. Windows priority classes use representative nice values (`-20`, `-15`, `-5`, `0`, `5`, `19`) with a tested reverse mapping; explicitly selecting `-20` requests `REALTIME_PRIORITY_CLASS`, and permission failures surface as a menu error. Live state aggregates Windows thread states into btop's runnable, sleeping, waiting, stopped, and dead states. Diagnostics report snapshot coverage alongside command-line and handle-based readability.
- Battery: present/percent/status/seconds when available. Watts are unavailable and reported as `-1`.

## Validation Commands and Results

### Clone and Branch

- `git clone https://github.com/aristocratos/btop.git .`
  - Result: success after removing a broken partial clone caused by sandboxed `.git` write restrictions.
- `git switch -c windows-port`
  - Result: success.

### Configure

- `cmake -S . -B build-windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBTOP_GPU=ON`
  - Result: success.
  - Compiler: `D:/msys64/ucrt64/bin/c++.exe`, GNU 14.2.0.
  - CMake generated Windows build files instead of failing with `Windows unsupported`.

### Full Build, Tests, Diagnostic, Smoke

Latest validation command (the script performs the required archive cleanup before every target):

```powershell
.\scripts\build-windows.ps1 -BuildDir .\build-windows -PackageDir C:\tmp\btop-gpu-package -ZipPath C:\tmp\btop-gpu-package.zip -Gpu ON -Parallel 2
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
100% tests passed, 0 tests failed out of 54
```

The six Windows input tests cover navigation/function keys, control/text keys, UTF-16 surrogate pairs, console mouse events, and raw key/mouse normalization. Three Windows CPU tests cover all-group logical-processor counting, elapsed-time 1/5/15-minute queue smoothing, and invalid input clamping. One shared PDH test covers both valid counter statuses used by CPU, disk, and vendor-neutral GPU collectors. Three Windows GPU-memory tests cover discrete dedicated-memory preference, integrated shared-memory fallback, and capacity clamping. Ten Windows disk tests cover drive-kind mapping, physical/virtual policy, IO eligibility, idle-time activity, busy-time fallback, counter resets, PDH rate conversion, drive-letter normalization, and include/exclude filtering. Six Windows hardware-sensor tests cover Intel package/core ordering, AMD Tctl fallback, CPU package-power preference, NVIDIA/AMD/Intel GPU grouping, full AMD and Intel GPU temperature/power/clock/load selection, and invalid/unrelated sample rejection. Five Windows memory tests cover free/available separation, counter clamping, actual pagefile conversion, overflow, and commit-based fallback. Five Windows network tests cover selector normalization, stable duplicate/fallback names, ambiguous aliases, and routable IPv4/IPv6 preference. Seven Windows process tests cover priority-class round trips, a live priority change/restore, total/per-core CPU math, counter resets, zeroed first samples, and creation-time conversion. Three Windows process-action tests cover the supported action set, non-mutating process existence probes, and unsupported-action errors. Two process-tree tests cover Windows PID 0 self-parenting and stale multi-process parent cycles. The existing string and CPU-name tests also pass.

Selected diagnostic output from the latest run after CPU queue validation:

```text
windows collector diagnostics
cpu.logical_processors=8
cpu.processor_groups=1
cpu.sample_total_percent=51
cpu.load_average_source=pdh_system_processor_queue_length
cpu.load_average_windows_seconds=60,300,900
cpu.processor_queue_length=1
cpu.frequency_samples=8
cpu.frequency_current_min_mhz=3600
cpu.frequency_current_max_mhz=3600
cpu.frequency_current_avg_mhz=3600
libre_hardware.available=0
libre_hardware.sensors=0
gpu.dxgi_hardware_adapters=2
gpu.dxgi_shared_system_memory=25666430976
gpu.nvml_devices=1
gpu.nvml_utilization_samples=1
gpu.nvml_memory_samples=1
gpu.nvml_power_samples=1
gpu.nvml_temperature_samples=1
cpu.sample_core_avg_percent=51
cpu.sample_core_min_percent=35
cpu.sample_core_max_percent=69
memory.available=2419929088
memory.free=1561837568
memory.cached=903233536
memory.commit_total=40741191680
memory.pagefile_provider=wmi
memory.pagefile_files=2
memory.pagefile_total=23629660160
memory.pagefile_used=2630877184
memory.pagefile_free=20998782976
disk.count=4
disk.fixed=3
disk.remote=1
disk.policy_physical_visible=3
disk.policy_all_visible=4
disk.policy_readable_network=1
disk.io_readable=3
disk.io_idle_time_samples=3
disk.pdh_candidates=3
disk.pdh_queries=3
disk.pdh_readable=3
disk.pdh_activity_samples=3
network.metadata_adapters=4
process.count=392
process.command_lines_readable=220
process.command_lines_with_args=198
process.memory_readable=219
process.memory_readable_vm_read=210
process.state_query_processes=392
process.snapshot_named=391
process.snapshot_working_set=392
process.snapshot_cpu_time=388
process.snapshot_creation_time=392
process.snapshot_io=293
process.state_query_threads=5790
```

Version smoke output:

```text
btop version: 1.4.7+079ed13
Compiled with: c++.exe (14.2.0)
Configured with: cmake -DBTOP_STATIC=OFF -DBTOP_GPU=ON
```

Earlier `./build-windows/btop.exe --help` also succeeded and printed CLI usage/options.

## Toolchain and Environment Notes

- CMake is available: 3.26.4.
- GCC is available: `D:\msys64\ucrt64\bin\g++.exe`, version 14.2.0.
- `cl.exe` and `clang++` were not found on PATH.
- Initial sandboxed configure failed because CMake could not remove scratch files in `build-windows`; rerunning with escalation fixed that environment issue.

## Windows Runtime and Packaging Notes

The Windows CI workflow builds with MinGW and `-DBTOP_GPU=ON`, runs the unit tests and Windows collector diagnostic executable, then uploads a portable zip staged as `bin/btop.exe` plus `share/btop/themes`. The packaging script uses `objdump -p` to recursively copy non-system MinGW runtime DLL dependencies beside `bin/btop.exe` when the build is not fully static. That layout matches the theme lookup path that checks `../share/btop/themes` relative to the executable and also allows user themes under the btop config directory. The validated package contains `btop.exe`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`, and the complete theme tree.

Recommended terminal for interactive use is Windows Terminal or another console host with virtual terminal input/output support enabled. Use a monospace font with braille, box drawing, block, and Powerline glyph coverage; Nerd Fonts such as Terminess Nerd Font Mono are known-good candidates. The Windows startup path sets UTF-8 console code pages and uses `%APPDATA%\btop` for config and `%LOCALAPPDATA%\btop` for state/log files when POSIX-style `HOME`/`XDG_*` variables are unavailable.

The README now has a Windows-specific build/run section documenting the one-command MinGW build, portable archive layout, terminal/font requirements, UTF-8 behavior, config/state locations, and the `--config` override. Runtime defaults remain the upstream braille graphs and full color UI; users with incomplete glyph coverage can select a compatible font or switch graph symbols to `block`/`tty` in Options without requiring a Windows-only config fork.

Runtime dependencies for the current MinGW build are the executable, the theme files, and any MinGW runtime DLLs that are not statically linked by the selected build configuration. `scripts/package-windows.ps1` uses `objdump` to inspect the PE import table and fails the package step if runtime dependency discovery is unavailable or a required non-system DLL cannot be found on `PATH`; a fully static build can still omit copied DLLs once the static Windows configuration has been validated.

LibreHardwareMonitor is an optional sensor provider, not a packaged runtime dependency. When its WMI provider is running, btop can consume richer CPU package/core temperature and package-power readings plus missing NVIDIA/AMD/Intel GPU temperature, power, clock, and load readings without linking its .NET library or driver. Without it, btop continues through native Windows thermal-zone, Power Meter, DXGI, PDH, and NVML paths. The current validation host has no LibreHardwareMonitor namespace, so graceful absence and parser behavior are verified, but positive live-provider coverage still needs a host running LibreHardwareMonitor.

## Visual Validation

A basic PTY smoke test was performed with:

```powershell
.\build-windows\btop.exe --force-utf
```

Result: success. The GPU-enabled process started, rendered recognizable btop boxes/text, showed live CPU, memory, disk, network, process, and GPU data, accepted `q`, and exited with code `0`. The latest smoke displayed two DXGI adapters, with live NVML utilization, 8 GiB VRAM usage, and roughly 18 W for GPU0 plus dedicated VRAM for GPU1. A follow-up plain `btop.exe` PowerShell smoke test also started without `--force-utf` and without POSIX `HOME`/`XDG_*` variables when `%APPDATA%` and `%LOCALAPPDATA%` were available.

A real Windows Terminal pass was also run against disposable `%APPDATA%` and `%LOCALAPPDATA%` roots. UTF-8 box drawing, braille graphs, full process command lines, DXGI/NVML GPU rows, and responsive resize/layout recalculation rendered correctly. F1 help, Escape, Options, CPU options paging, filter text entry, process selection, detailed view, follow mode, preset cycling, tree mode, sorting, pause, and per-core process CPU controls all accepted input. A clean run persisted `color_theme = "TTY"`, `proc_tree = true`, `proc_per_core = true`, and the changed process sort in the isolated `btop.conf`.

Physical mouse clicks opened the main menu, selected Options, changed the theme arrow control, and selected a process row. Wheel input moved the process selection/list from `3/N` to `18/N`. A fixed-geometry, topmost Windows Terminal run then proved scrollbar behavior end to end: clicking the down arrow paged the flat process list, dragging the thumb moved it from `0/377` to `358/377`, and moving the pointer back to the top after button release left the viewport near the bottom (`357/376`, with the denominator changing as processes exited). The raw Windows/SGR tests independently cover click, wheel, drag, release, and coordinate translation.

An isolated real-console network pass exercised `n`, `a`, `s`, and `z` through the `ReadConsoleInputW` backend. Interface switching changed the rendered title to a second friendly adapter, auto mode changed the graph from its live KiB/s scale to the configured 12 MiB/s fixed scale, sync toggled without disrupting collection, and total reset activated the rendered zero indicator. The clean exit persisted `net_auto = false`, `net_sync = true`, and `net_iface = "Ethernet-WFP 802.3 MAC Layer LightWeight Filter-0000"` in the disposable config. Live metadata enumeration found four adapters with friendly names, addresses, and link speeds; duplicate names and ambiguous aliases are covered deterministically by the Windows network tests.

A post-snapshot real-console process smoke rendered 391 processes and exited cleanly. Protected entries that still had an unavailable user token, including `WmiPrvSE` and `MsMpEng`, displayed nonzero 38 MiB and 406 MiB working sets from the NT snapshot. This confirms the fallback is active in the real process table, not only in the standalone diagnostic parser.

A memory-only real-console pass rendered distinct physical values of roughly 2.0 GiB Available, 1.34 GiB Cached, and 0.9 GiB Free, confirming the NT free-page value reaches the UI instead of repeating Available. The shared WMI provider simultaneously reported two pagefiles with 23.63 GB allocated and 2.63 GB used, matching `Win32_PageFileUsage`; the earlier commit-derived display had incorrectly reported more than 14 GB used.

For live network-disk validation, `Z:` was temporarily mapped to `\\localhost\c$`. Diagnostics classified four readable volumes as three fixed plus one network, with three visible under `only_physical=true`, four under `false`, and only the three local volumes eligible for IO collection. An isolated mem-only run with `only_physical=false` rendered `Z:\` with capacity/used/free after C:, D:, and E:, while Z correctly had no IO% graph. The disposable mapping and config were removed after the pass.

The same pass found and fixed a deterministic process-tree stack overflow. Windows PID 0 reports parent PID 0, so the shared recursive tree builder treated PID 0 as its own descendant. Windows Error Reporting recorded `0xc00000fd`, and GDB showed `_tree_gen()` beyond depth 6400. The tree builder now treats self-parenting entries as standalone roots and tracks the active ancestor path to stop stale multi-process cycles. The exact filter-clear/tree-toggle reproduction now stays alive and renders the expected hierarchy.

Notes:

- The captured PTY output showed mojibake for some box drawing glyphs, but the layout was recognizable.
- The GPU-enabled build opened the Options menu with `o`, switched to the CPU tab with `2`, paged to `Freq mode: first`, closed with Escape, and exited with `q` at status 0.
- Because `$XDG_CONFIG_HOME` and `$HOME` were not set in the test environment, btop warned that config and logging were disabled for that run.
- Windows Terminal rendered the same build without the PTY glyph corruption.

## Additional Hardware Validation and Enhancements

These are useful follow-up coverage and optional provider enhancements, not missing parity implementation:

1. Repeat disk filtering and graph checks on physical removable media; the same drive-kind/IO paths have deterministic tests, while fixed-drive IOCTL/PDH and mapped-network-drive paths are covered live.
2. Repeat positive LibreHardwareMonitor CPU sensor checks on Intel and AMD hosts; provider absence, Intel/AMD parsing, package/core ordering, and fallback behavior are already covered.
3. Repeat GPU enrichment checks on physical AMD and Intel hosts, and consider direct ADLX/Intel vendor paths for additional metrics where redistributable APIs permit them.

The shared compatibility audit is complete for the current port: duplicated signal definitions and the accidental NetBSD-nested Windows startup shim were removed, process actions now have platform implementations, and shared `Input::process()` compiles through the centralized Windows signal compatibility header.

## Optional Future Work

- Windows service management.
- Richer admin-mode process details.
- Installer work beyond the current portable zip artifact.
