/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winioctl.h>
#include <sddl.h>
#include <powerbase.h>
#include <winternl.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <fmt/format.h>

#include "../btop_config.hpp"
#include "../btop_log.hpp"
#include "../btop_shared.hpp"
#include "../btop_tools.hpp"

namespace fs = std::filesystem;
namespace rng = std::ranges;
using namespace Tools;
using std::clamp;
using std::max;
using std::min;
using std::round;
using std::string;
using std::vector;

namespace {

uint64_t filetime_to_u64(const FILETIME& ft) {
	ULARGE_INTEGER uli{};
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	return uli.QuadPart;
}

string wide_to_utf8(const wchar_t* input) {
	if (input == nullptr or input[0] == L'\0') return {};
	const int needed = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 1) return {};
	string out(needed - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, input, -1, out.data(), needed, nullptr, nullptr);
	return out;
}

void push_limited(std::deque<long long>& values, long long value, size_t limit) {
	values.push_back(value);
	while (values.size() > limit) values.pop_front();
}

string query_registry_string(HKEY root, const wchar_t* path, const wchar_t* name) {
	HKEY key{};
	if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
	DWORD type = 0;
	DWORD bytes = 0;
	if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS or (type != REG_SZ and type != REG_EXPAND_SZ)) {
		RegCloseKey(key);
		return {};
	}
	std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
	const auto result = RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
	RegCloseKey(key);
	if (result != ERROR_SUCCESS) return {};
	return wide_to_utf8(buffer.c_str());
}

uint64_t current_system_cpu_time() {
	FILETIME idle{}, kernel{}, user{};
	if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
	return filetime_to_u64(kernel) + filetime_to_u64(user);
}

string process_path(HANDLE process) {
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD size = static_cast<DWORD>(buffer.size());
	if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
		buffer.resize(size);
		return wide_to_utf8(buffer.c_str());
	}
	return {};
}

string process_user(HANDLE process) {
	HANDLE token{};
	if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
	DWORD bytes = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
	if (bytes == 0) {
		CloseHandle(token);
		return {};
	}
	vector<std::byte> data(bytes);
	if (!GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes)) {
		CloseHandle(token);
		return {};
	}
	CloseHandle(token);
	auto* token_user = reinterpret_cast<TOKEN_USER*>(data.data());
	wchar_t name[256]{};
	wchar_t domain[256]{};
	DWORD name_len = 256;
	DWORD domain_len = 256;
	SID_NAME_USE use{};
	if (LookupAccountSidW(nullptr, token_user->User.Sid, name, &name_len, domain, &domain_len, &use)) {
		const auto user = wide_to_utf8(name);
		const auto dom = wide_to_utf8(domain);
		return dom.empty() ? user : dom + "\\" + user;
	}
	return {};
}

}

namespace Cpu {
	extern vector<long long> core_old_totals;
	extern vector<long long> core_old_idles;
	extern cpu_info current_cpu;
}

namespace Shared {
	long coreCount = 1;
	long page_size = 4096;
	long clk_tck = 1000;
	uint64_t totalMem = 0;

	void init() {
		SYSTEM_INFO sysinfo{};
		GetNativeSystemInfo(&sysinfo);
		coreCount = static_cast<long>(max<DWORD>(1, sysinfo.dwNumberOfProcessors));
		page_size = sysinfo.dwPageSize > 0 ? static_cast<long>(sysinfo.dwPageSize) : 4096;
		clk_tck = 1000;

		MEMORYSTATUSEX mem{};
		mem.dwLength = sizeof(mem);
		if (GlobalMemoryStatusEx(&mem)) totalMem = mem.ullTotalPhys;

		Cpu::current_cpu.core_percent.assign(coreCount, {});
		Cpu::current_cpu.temp.assign(coreCount + 1, {});
		Cpu::core_old_totals.assign(coreCount, 0);
		Cpu::core_old_idles.assign(coreCount, 0);
		Cpu::available_fields = {"Auto", "total", "user", "system", "idle"};
		Cpu::available_sensors = {"Auto"};
		Cpu::cpuName = Cpu::trim_name(query_registry_string(HKEY_LOCAL_MACHINE,
			LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", L"ProcessorNameString"));
		if (Cpu::cpuName.empty()) Cpu::cpuName = "Windows CPU";
		Cpu::has_battery = true;
		Cpu::got_sensors = false;
		Cpu::supports_watts = false;
		Cpu::core_mapping = Cpu::get_core_mapping();
		Cpu::collect();
		Mem::collect();
		Net::collect();
		Proc::collect();
		Logger::debug("Init -> Windows collectors initialized.");
	}
}

namespace Cpu {
	vector<long long> core_old_totals;
	vector<long long> core_old_idles;
	vector<string> available_fields = {"Auto", "total"};
	vector<string> available_sensors = {"Auto"};
	cpu_info current_cpu;
	bool got_sensors = false;
	bool cpu_temp_only = false;
	bool supports_watts = false;
	string cpuName;
	string cpuHz;
	bool has_battery = true;
	tuple<int, float, long, string> current_bat;
	std::unordered_map<int, int> core_mapping;

	std::unordered_map<string, long long> cpu_old = {
		{"totals", 0}, {"idles", 0}, {"user", 0}, {"system", 0}, {"idle", 0}
	};

	auto get_core_mapping() -> std::unordered_map<int, int> {
		std::unordered_map<int, int> mapping;
		for (int i = 0; i < Shared::coreCount; ++i) mapping[i] = i;
		return mapping;
	}

	auto get_cpuHz() -> string {
		HKEY key{};
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
		DWORD mhz = 0;
		DWORD bytes = sizeof(mhz);
		const auto result = RegQueryValueExW(key, L"~MHz", nullptr, nullptr, reinterpret_cast<BYTE*>(&mhz), &bytes);
		RegCloseKey(key);
		if (result != ERROR_SUCCESS or mhz == 0) return {};
		return mhz >= 1000 ? fmt::format("{:.2f} GHz", mhz / 1000.0) : fmt::format("{} MHz", mhz);
	}

	auto get_battery() -> tuple<int, float, long, string> {
		SYSTEM_POWER_STATUS status{};
		if (!GetSystemPowerStatus(&status) or status.BatteryFlag == 128 or status.BatteryLifePercent == 255) {
			has_battery = false;
			return {0, 0.0F, 0, ""};
		}
		string state = "unknown";
		if ((status.BatteryFlag & 8) != 0) state = "charging";
		else if ((status.ACLineStatus == 1) and status.BatteryLifePercent >= 100) state = "full";
		else if (status.ACLineStatus == 0) state = "discharging";
		const long seconds = status.BatteryLifeTime == static_cast<DWORD>(-1) ? -1 : static_cast<long>(status.BatteryLifeTime);
		return {static_cast<int>(status.BatteryLifePercent), -1.0F, seconds, state};
	}

	auto collect(bool no_update) -> cpu_info& {
		if (Runner::stopping or (no_update and not current_cpu.cpu_percent.at("total").empty())) return current_cpu;
		auto& cpu = current_cpu;
		if (Config::getB("show_cpu_freq")) cpuHz = get_cpuHz();
		cpu.load_avg = {0.0, 0.0, 0.0};

		FILETIME idle{}, kernel{}, user{};
		if (GetSystemTimes(&idle, &kernel, &user)) {
			const long long idle_time = static_cast<long long>(filetime_to_u64(idle));
			const long long kernel_time = static_cast<long long>(filetime_to_u64(kernel));
			const long long user_time = static_cast<long long>(filetime_to_u64(user));
			const long long system_time = max(0ll, kernel_time - idle_time);
			const long long total_time = max(1ll, user_time + kernel_time);
			const long long total_delta = max(1ll, total_time - cpu_old.at("totals"));
			const long long idle_delta = max(0ll, idle_time - cpu_old.at("idles"));
			const long long user_delta = max(0ll, user_time - cpu_old.at("user"));
			const long long system_delta = max(0ll, system_time - cpu_old.at("system"));
			cpu_old.at("totals") = total_time;
			cpu_old.at("idles") = idle_time;
			cpu_old.at("user") = user_time;
			cpu_old.at("system") = system_time;
			push_limited(cpu.cpu_percent.at("total"), clamp((long long)round((total_delta - idle_delta) * 100.0 / total_delta), 0ll, 100ll), max(1, Cpu::width * 2));
			push_limited(cpu.cpu_percent.at("user"), clamp((long long)round(user_delta * 100.0 / total_delta), 0ll, 100ll), max(1, Cpu::width * 2));
			push_limited(cpu.cpu_percent.at("system"), clamp((long long)round(system_delta * 100.0 / total_delta), 0ll, 100ll), max(1, Cpu::width * 2));
			push_limited(cpu.cpu_percent.at("idle"), clamp((long long)round(idle_delta * 100.0 / total_delta), 0ll, 100ll), max(1, Cpu::width * 2));
		}

		while (std::cmp_less(cpu.core_percent.size(), Shared::coreCount)) cpu.core_percent.emplace_back();
		SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION spi[256]{};
		ULONG returned = 0;
		auto NtQuerySystemInformationPtr = reinterpret_cast<decltype(&NtQuerySystemInformation)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
		if (NtQuerySystemInformationPtr and NtQuerySystemInformationPtr(SystemProcessorPerformanceInformation, spi, sizeof(spi), &returned) >= 0) {
			const int count = min<int>(Shared::coreCount, returned / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
			for (int i = 0; i < count; ++i) {
				const long long idle_time = spi[i].IdleTime.QuadPart;
				const long long total_time = spi[i].KernelTime.QuadPart + spi[i].UserTime.QuadPart;
				const long long total_delta = max(1ll, total_time - core_old_totals.at(i));
				const long long idle_delta = max(0ll, idle_time - core_old_idles.at(i));
				core_old_totals.at(i) = total_time;
				core_old_idles.at(i) = idle_time;
				push_limited(cpu.core_percent.at(i), clamp((long long)round((total_delta - idle_delta) * 100.0 / total_delta), 0ll, 100ll), 40);
			}
		}
		else if (!cpu.cpu_percent.at("total").empty()) {
			const auto total = cpu.cpu_percent.at("total").back();
			for (auto& core : cpu.core_percent) push_limited(core, total, 40);
		}

		if (Config::getB("show_battery") and has_battery) current_bat = get_battery();
		cpu.active_cpus = std::views::iota(0, static_cast<int>(Shared::coreCount)) | std::ranges::to<std::vector<std::int32_t>>();
		return cpu;
	}
}

namespace Mem {
	bool has_swap = true;
	int disk_ios = 0;
	mem_info current_mem;
	std::unordered_map<char, array<int64_t, 3>> old_disk_io;

	uint64_t get_totalMem() { return Shared::totalMem; }

	auto collect(bool no_update) -> mem_info& {
		if (Runner::stopping or (no_update and not current_mem.percent.at("used").empty())) return current_mem;
		auto& mem = current_mem;
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (GlobalMemoryStatusEx(&status)) {
			Shared::totalMem = status.ullTotalPhys;
			mem.stats["free"] = status.ullAvailPhys;
			mem.stats["available"] = status.ullAvailPhys;
			mem.stats["used"] = status.ullTotalPhys - status.ullAvailPhys;
			mem.stats["cached"] = 0;
			mem.stats["swap_total"] = status.ullTotalPageFile;
			mem.stats["swap_free"] = status.ullAvailPageFile;
			mem.stats["swap_used"] = status.ullTotalPageFile > status.ullAvailPageFile ? status.ullTotalPageFile - status.ullAvailPageFile : 0;
			has_swap = status.ullTotalPageFile > 0;
			for (const auto& name : mem_names) {
				const auto denom = max<uint64_t>(1, status.ullTotalPhys);
				push_limited(mem.percent[name], clamp((long long)round(mem.stats[name] * 100.0 / denom), 0ll, 100ll), max(1, Mem::width));
			}
			for (const auto& name : swap_names) {
				const auto denom = max<uint64_t>(1, status.ullTotalPageFile);
				push_limited(mem.percent[name], clamp((long long)round(mem.stats[name] * 100.0 / denom), 0ll, 100ll), max(1, Mem::width));
			}
		}

		wchar_t drives[512]{};
		const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
		std::unordered_set<string> found;
		for (const wchar_t* drive = drives; drive < drives + length and *drive != L'\0'; drive += std::wcslen(drive) + 1) {
			const UINT type = GetDriveTypeW(drive);
			if (type != DRIVE_FIXED and type != DRIVE_REMOVABLE and type != DRIVE_REMOTE) continue;
			ULARGE_INTEGER free_bytes{}, total_bytes{}, avail_bytes{};
			if (!GetDiskFreeSpaceExW(drive, &avail_bytes, &total_bytes, &free_bytes) or total_bytes.QuadPart == 0) continue;
			const string name = wide_to_utf8(drive);
			found.insert(name);
			auto& disk = mem.disks[name];
			disk.name = name;
			disk.dev = name;
			disk.fstype = type == DRIVE_REMOTE ? "network" : "windows";
			disk.total = static_cast<int64_t>(total_bytes.QuadPart);
			disk.free = static_cast<int64_t>(free_bytes.QuadPart);
			disk.used = disk.total - disk.free;
			disk.used_percent = clamp((int)round(disk.used * 100.0 / max<int64_t>(1, disk.total)), 0, 100);
			disk.free_percent = 100 - disk.used_percent;
			if (!v_contains(mem.disks_order, name)) mem.disks_order.push_back(name);
		}
		auto order_end = rng::remove_if(mem.disks_order, [&](const string& name) { return !found.contains(name); });
		mem.disks_order.erase(order_end.begin(), order_end.end());
		for (auto it = mem.disks.begin(); it != mem.disks.end();) {
			if (!found.contains(it->first)) it = mem.disks.erase(it);
			else ++it;
		}
		disk_ios = 0;
		return mem;
	}
}

namespace Net {
	vector<string> interfaces;
	string selected_iface;
	std::unordered_map<string, net_info> current_net;
	std::unordered_map<string, uint64_t> graph_max = {{"download", {}}, {"upload", {}}};
	bool rescale = true;

	auto collect(bool no_update) -> net_info& {
		static net_info empty_net;
		if (Runner::stopping or (no_update and !selected_iface.empty() and current_net.contains(selected_iface))) return current_net.at(selected_iface);

		DWORD bytes = 0;
		if (GetIfTable(nullptr, &bytes, FALSE) != ERROR_INSUFFICIENT_BUFFER) return empty_net;
		vector<std::byte> table_buf(bytes);
		auto* table = reinterpret_cast<MIB_IFTABLE*>(table_buf.data());
		if (GetIfTable(table, &bytes, FALSE) != NO_ERROR) return empty_net;

		std::unordered_set<string> found;
		for (DWORD i = 0; i < table->dwNumEntries; ++i) {
			const auto& row = table->table[i];
			if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
			string name(reinterpret_cast<const char*>(row.bDescr), reinterpret_cast<const char*>(row.bDescr) + row.dwDescrLen);
			if (name.empty()) name = fmt::format("if{}", row.dwIndex);
			found.insert(name);
			if (!v_contains(interfaces, name)) interfaces.push_back(name);

			auto& net = current_net[name];
			net.connected = row.dwOperStatus == IF_OPER_STATUS_OPERATIONAL;
			auto update_dir = [&](const string& dir, uint64_t total) {
				auto& stat = net.stat[dir];
				if (stat.last != 0 and total < stat.last) stat.rollover += 1ULL << 32;
				stat.total = total + stat.rollover;
				stat.speed = (stat.last != 0 and stat.total >= stat.last) ? stat.total - stat.last : 0;
				stat.last = stat.total;
				stat.top = max(stat.top, stat.speed);
				push_limited(net.bandwidth[dir], static_cast<long long>(stat.speed), max(1, Net::width));
				graph_max[dir] = max<uint64_t>(graph_max[dir], max<uint64_t>(stat.speed, 10 << 10));
			};
			update_dir("download", row.dwInOctets);
			update_dir("upload", row.dwOutOctets);
		}

		auto iface_end = rng::remove_if(interfaces, [&](const string& name) { return !found.contains(name); });
		interfaces.erase(iface_end.begin(), iface_end.end());
		for (auto it = current_net.begin(); it != current_net.end();) {
			if (!found.contains(it->first)) it = current_net.erase(it);
			else ++it;
		}
		if (selected_iface.empty() or !v_contains(interfaces, selected_iface)) selected_iface = interfaces.empty() ? string{} : interfaces.front();
		rescale = false;
		return selected_iface.empty() ? empty_net : current_net.at(selected_iface);
	}
}

namespace Proc {
	vector<proc_info> current_procs;
	atomic<int> numpids{};
	detail_container detailed;
	int filter_found{};
	std::unordered_map<size_t, uint64_t> old_proc_cpu;
	uint64_t old_system_cpu = 0;

	auto collect(bool no_update) -> vector<proc_info>& {
		if (Runner::stopping or (no_update and !current_procs.empty())) return current_procs;
		const uint64_t system_now = current_system_cpu_time();
		const uint64_t system_delta = max<uint64_t>(1, system_now - old_system_cpu);
		old_system_cpu = system_now;
		std::unordered_set<size_t> found;
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return current_procs;
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
			const size_t pid = entry.th32ProcessID;
			found.insert(pid);
			auto old = rng::find(current_procs, pid, &proc_info::pid);
			if (old == current_procs.end()) {
				current_procs.push_back({pid});
				old = current_procs.end() - 1;
			}
			auto& proc = *old;
			proc.name = wide_to_utf8(entry.szExeFile);
			proc.ppid = entry.th32ParentProcessID;
			proc.threads = entry.cntThreads;
			proc.state = 'S';
			proc.p_nice = 0;
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
			if (process != nullptr) {
				PROCESS_MEMORY_COUNTERS_EX pmc{};
				if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) proc.mem = pmc.WorkingSetSize;
				FILETIME create{}, exit{}, kernel{}, user{};
				if (GetProcessTimes(process, &create, &exit, &kernel, &user)) {
					const uint64_t cpu_now = filetime_to_u64(kernel) + filetime_to_u64(user);
					const uint64_t cpu_delta = cpu_now >= old_proc_cpu[pid] ? cpu_now - old_proc_cpu[pid] : 0;
					old_proc_cpu[pid] = cpu_now;
					proc.cpu_p = clamp(cpu_delta * 100.0 / system_delta * Shared::coreCount, 0.0, 100.0 * Shared::coreCount);
					proc.cpu_c = proc.cpu_p;
					proc.cpu_t = cpu_now / 10000;
				}
				proc.cmd = process_path(process);
				proc.user = process_user(process);
				CloseHandle(process);
			}
			if (proc.cmd.empty()) proc.cmd = proc.name;
			if (proc.user.empty()) proc.user = "unknown";
		}
		CloseHandle(snapshot);
		auto erase_end = rng::remove_if(current_procs, [&](const proc_info& p) { return !found.contains(p.pid); });
		current_procs.erase(erase_end.begin(), erase_end.end());
		for (auto it = old_proc_cpu.begin(); it != old_proc_cpu.end();) {
			if (!found.contains(it->first)) it = old_proc_cpu.erase(it);
			else ++it;
		}
		const string sorting = Config::getS("proc_sorting");
		const bool reverse = Config::getB("proc_reversed");
		proc_sorter(current_procs, sorting.empty() ? "cpu lazy" : sorting, reverse, false);
		numpids = static_cast<int>(current_procs.size());
		return current_procs;
	}
}

namespace Tools {
	double system_uptime() {
		return GetTickCount64() / 1000.0;
	}
}