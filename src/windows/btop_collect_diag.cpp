// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>

namespace {

std::uint64_t filetime_to_u64(const FILETIME& ft) {
	ULARGE_INTEGER uli{};
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	return uli.QuadPart;
}

std::string wide_to_utf8(const wchar_t* input) {
	if (input == nullptr or input[0] == L'\0') return {};
	const int needed = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 1) return {};
	std::string out(needed - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, input, -1, out.data(), needed, nullptr, nullptr);
	return out;
}

int count_processes() {
	int count = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return 0;
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) ++count;
	CloseHandle(snapshot);
	return count;
}

int count_network_adapters() {
	DWORD count = 0;
	if (GetNumberOfInterfaces(&count) != NO_ERROR) return 0;
	return static_cast<int>(count);
}

int count_logical_disks() {
	wchar_t drives[512]{};
	const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
	int count = 0;
	for (const wchar_t* drive = drives; drive < drives + length and *drive != L'\0'; drive += std::wcslen(drive) + 1) {
		const UINT type = GetDriveTypeW(drive);
		if (type == DRIVE_FIXED or type == DRIVE_REMOVABLE or type == DRIVE_REMOTE) ++count;
	}
	return count;
}

struct cpu_times {
	std::uint64_t total{};
	std::uint64_t idle{};
};

cpu_times system_cpu_times() {
	FILETIME idle{}, kernel{}, user{};
	if (!GetSystemTimes(&idle, &kernel, &user)) return {};
	return {filetime_to_u64(kernel) + filetime_to_u64(user), filetime_to_u64(idle)};
}

std::vector<cpu_times> core_cpu_times() {
	std::vector<cpu_times> result;
	auto query = reinterpret_cast<decltype(&NtQuerySystemInformation)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
	if (query == nullptr) return result;
	SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION info[256]{};
	ULONG returned = 0;
	if (query(SystemProcessorPerformanceInformation, info, sizeof(info), &returned) < 0) return result;
	const int count = static_cast<int>(returned / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
	result.reserve(count);
	for (int i = 0; i < count; ++i) {
		result.push_back({
			static_cast<std::uint64_t>(info[i].KernelTime.QuadPart + info[i].UserTime.QuadPart),
			static_cast<std::uint64_t>(info[i].IdleTime.QuadPart),
		});
	}
	return result;
}

double cpu_percent(const cpu_times& before, const cpu_times& after) {
	const auto total_delta = after.total > before.total ? after.total - before.total : 0;
	const auto idle_delta = after.idle > before.idle ? after.idle - before.idle : 0;
	if (total_delta == 0) return 0.0;
	return std::clamp((total_delta - std::min(total_delta, idle_delta)) * 100.0 / total_delta, 0.0, 100.0);
}

}

int main() {
	SYSTEM_INFO sysinfo{};
	GetNativeSystemInfo(&sysinfo);

	MEMORYSTATUSEX mem{};
	mem.dwLength = sizeof(mem);
	const bool got_mem = GlobalMemoryStatusEx(&mem);

	const auto system_before = system_cpu_times();
	const auto cores_before = core_cpu_times();
	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	const auto system_after = system_cpu_times();
	const auto cores_after = core_cpu_times();

	SYSTEM_POWER_STATUS power{};
	const bool got_power = GetSystemPowerStatus(&power);

	std::cout << "windows collector diagnostics\n";
	std::cout << "cpu.logical_processors=" << std::max<DWORD>(1, sysinfo.dwNumberOfProcessors) << "\n";
	std::cout << "cpu.system_time_100ns=" << system_after.total << "\n";
	std::cout << "cpu.sample_total_percent=" << std::lround(cpu_percent(system_before, system_after)) << "\n";
	if (cores_before.size() == cores_after.size() and !cores_after.empty()) {
		std::vector<double> core_percent;
		core_percent.reserve(cores_after.size());
		for (size_t i = 0; i < cores_after.size(); ++i) core_percent.push_back(cpu_percent(cores_before.at(i), cores_after.at(i)));
		const auto [min_it, max_it] = std::minmax_element(core_percent.begin(), core_percent.end());
		const auto avg = std::accumulate(core_percent.begin(), core_percent.end(), 0.0) / core_percent.size();
		std::cout << "cpu.sample_core_avg_percent=" << std::lround(avg) << "\n";
		std::cout << "cpu.sample_core_min_percent=" << std::lround(*min_it) << "\n";
		std::cout << "cpu.sample_core_max_percent=" << std::lround(*max_it) << "\n";
	}
	std::cout << "memory.total=" << (got_mem ? mem.ullTotalPhys : 0) << "\n";
	std::cout << "memory.available=" << (got_mem ? mem.ullAvailPhys : 0) << "\n";
	std::cout << "disk.count=" << count_logical_disks() << "\n";
	std::cout << "network.adapters=" << count_network_adapters() << "\n";
	std::cout << "process.count=" << count_processes() << "\n";
	std::cout << "battery.present=" << (got_power and power.BatteryFlag != 128 and power.BatteryLifePercent != 255 ? 1 : 0) << "\n";
	return 0;
}