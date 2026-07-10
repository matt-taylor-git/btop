// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dxgi.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winioctl.h>
#include <winternl.h>
#include <powerbase.h>

#include "cpu_helpers.hpp"
#include "disk_helpers.hpp"
#include "memory_helpers.hpp"
#include "memory_wmi.hpp"
#include "pdh_helpers.hpp"

namespace {

template <typename Fn>
Fn windows_function(HMODULE module, const char* name) {
	const FARPROC proc = GetProcAddress(module, name);
	Fn fn{};
	static_assert(sizeof(fn) == sizeof(proc));
	std::memcpy(&fn, &proc, sizeof(fn));
	return fn;
}

template <typename Fn>
Fn ntdll_function(const char* name) {
	return windows_function<Fn>(GetModuleHandleW(L"ntdll.dll"), name);
}

struct system_memory_list_information {
	SIZE_T zero_page_count{};
	SIZE_T free_page_count{};
	SIZE_T modified_page_count{};
	SIZE_T modified_no_write_page_count{};
	SIZE_T bad_page_count{};
	SIZE_T page_count_by_priority[8]{};
	SIZE_T repurposed_pages_by_priority[8]{};
	SIZE_T modified_page_count_page_file{};
};

struct windows_memory_list_sample {
	std::uint64_t free{};
	std::uint64_t cache{};
};

std::optional<windows_memory_list_sample> read_windows_memory_lists(const std::uint64_t page_size) {
	using NtQuerySystemInformationFn = NTSTATUS (WINAPI *)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
	const auto query = ntdll_function<NtQuerySystemInformationFn>("NtQuerySystemInformation");
	if (query == nullptr or page_size == 0) return std::nullopt;

	system_memory_list_information info{};
	constexpr auto SystemMemoryListInformation = static_cast<SYSTEM_INFORMATION_CLASS>(80);
	if (query(SystemMemoryListInformation, &info, sizeof(info), nullptr) < 0) return std::nullopt;

	std::uint64_t standby_pages = 0;
	for (const auto pages : info.page_count_by_priority) standby_pages += static_cast<std::uint64_t>(pages);
	const auto modified_pages = static_cast<std::uint64_t>(info.modified_page_count) + static_cast<std::uint64_t>(info.modified_no_write_page_count);
	const auto free_pages = static_cast<std::uint64_t>(info.zero_page_count) + static_cast<std::uint64_t>(info.free_page_count);
	return windows_memory_list_sample{free_pages * page_size, (standby_pages + modified_pages) * page_size};
}
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

std::string wide_to_utf8(const std::wstring_view input) {
	if (input.empty()) return {};
	const int needed = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
	if (needed <= 0) return {};
	std::string out(needed, '\0');
	WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), needed, nullptr, nullptr);
	return out;
}

std::string process_command_line(HANDLE process) {
	using NtQueryInformationProcessFn = NTSTATUS (WINAPI *)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
	const auto query = ntdll_function<NtQueryInformationProcessFn>("NtQueryInformationProcess");
	if (query == nullptr) return {};

	ULONG bytes = 0;
	constexpr auto ProcessCommandLineInformation = static_cast<PROCESSINFOCLASS>(60);
	query(process, ProcessCommandLineInformation, nullptr, 0, &bytes);
	if (bytes < sizeof(UNICODE_STRING)) return {};

	std::vector<std::byte> buffer(bytes);
	if (query(process, ProcessCommandLineInformation, buffer.data(), bytes, &bytes) < 0) return {};
	auto* command = reinterpret_cast<UNICODE_STRING*>(buffer.data());
	if (command->Buffer == nullptr or command->Length == 0) return {};
	return wide_to_utf8(std::wstring_view(command->Buffer, command->Length / sizeof(wchar_t)));
}

bool command_line_has_args(const std::string& command) {
	if (command.empty()) return false;
	size_t pos = 0;
	if (command.front() == '"') {
		pos = command.find('"', 1);
		if (pos == std::string::npos) return false;
		++pos;
	}
	else {
		pos = command.find(' ');
		if (pos == std::string::npos) return false;
	}
	while (pos < command.size() and command.at(pos) == ' ') ++pos;
	return pos < command.size();
}

std::pair<int, int> count_process_command_lines() {
	int readable = 0;
	int with_args = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return {0, 0};
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
		if (process == nullptr) continue;
		const auto command = process_command_line(process);
		CloseHandle(process);
		if (command.empty()) continue;
		++readable;
		if (command_line_has_args(command)) ++with_args;
	}
	CloseHandle(snapshot);
	return {readable, with_args};
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

int count_process_memory_readable(const DWORD access_rights) {
	int readable = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return 0;
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
		HANDLE process = OpenProcess(access_rights, FALSE, entry.th32ProcessID);
		if (process == nullptr) continue;
		PROCESS_MEMORY_COUNTERS_EX pmc{};
		if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) ++readable;
		CloseHandle(process);
	}
	CloseHandle(snapshot);
	return readable;
}

struct process_state_summary {
	int processes{};
	int threads{};
	int named_processes{};
	int working_set_processes{};
	int cpu_time_processes{};
	int creation_time_processes{};
	int io_processes{};
	int runnable_threads{};
	int waiting_threads{};
	int suspended_threads{};
	int transition_threads{};
};

std::optional<process_state_summary> read_process_state_summary() {
	using NtQuerySystemInformationFn = NTSTATUS (WINAPI *)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
	const auto query = ntdll_function<NtQuerySystemInformationFn>("NtQuerySystemInformation");
	if (query == nullptr) return std::nullopt;

	constexpr NTSTATUS status_info_length_mismatch = static_cast<NTSTATUS>(0xC0000004UL);
	ULONG bytes = 1U << 20;
	std::vector<std::byte> buffer(bytes);
	NTSTATUS status{};
	for (int attempt = 0; attempt < 5; ++attempt) {
		ULONG needed = 0;
		status = query(SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
		if (status >= 0) break;
		if (status != status_info_length_mismatch) return std::nullopt;
		bytes = std::max<ULONG>(static_cast<ULONG>(buffer.size() * 2), needed + (64U << 10));
		buffer.resize(bytes);
	}
	if (status < 0) return std::nullopt;

	process_state_summary summary;
	auto* cursor = buffer.data();
	while (true) {
		auto* process = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(cursor);
		auto* threads = reinterpret_cast<SYSTEM_THREAD_INFORMATION*>(process + 1);
		++summary.processes;
		if (process->ImageName.Buffer != nullptr and process->ImageName.Length > 0) ++summary.named_processes;
		if (process->VirtualMemoryCounters.WorkingSetSize > 0) ++summary.working_set_processes;
		if (process->KernelTime.QuadPart > 0 or process->UserTime.QuadPart > 0) ++summary.cpu_time_processes;
		if (process->CreateTime.QuadPart > 0) ++summary.creation_time_processes;
		if (process->IoCounters.ReadTransferCount > 0 or process->IoCounters.WriteTransferCount > 0) ++summary.io_processes;
		for (ULONG i = 0; i < process->NumberOfThreads; ++i) {
			++summary.threads;
			const ULONG state = threads[i].ThreadState;
			const ULONG reason = threads[i].WaitReason;
			if (state == 1 or state == 2 or state == 3 or state == 7) ++summary.runnable_threads;
			if (state == 5 or state == 8) ++summary.waiting_threads;
			if ((state == 5 or state == 8) and (reason == 5 or reason == 12)) ++summary.suspended_threads;
			if (state == 6 or state == 9) ++summary.transition_threads;
		}
		if (process->NextEntryOffset == 0) break;
		cursor += process->NextEntryOffset;
	}
	return summary;
}

int count_network_adapters() {
	DWORD count = 0;
	if (GetNumberOfInterfaces(&count) != NO_ERROR) return 0;
	return static_cast<int>(count);
}

int count_network_if_table2_rows() {
	MIB_IF_TABLE2* table = nullptr;
	if (GetIfTable2(&table) != NO_ERROR or table == nullptr) return 0;
	const int rows = static_cast<int>(table->NumEntries);
	FreeMibTable(table);
	return rows;
}

struct gpu_dxgi_summary {
	int adapters{};
	int hardware_adapters{};
	int software_adapters{};
	int nvidia{};
	int amd{};
	int intel{};
	std::uint64_t dedicated_vram{};
	std::uint64_t shared_system_memory{};
	std::vector<std::string> names;
};

std::string trim_copy(std::string value) {
	while (!value.empty() and std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
	while (!value.empty() and std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
	return value;
}

gpu_dxgi_summary read_dxgi_gpu_summary() {
	gpu_dxgi_summary summary;
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	if (dxgi == nullptr) return summary;
	auto close_library = [&] { FreeLibrary(dxgi); };
	using CreateDXGIFactory1Fn = HRESULT (WINAPI *)(REFIID, void**);
	const auto create_factory = windows_function<CreateDXGIFactory1Fn>(dxgi, "CreateDXGIFactory1");
	if (create_factory == nullptr) {
		close_library();
		return summary;
	}

	IID factory_iid{};
	if (FAILED(IIDFromString(const_cast<LPOLESTR>(L"{770AAE78-F26F-4DBA-A829-253C83D1B387}"), &factory_iid))) {
		close_library();
		return summary;
	}

	IDXGIFactory1* factory{};
	if (FAILED(create_factory(factory_iid, reinterpret_cast<void**>(&factory))) or factory == nullptr) {
		close_library();
		return summary;
	}

	for (UINT index = 0;; ++index) {
		IDXGIAdapter1* adapter{};
		const HRESULT result = factory->EnumAdapters1(index, &adapter);
		if (result == DXGI_ERROR_NOT_FOUND) break;
		if (FAILED(result) or adapter == nullptr) continue;
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc))) {
			++summary.adapters;
			const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
			if (software) ++summary.software_adapters;
			else ++summary.hardware_adapters;
			summary.dedicated_vram += static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
			summary.shared_system_memory += static_cast<std::uint64_t>(desc.SharedSystemMemory);
			switch (desc.VendorId) {
				case 0x10DE: ++summary.nvidia; break;
				case 0x1002:
				case 0x1022: ++summary.amd; break;
				case 0x8086: ++summary.intel; break;
				default: break;
			}
			const auto name = trim_copy(wide_to_utf8(desc.Description));
			if (!name.empty()) summary.names.push_back(name);
		}
		adapter->Release();
	}

	factory->Release();
	close_library();
	return summary;
}

struct nvml_utilization_t {
	unsigned int gpu{};
	unsigned int memory{};
};

struct nvml_memory_t {
	unsigned long long total{};
	unsigned long long free{};
	unsigned long long used{};
};

struct gpu_nvml_summary {
	bool library_loaded{};
	bool initialized{};
	unsigned int devices{};
	int names{};
	int utilization_samples{};
	int memory_samples{};
	int power_samples{};
	int graphics_clock_samples{};
	int memory_clock_samples{};
	int temperature_samples{};
	int encoder_samples{};
	int decoder_samples{};
	unsigned long long vram_total{};
	unsigned long long vram_used{};
};

gpu_nvml_summary read_nvml_gpu_summary() {
	gpu_nvml_summary summary;
	HMODULE nvml = LoadLibraryW(L"nvml.dll");
	if (nvml == nullptr) nvml = LoadLibraryW(L"nvml64.dll");
	if (nvml == nullptr) return summary;
	summary.library_loaded = true;
	auto close_library = [&] { FreeLibrary(nvml); };

	using nvmlDevice_t = void*;
	using nvmlReturn_t = int;
	using nvmlInitFn = nvmlReturn_t (*)();
	using nvmlShutdownFn = nvmlReturn_t (*)();
	using nvmlDeviceGetCountFn = nvmlReturn_t (*)(unsigned int*);
	using nvmlDeviceGetHandleByIndexFn = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
	using nvmlDeviceGetNameFn = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
	using nvmlDeviceGetUtilizationRatesFn = nvmlReturn_t (*)(nvmlDevice_t, nvml_utilization_t*);
	using nvmlDeviceGetMemoryInfoFn = nvmlReturn_t (*)(nvmlDevice_t, nvml_memory_t*);
	using nvmlDeviceGetPowerUsageFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
	using nvmlDeviceGetClockInfoFn = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
	using nvmlDeviceGetTemperatureFn = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
	using nvmlDeviceGetEncoderUtilizationFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, unsigned int*);
	using nvmlDeviceGetDecoderUtilizationFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, unsigned int*);

	const auto get_symbol = [&](const char* name) { return GetProcAddress(nvml, name); };
	const auto init = reinterpret_cast<nvmlInitFn>(get_symbol("nvmlInit_v2") != nullptr ? get_symbol("nvmlInit_v2") : get_symbol("nvmlInit"));
	const auto shutdown = reinterpret_cast<nvmlShutdownFn>(get_symbol("nvmlShutdown"));
	const auto get_count = reinterpret_cast<nvmlDeviceGetCountFn>(get_symbol("nvmlDeviceGetCount_v2") != nullptr ? get_symbol("nvmlDeviceGetCount_v2") : get_symbol("nvmlDeviceGetCount"));
	const auto get_handle = reinterpret_cast<nvmlDeviceGetHandleByIndexFn>(get_symbol("nvmlDeviceGetHandleByIndex_v2") != nullptr ? get_symbol("nvmlDeviceGetHandleByIndex_v2") : get_symbol("nvmlDeviceGetHandleByIndex"));
	const auto get_name = reinterpret_cast<nvmlDeviceGetNameFn>(get_symbol("nvmlDeviceGetName"));
	const auto get_utilization = reinterpret_cast<nvmlDeviceGetUtilizationRatesFn>(get_symbol("nvmlDeviceGetUtilizationRates"));
	const auto get_memory = reinterpret_cast<nvmlDeviceGetMemoryInfoFn>(get_symbol("nvmlDeviceGetMemoryInfo"));
	const auto get_power = reinterpret_cast<nvmlDeviceGetPowerUsageFn>(get_symbol("nvmlDeviceGetPowerUsage"));
	const auto get_clock = reinterpret_cast<nvmlDeviceGetClockInfoFn>(get_symbol("nvmlDeviceGetClockInfo"));
	const auto get_temperature = reinterpret_cast<nvmlDeviceGetTemperatureFn>(get_symbol("nvmlDeviceGetTemperature"));
	const auto get_encoder = reinterpret_cast<nvmlDeviceGetEncoderUtilizationFn>(get_symbol("nvmlDeviceGetEncoderUtilization"));
	const auto get_decoder = reinterpret_cast<nvmlDeviceGetDecoderUtilizationFn>(get_symbol("nvmlDeviceGetDecoderUtilization"));
	if (init == nullptr or shutdown == nullptr or get_count == nullptr or get_handle == nullptr) {
		close_library();
		return summary;
	}

	constexpr nvmlReturn_t nvml_success = 0;
	if (init() != nvml_success) {
		close_library();
		return summary;
	}
	summary.initialized = true;

	unsigned int device_count = 0;
	if (get_count(&device_count) == nvml_success) summary.devices = device_count;
	for (unsigned int i = 0; i < summary.devices; ++i) {
		nvmlDevice_t device{};
		if (get_handle(i, &device) != nvml_success or device == nullptr) continue;
		if (get_name != nullptr) {
			char name[96]{};
			if (get_name(device, name, sizeof(name)) == nvml_success and name[0] != '\0') ++summary.names;
		}
		if (get_utilization != nullptr) {
			nvml_utilization_t utilization{};
			if (get_utilization(device, &utilization) == nvml_success) ++summary.utilization_samples;
		}
		if (get_memory != nullptr) {
			nvml_memory_t memory{};
			if (get_memory(device, &memory) == nvml_success) {
				++summary.memory_samples;
				summary.vram_total += memory.total;
				summary.vram_used += memory.used;
			}
		}
		if (get_power != nullptr) {
			unsigned int power{};
			if (get_power(device, &power) == nvml_success) ++summary.power_samples;
		}
		if (get_clock != nullptr) {
			unsigned int clock{};
			if (get_clock(device, 0, &clock) == nvml_success) ++summary.graphics_clock_samples;
			if (get_clock(device, 2, &clock) == nvml_success) ++summary.memory_clock_samples;
		}
		if (get_temperature != nullptr) {
			unsigned int temperature{};
			if (get_temperature(device, 0, &temperature) == nvml_success) ++summary.temperature_samples;
		}
		if (get_encoder != nullptr) {
			unsigned int utilization{};
			unsigned int sampling_period{};
			if (get_encoder(device, &utilization, &sampling_period) == nvml_success) ++summary.encoder_samples;
		}
		if (get_decoder != nullptr) {
			unsigned int utilization{};
			unsigned int sampling_period{};
			if (get_decoder(device, &utilization, &sampling_period) == nvml_success) ++summary.decoder_samples;
		}
	}

	shutdown();
	close_library();
	return summary;
}

struct network_metadata_summary {
	int adapters{};
	int friendly_names{};
	int addresses{};
	int link_speeds{};
};

std::string sockaddr_to_string(const SOCKADDR* address, const int length) {
	static const bool winsock_ready = [] {
		WSADATA data{};
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	char host[NI_MAXHOST]{};
	if (!winsock_ready or address == nullptr) return {};
	if (getnameinfo(address, length, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0) return {};
	return host;
}

network_metadata_summary read_network_metadata_summary() {
	network_metadata_summary summary;
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG bytes = 15 * 1024;
	std::vector<std::byte> buffer(bytes);
	ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.assign(bytes, {});
		result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
	}
	if (result != NO_ERROR) return summary;

	for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr; adapter = adapter->Next) {
		if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		++summary.adapters;
		if (adapter->FriendlyName != nullptr and adapter->FriendlyName[0] != L'\0') ++summary.friendly_names;
		if (adapter->ReceiveLinkSpeed > 0 or adapter->TransmitLinkSpeed > 0) ++summary.link_speeds;
		for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
			if (!sockaddr_to_string(unicast->Address.lpSockaddr, static_cast<int>(unicast->Address.iSockaddrLength)).empty()) {
				++summary.addresses;
				break;
			}
		}
	}
	return summary;
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

struct disk_type_summary {
	int fixed{};
	int removable{};
	int remote{};
	int ramdisk{};
	int optical{};
};

struct disk_policy_summary {
	int physical_visible{};
	int all_visible{};
	int readable_network{};
};

disk_type_summary count_disk_types() {
	disk_type_summary summary;
	wchar_t drives[512]{};
	const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
	for (const wchar_t* drive = drives; drive < drives + length and *drive != L'\0'; drive += std::wcslen(drive) + 1) {
		switch (WindowsDisk::drive_kind_from_code(GetDriveTypeW(drive))) {
			case WindowsDisk::drive_kind::fixed: ++summary.fixed; break;
			case WindowsDisk::drive_kind::removable: ++summary.removable; break;
			case WindowsDisk::drive_kind::network: ++summary.remote; break;
			case WindowsDisk::drive_kind::ramdisk: ++summary.ramdisk; break;
			case WindowsDisk::drive_kind::optical: ++summary.optical; break;
			default: break;
		}
	}
	return summary;
}

disk_policy_summary count_disk_policy_volumes() {
	disk_policy_summary summary;
	wchar_t drives[512]{};
	const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
	if (length == 0 or length >= std::size(drives)) return summary;
	for (const wchar_t* drive = drives; drive < drives + length and *drive != L'\0'; drive += std::wcslen(drive) + 1) {
		const auto kind = WindowsDisk::drive_kind_from_code(GetDriveTypeW(drive));
		ULARGE_INTEGER free_bytes{};
		ULARGE_INTEGER total_bytes{};
		ULARGE_INTEGER available_bytes{};
		if (!GetDiskFreeSpaceExW(drive, &available_bytes, &total_bytes, &free_bytes) or total_bytes.QuadPart == 0) continue;
		if (WindowsDisk::allowed(kind, true)) ++summary.physical_visible;
		if (WindowsDisk::allowed(kind, false)) ++summary.all_visible;
		if (kind == WindowsDisk::drive_kind::network) ++summary.readable_network;
	}
	return summary;
}
struct disk_performance_summary {
	int candidates{};
	int readable{};
	int idle_time_samples{};
};

disk_performance_summary count_disk_performance_volumes() {
	wchar_t drives[512]{};
	const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
	disk_performance_summary summary;
	for (const wchar_t* drive = drives; drive < drives + length and *drive != L'\0'; drive += std::wcslen(drive) + 1) {
		const UINT type = GetDriveTypeW(drive);
		if (type != DRIVE_FIXED and type != DRIVE_REMOVABLE) continue;
		++summary.candidates;
		wchar_t volume_path[] = L"\\\\.\\C:";
		volume_path[4] = drive[0];
		HANDLE volume = CreateFileW(volume_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (volume == INVALID_HANDLE_VALUE) continue;
		DISK_PERFORMANCE performance{};
		DWORD bytes_returned = 0;
		if (DeviceIoControl(volume, IOCTL_DISK_PERFORMANCE, nullptr, 0, &performance, sizeof(performance), &bytes_returned, nullptr) and bytes_returned >= sizeof(DISK_PERFORMANCE)) {
			++summary.readable;
			if (performance.QueryTime.QuadPart > 0 and performance.IdleTime.QuadPart > 0) ++summary.idle_time_samples;
		}
		CloseHandle(volume);
	}
	return summary;
}

struct disk_pdh_summary {
	int candidates{};
	int queries{};
	int readable{};
	int activity_samples{};
	std::uint64_t read_bytes{};
	std::uint64_t write_bytes{};
};

struct disk_pdh_query {
	PDH_HQUERY query{};
	PDH_HCOUNTER read_counter{};
	PDH_HCOUNTER write_counter{};
	PDH_HCOUNTER idle_counter{};
};

bool add_logical_disk_counter(const PDH_HQUERY query, const std::wstring& path, PDH_HCOUNTER& counter) {
	if (PdhAddEnglishCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS) return true;
	return PdhAddCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS;
}

auto read_logical_disk_counter(const PDH_HCOUNTER counter) -> std::optional<double> {
	PDH_FMT_COUNTERVALUE value{};
	if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) return std::nullopt;
	if (not WindowsPdh::valid_counter_status(value.CStatus)) return std::nullopt;
	return value.doubleValue;
}

disk_pdh_summary count_disk_pdh_volumes() {
	disk_pdh_summary summary;
	std::vector<disk_pdh_query> queries;
	wchar_t drives[512]{};
	const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
	for (const wchar_t* drive = drives; drive < drives + length and *drive != L'\0'; drive += std::wcslen(drive) + 1) {
		const UINT type = GetDriveTypeW(drive);
		if (type != DRIVE_FIXED and type != DRIVE_REMOVABLE) continue;
		++summary.candidates;

		disk_pdh_query query;
		if (PdhOpenQueryW(nullptr, 0, &query.query) != ERROR_SUCCESS) continue;
		const std::wstring instance = std::wstring(1, drive[0]) + L":";
		const std::wstring prefix = L"\\LogicalDisk(" + instance + L")\\";
		if (not add_logical_disk_counter(query.query, prefix + L"Disk Read Bytes/sec", query.read_counter) or
			not add_logical_disk_counter(query.query, prefix + L"Disk Write Bytes/sec", query.write_counter) or
			not add_logical_disk_counter(query.query, prefix + L"% Idle Time", query.idle_counter) or
			PdhCollectQueryData(query.query) != ERROR_SUCCESS) {
			PdhCloseQuery(query.query);
			continue;
		}
		queries.push_back(query);
		++summary.queries;
	}

	const auto sampled_at = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	for (auto& query : queries) {
		if (PdhCollectQueryData(query.query) == ERROR_SUCCESS) {
			const auto read_rate = read_logical_disk_counter(query.read_counter);
			const auto write_rate = read_logical_disk_counter(query.write_counter);
			const auto idle_percent = read_logical_disk_counter(query.idle_counter);
			const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - sampled_at).count();
			if (read_rate and write_rate and idle_percent) {
				if (const auto delta = WindowsDisk::pdh_rates_to_delta(*read_rate, *write_rate, *idle_percent, elapsed)) {
					++summary.readable;
					++summary.activity_samples;
					summary.read_bytes += static_cast<std::uint64_t>(delta->read_bytes);
					summary.write_bytes += static_cast<std::uint64_t>(delta->write_bytes);
				}
			}
		}
		PdhCloseQuery(query.query);
	}
	return summary;
}

struct windows_processor_power_information {
    ULONG Number{};
    ULONG MaxMhz{};
    ULONG CurrentMhz{};
    ULONG MhzLimit{};
    ULONG MaxIdleState{};
    ULONG CurrentIdleState{};
};

struct cpu_frequency_summary {
    int samples{};
    unsigned long current_min{};
    unsigned long current_max{};
    double current_avg{};
    unsigned long max_mhz{};
};

std::optional<cpu_frequency_summary> cpu_frequencies(const DWORD logical_processors) {
    std::vector<windows_processor_power_information> info(std::max<DWORD>(1, logical_processors));
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, info.data(), static_cast<ULONG>(info.size() * sizeof(windows_processor_power_information))) < 0) return std::nullopt;
    std::vector<unsigned long> current;
    unsigned long max_mhz = 0;
    for (const auto& cpu : info) {
        if (cpu.CurrentMhz > 0) current.push_back(cpu.CurrentMhz);
        max_mhz = std::max(max_mhz, cpu.MaxMhz);
    }
    if (current.empty()) return std::nullopt;
    const auto [min_it, max_it] = std::minmax_element(current.begin(), current.end());
    return cpu_frequency_summary{
        static_cast<int>(current.size()),
        *min_it,
        *max_it,
        std::accumulate(current.begin(), current.end(), 0.0) / current.size(),
        max_mhz,
    };
}

int count_pdh_wildcard_paths(const wchar_t* path) {
	using PdhExpandWildCardPathWFn = PDH_STATUS (WINAPI *)(LPCWSTR, LPCWSTR, LPWSTR, LPDWORD, DWORD);
	HMODULE pdh = LoadLibraryW(L"pdh.dll");
	if (pdh == nullptr) return 0;
	auto close_library = [&] { FreeLibrary(pdh); };
	const auto expand_path = windows_function<PdhExpandWildCardPathWFn>(pdh, "PdhExpandWildCardPathW");
	if (expand_path == nullptr) {
		close_library();
		return 0;
	}

	DWORD bytes = 0;
	DWORD count = 0;
	constexpr PDH_STATUS pdh_more_data = static_cast<PDH_STATUS>(0x800007D2);
	PDH_STATUS status = expand_path(nullptr, path, nullptr, &bytes, 0);
	if (status != pdh_more_data or bytes == 0) {
		close_library();
		return 0;
	}
	std::vector<wchar_t> buffer(bytes);
	status = expand_path(nullptr, path, buffer.data(), &bytes, 0);
	if (status == ERROR_SUCCESS) {
		for (const wchar_t* item = buffer.data(); item != nullptr and *item != L'\0'; item += std::wcslen(item) + 1) ++count;
	}
	close_library();
	return static_cast<int>(count);
}

std::optional<double> processor_queue_length() {
	using PdhOpenQueryWFn = PDH_STATUS (WINAPI *)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
	using PdhAddCounterWFn = PDH_STATUS (WINAPI *)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
	using PdhCollectQueryDataFn = PDH_STATUS (WINAPI *)(PDH_HQUERY);
	using PdhGetFormattedCounterValueFn = PDH_STATUS (WINAPI *)(PDH_HCOUNTER, DWORD, LPDWORD, PPDH_FMT_COUNTERVALUE);
	using PdhCloseQueryFn = PDH_STATUS (WINAPI *)(PDH_HQUERY);

	HMODULE pdh = LoadLibraryW(L"pdh.dll");
	if (pdh == nullptr) return std::nullopt;
	auto close_library = [&] { FreeLibrary(pdh); };
	const auto open_query = windows_function<PdhOpenQueryWFn>(pdh, "PdhOpenQueryW");
	const auto add_english_counter = windows_function<PdhAddCounterWFn>(pdh, "PdhAddEnglishCounterW");
	const auto add_counter = windows_function<PdhAddCounterWFn>(pdh, "PdhAddCounterW");
	const auto collect_query = windows_function<PdhCollectQueryDataFn>(pdh, "PdhCollectQueryData");
	const auto get_value = windows_function<PdhGetFormattedCounterValueFn>(pdh, "PdhGetFormattedCounterValue");
	const auto close_query = windows_function<PdhCloseQueryFn>(pdh, "PdhCloseQuery");
	if (open_query == nullptr or add_counter == nullptr or collect_query == nullptr or get_value == nullptr or close_query == nullptr) {
		close_library();
		return std::nullopt;
	}

	PDH_HQUERY query{};
	PDH_HCOUNTER counter{};
	if (open_query(nullptr, 0, &query) != ERROR_SUCCESS) {
		close_library();
		return std::nullopt;
	}
	PDH_STATUS status = add_english_counter != nullptr ? add_english_counter(query, L"\\System\\Processor Queue Length", 0, &counter) : ERROR_INVALID_PARAMETER;
	if (status != ERROR_SUCCESS) status = add_counter(query, L"\\System\\Processor Queue Length", 0, &counter);
	if (status != ERROR_SUCCESS) {
		close_query(query);
		close_library();
		return std::nullopt;
	}

	std::optional<double> result;
	if (collect_query(query) == ERROR_SUCCESS) {
		PDH_FMT_COUNTERVALUE value{};
		if (get_value(counter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS
		and WindowsPdh::valid_counter_status(value.CStatus)) result = std::max(0.0, value.doubleValue);
	}
	close_query(query);
	close_library();
	return result;
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

std::vector<cpu_times> core_cpu_times(const DWORD logical_processors) {
	std::vector<cpu_times> result;
	auto query = ntdll_function<decltype(&NtQuerySystemInformation)>("NtQuerySystemInformation");
	if (query == nullptr) return result;
	std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> info(std::max<DWORD>(1, logical_processors));
	ULONG returned = 0;
	const auto bytes = static_cast<ULONG>(info.size() * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
	if (query(SystemProcessorPerformanceInformation, info.data(), bytes, &returned) < 0) return result;
	const int count = std::min<int>(static_cast<int>(info.size()), returned / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
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

struct thermal_summary {
	int samples{};
	long long min{};
	long long max{};
	long long critical{};
};

struct power_meter_summary {
	int samples{};
	double watts{};
};
std::optional<long long> variant_to_i64(const VARIANT& value) {
	switch (value.vt) {
		case VT_I1: return value.cVal;
		case VT_UI1: return value.bVal;
		case VT_I2: return value.iVal;
		case VT_UI2: return value.uiVal;
		case VT_I4:
		case VT_INT: return value.lVal;
		case VT_UI4:
		case VT_UINT: return static_cast<long long>(value.ulVal);
		case VT_I8: return value.llVal;
		case VT_UI8: return static_cast<long long>(value.ullVal);
		default: return std::nullopt;
	}
}

std::optional<std::string> variant_to_string(const VARIANT& value) {
	if (value.vt == VT_BSTR and value.bstrVal != nullptr) return wide_to_utf8(value.bstrVal);
	return std::nullopt;
}

std::optional<double> variant_to_double(const VARIANT& value) {
	switch (value.vt) {
		case VT_R4: return value.fltVal;
		case VT_R8: return value.dblVal;
		default: {
			const auto integer = variant_to_i64(value);
			return integer ? std::optional<double>{static_cast<double>(*integer)} : std::nullopt;
		}
	}
}

struct libre_hardware_summary {
	bool available{};
	int sensors{};
	int cpu_temperature_samples{};
	int cpu_core_temperature_samples{};
	int cpu_power_samples{};
	int gpu_temperature_samples{};
	int gpu_power_samples{};
	int gpu_clock_samples{};
};

libre_hardware_summary read_libre_hardware_summary() {
	libre_hardware_summary summary;
	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool should_uninitialize = SUCCEEDED(init);
	if (FAILED(init) and init != RPC_E_CHANGED_MODE) return summary;

	const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
	if (FAILED(security) and security != RPC_E_TOO_LATE) {
		if (should_uninitialize) CoUninitialize();
		return summary;
	}

	IWbemLocator* locator{};
	IWbemServices* services{};
	IEnumWbemClassObject* enumerator{};
	auto cleanup = [&] {
		if (enumerator != nullptr) enumerator->Release();
		if (services != nullptr) services->Release();
		if (locator != nullptr) locator->Release();
		if (should_uninitialize) CoUninitialize();
	};

	CLSID wbem_locator_clsid{};
	IID wbem_locator_iid{};
	HRESULT hr = CLSIDFromString(const_cast<LPOLESTR>(L"{4590F811-1D3A-11D0-891F-00AA004B2E24}"), &wbem_locator_clsid);
	if (SUCCEEDED(hr)) hr = IIDFromString(const_cast<LPOLESTR>(L"{DC12A687-737F-11CF-884D-00AA004B2E24}"), &wbem_locator_iid);
	if (SUCCEEDED(hr)) hr = CoCreateInstance(wbem_locator_clsid, nullptr, CLSCTX_INPROC_SERVER, wbem_locator_iid, reinterpret_cast<void**>(&locator));
	if (FAILED(hr) or locator == nullptr) {
		cleanup();
		return summary;
	}

	BSTR namespace_path = SysAllocString(L"ROOT\\LibreHardwareMonitor");
	hr = locator->ConnectServer(namespace_path, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
	SysFreeString(namespace_path);
	if (FAILED(hr) or services == nullptr) {
		cleanup();
		return summary;
	}
	if (FAILED(CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) {
		cleanup();
		return summary;
	}

	BSTR query_language = SysAllocString(L"WQL");
	BSTR query = SysAllocString(L"SELECT Identifier, Parent, Name, SensorType, Value FROM Sensor");
	hr = services->ExecQuery(query_language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
	SysFreeString(query);
	SysFreeString(query_language);
	if (FAILED(hr) or enumerator == nullptr) {
		cleanup();
		return summary;
	}
	summary.available = true;

	IWbemClassObject* object{};
	ULONG returned = 0;
	while (enumerator->Next(static_cast<LONG>(-1), 1, &object, &returned) == WBEM_S_NO_ERROR and returned > 0 and object != nullptr) {
		VARIANT identifier{};
		VARIANT parent{};
		VARIANT name{};
		VARIANT type{};
		VARIANT value{};
		const auto got_identifier = SUCCEEDED(object->Get(L"Identifier", 0, &identifier, nullptr, nullptr)) ? variant_to_string(identifier) : std::nullopt;
		const auto got_parent = SUCCEEDED(object->Get(L"Parent", 0, &parent, nullptr, nullptr)) ? variant_to_string(parent) : std::nullopt;
		const auto got_name = SUCCEEDED(object->Get(L"Name", 0, &name, nullptr, nullptr)) ? variant_to_string(name) : std::nullopt;
		const auto got_type = SUCCEEDED(object->Get(L"SensorType", 0, &type, nullptr, nullptr)) ? variant_to_string(type) : std::nullopt;
		const auto got_value = SUCCEEDED(object->Get(L"Value", 0, &value, nullptr, nullptr)) ? variant_to_double(value) : std::nullopt;
		VariantClear(&identifier);
		VariantClear(&parent);
		VariantClear(&name);
		VariantClear(&type);
		VariantClear(&value);
		object->Release();
		object = nullptr;
		if (!got_identifier or !got_name or !got_type or !got_value or !std::isfinite(*got_value)) continue;
		++summary.sensors;
		auto id = *got_identifier + " " + got_parent.value_or("");
		auto sensor_name = *got_name;
		auto sensor_type = *got_type;
		std::ranges::transform(id, id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::ranges::transform(sensor_name, sensor_name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::ranges::transform(sensor_type, sensor_type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const bool cpu = id.contains("/intelcpu/") or id.contains("/amdcpu/");
		const bool gpu = id.contains("/nvidiagpu/") or id.contains("/atigpu/") or id.contains("/amdgpu/") or id.contains("/intelgpu/");
		if (cpu and sensor_type == "temperature") {
			++summary.cpu_temperature_samples;
			if (sensor_name.contains("core #") and !sensor_name.contains("max") and !sensor_name.contains("average")) ++summary.cpu_core_temperature_samples;
		}
		if (cpu and sensor_type == "power") ++summary.cpu_power_samples;
		if (gpu and sensor_type == "temperature") ++summary.gpu_temperature_samples;
		if (gpu and sensor_type == "power") ++summary.gpu_power_samples;
		if (gpu and sensor_type == "clock") ++summary.gpu_clock_samples;
	}

	cleanup();
	return summary;
}

std::optional<power_meter_summary> read_power_meters() {
	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool should_uninitialize = SUCCEEDED(init);
	if (FAILED(init) and init != RPC_E_CHANGED_MODE) return std::nullopt;

	const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
	if (FAILED(security) and security != RPC_E_TOO_LATE) {
		if (should_uninitialize) CoUninitialize();
		return std::nullopt;
	}

	IWbemLocator* locator{};
	IWbemServices* services{};
	IEnumWbemClassObject* enumerator{};
	power_meter_summary summary;
	bool got_total = false;
	double total_watts = 0.0;

	auto cleanup = [&] {
		if (enumerator != nullptr) enumerator->Release();
		if (services != nullptr) services->Release();
		if (locator != nullptr) locator->Release();
		if (should_uninitialize) CoUninitialize();
	};

	CLSID wbem_locator_clsid{};
	IID wbem_locator_iid{};
	HRESULT hr = CLSIDFromString(const_cast<LPOLESTR>(L"{4590F811-1D3A-11D0-891F-00AA004B2E24}"), &wbem_locator_clsid);
	if (SUCCEEDED(hr)) hr = IIDFromString(const_cast<LPOLESTR>(L"{DC12A687-737F-11CF-884D-00AA004B2E24}"), &wbem_locator_iid);
	if (SUCCEEDED(hr)) hr = CoCreateInstance(wbem_locator_clsid, nullptr, CLSCTX_INPROC_SERVER, wbem_locator_iid, reinterpret_cast<void**>(&locator));
	if (FAILED(hr) or locator == nullptr) {
		cleanup();
		return std::nullopt;
	}

	BSTR namespace_path = SysAllocString(L"ROOT\\CIMV2");
	hr = locator->ConnectServer(namespace_path, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
	SysFreeString(namespace_path);
	if (FAILED(hr) or services == nullptr) {
		cleanup();
		return std::nullopt;
	}

	CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

	BSTR query_language = SysAllocString(L"WQL");
	BSTR query = SysAllocString(L"SELECT Name, Power FROM Win32_PerfFormattedData_Counters_PowerMeter");
	hr = services->ExecQuery(query_language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
	SysFreeString(query);
	SysFreeString(query_language);
	if (FAILED(hr) or enumerator == nullptr) {
		cleanup();
		return std::nullopt;
	}

	IWbemClassObject* object{};
	ULONG returned = 0;
	while (enumerator->Next(static_cast<LONG>(-1), 1, &object, &returned) == WBEM_S_NO_ERROR and returned > 0 and object != nullptr) {
		VARIANT name{};
		VARIANT power{};
		std::optional<std::string> got_name;
		std::optional<long long> got_power;
		if (SUCCEEDED(object->Get(L"Name", 0, &name, nullptr, nullptr))) got_name = variant_to_string(name);
		if (SUCCEEDED(object->Get(L"Power", 0, &power, nullptr, nullptr))) got_power = variant_to_i64(power);
		VariantClear(&name);
		VariantClear(&power);
		object->Release();
		object = nullptr;
		if (!got_power or *got_power <= 0) continue;
		const double watts = static_cast<double>(*got_power);
		if (got_name and *got_name == "_Total") {
			got_total = true;
			total_watts = watts;
		} else if (!got_total) {
			total_watts += watts;
		}
		++summary.samples;
	}

	cleanup();
	if (summary.samples == 0 or total_watts <= 0.0) return std::nullopt;
	summary.watts = total_watts;
	return summary;
}
std::optional<thermal_summary> read_thermal_zones() {
	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool should_uninitialize = SUCCEEDED(init);
	if (FAILED(init) and init != RPC_E_CHANGED_MODE) return std::nullopt;
	const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
	if (FAILED(security) and security != RPC_E_TOO_LATE) {
		if (should_uninitialize) CoUninitialize();
		return std::nullopt;
	}

	IWbemLocator* locator{};
	IWbemServices* services{};
	IEnumWbemClassObject* enumerator{};
	auto cleanup = [&] {
		if (enumerator != nullptr) enumerator->Release();
		if (services != nullptr) services->Release();
		if (locator != nullptr) locator->Release();
		if (should_uninitialize) CoUninitialize();
	};

	CLSID wbem_locator_clsid{};
	IID wbem_locator_iid{};
	HRESULT hr = CLSIDFromString(const_cast<LPOLESTR>(L"{4590F811-1D3A-11D0-891F-00AA004B2E24}"), &wbem_locator_clsid);
	if (SUCCEEDED(hr)) hr = IIDFromString(const_cast<LPOLESTR>(L"{DC12A687-737F-11CF-884D-00AA004B2E24}"), &wbem_locator_iid);
	if (SUCCEEDED(hr)) hr = CoCreateInstance(wbem_locator_clsid, nullptr, CLSCTX_INPROC_SERVER, wbem_locator_iid, reinterpret_cast<void**>(&locator));
	if (FAILED(hr) or locator == nullptr) {
		cleanup();
		return std::nullopt;
	}
	BSTR namespace_path = SysAllocString(L"ROOT\\WMI");
	hr = locator->ConnectServer(namespace_path, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
	SysFreeString(namespace_path);
	if (FAILED(hr) or services == nullptr) {
		cleanup();
		return std::nullopt;
	}
	CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

	BSTR query_language = SysAllocString(L"WQL");
	BSTR query = SysAllocString(L"SELECT CurrentTemperature, CriticalTripPoint FROM MSAcpi_ThermalZoneTemperature");
	hr = services->ExecQuery(query_language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
	SysFreeString(query);
	SysFreeString(query_language);
	if (FAILED(hr) or enumerator == nullptr) {
		cleanup();
		return std::nullopt;
	}

	thermal_summary summary{};
	IWbemClassObject* object{};
	ULONG returned = 0;
	while (enumerator->Next(static_cast<LONG>(-1), 1, &object, &returned) == WBEM_S_NO_ERROR and returned > 0 and object != nullptr) {
		VARIANT current{};
		VARIANT critical{};
		std::optional<long long> got_current;
		std::optional<long long> got_critical;
		if (SUCCEEDED(object->Get(L"CurrentTemperature", 0, &current, nullptr, nullptr))) got_current = variant_to_i64(current);
		if (SUCCEEDED(object->Get(L"CriticalTripPoint", 0, &critical, nullptr, nullptr))) got_critical = variant_to_i64(critical);
		VariantClear(&current);
		VariantClear(&critical);
		object->Release();
		object = nullptr;
		if (!got_current or *got_current <= 0) continue;
		const long long celsius = std::llround((*got_current / 10.0) - 273.15);
		if (celsius < -20 or celsius > 125) continue;
		if (summary.samples == 0) summary.min = summary.max = celsius;
		else {
			summary.min = std::min(summary.min, celsius);
			summary.max = std::max(summary.max, celsius);
		}
		if (got_critical and *got_critical > 0) {
			const long long critical_celsius = std::llround((*got_critical / 10.0) - 273.15);
			if (critical_celsius >= celsius and critical_celsius <= 130) summary.critical = std::max(summary.critical, critical_celsius);
		}
		++summary.samples;
	}
	cleanup();
	if (summary.samples == 0) return std::nullopt;
	return summary;
}

}

int main() {
	const DWORD logical_processors = WindowsCpu::logical_processor_count();

	MEMORYSTATUSEX mem{};
	mem.dwLength = sizeof(mem);
	const bool got_mem = GlobalMemoryStatusEx(&mem);
	PERFORMANCE_INFORMATION perf{};
	perf.cb = sizeof(perf);
	const bool got_perf = GetPerformanceInfo(&perf, sizeof(perf));
	const auto perf_page_size = got_perf ? static_cast<std::uint64_t>(perf.PageSize) : 0;

	const auto system_before = system_cpu_times();
	const auto cores_before = core_cpu_times(logical_processors);
	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	const auto system_after = system_cpu_times();
	const auto cores_after = core_cpu_times(logical_processors);

	SYSTEM_POWER_STATUS power{};
	const bool got_power = GetSystemPowerStatus(&power);

	std::cout << "windows collector diagnostics\n";
	std::cout << "cpu.logical_processors=" << logical_processors << "\n";
	std::cout << "cpu.processor_groups=" << WindowsCpu::processor_group_count() << "\n";
	std::cout << "cpu.system_time_100ns=" << system_after.total << "\n";
	std::cout << "cpu.sample_total_percent=" << std::lround(cpu_percent(system_before, system_after)) << "\n";
	const auto queue_length = processor_queue_length();
	std::cout << "cpu.load_average_source=pdh_system_processor_queue_length\n";
	std::cout << "cpu.load_average_windows_seconds=60,300,900\n";
	std::cout << "cpu.processor_queue_length=" << (queue_length ? *queue_length : -1.0) << "\n";
    const auto frequencies = cpu_frequencies(logical_processors);
    std::cout << "cpu.frequency_samples=" << (frequencies ? frequencies->samples : 0) << "\n";
    std::cout << "cpu.frequency_current_min_mhz=" << (frequencies ? frequencies->current_min : 0) << "\n";
    std::cout << "cpu.frequency_current_max_mhz=" << (frequencies ? frequencies->current_max : 0) << "\n";
    std::cout << "cpu.frequency_current_avg_mhz=" << (frequencies ? frequencies->current_avg : 0.0) << "\n";
    std::cout << "cpu.frequency_max_mhz=" << (frequencies ? frequencies->max_mhz : 0) << "\n";
	const auto thermal_zones = read_thermal_zones();
	std::cout << "cpu.thermal_zone_samples=" << (thermal_zones ? thermal_zones->samples : 0) << "\n";
	std::cout << "cpu.thermal_zone_min_c=" << (thermal_zones ? thermal_zones->min : 0) << "\n";
	std::cout << "cpu.thermal_zone_max_c=" << (thermal_zones ? thermal_zones->max : 0) << "\n";
	std::cout << "cpu.thermal_zone_critical_c=" << (thermal_zones ? thermal_zones->critical : 0) << "\n";
	const auto power_meters = read_power_meters();
	std::cout << "cpu.power_meter_samples=" << (power_meters ? power_meters->samples : 0) << "\n";
	std::cout << "cpu.power_meter_watts=" << (power_meters ? power_meters->watts : 0.0) << "\n";
	const auto libre_hardware = read_libre_hardware_summary();
	std::cout << "libre_hardware.available=" << (libre_hardware.available ? 1 : 0) << "\n";
	std::cout << "libre_hardware.sensors=" << libre_hardware.sensors << "\n";
	std::cout << "libre_hardware.cpu_temperature_samples=" << libre_hardware.cpu_temperature_samples << "\n";
	std::cout << "libre_hardware.cpu_core_temperature_samples=" << libre_hardware.cpu_core_temperature_samples << "\n";
	std::cout << "libre_hardware.cpu_power_samples=" << libre_hardware.cpu_power_samples << "\n";
	std::cout << "libre_hardware.gpu_temperature_samples=" << libre_hardware.gpu_temperature_samples << "\n";
	std::cout << "libre_hardware.gpu_power_samples=" << libre_hardware.gpu_power_samples << "\n";
	std::cout << "libre_hardware.gpu_clock_samples=" << libre_hardware.gpu_clock_samples << "\n";
	const auto gpu_dxgi = read_dxgi_gpu_summary();
	std::cout << "gpu.dxgi_adapters=" << gpu_dxgi.adapters << "\n";
	std::cout << "gpu.dxgi_hardware_adapters=" << gpu_dxgi.hardware_adapters << "\n";
	std::cout << "gpu.dxgi_software_adapters=" << gpu_dxgi.software_adapters << "\n";
	std::cout << "gpu.dxgi_nvidia=" << gpu_dxgi.nvidia << "\n";
	std::cout << "gpu.dxgi_amd=" << gpu_dxgi.amd << "\n";
	std::cout << "gpu.dxgi_intel=" << gpu_dxgi.intel << "\n";
	std::cout << "gpu.dxgi_dedicated_vram=" << gpu_dxgi.dedicated_vram << "\n";
	std::cout << "gpu.dxgi_shared_system_memory=" << gpu_dxgi.shared_system_memory << "\n";
	std::cout << "gpu.dxgi_names=" << gpu_dxgi.names.size() << "\n";
	std::cout << "gpu.pdh_engine_counters=" << count_pdh_wildcard_paths(L"\\GPU Engine(*)\\Utilization Percentage") << "\n";
	std::cout << "gpu.pdh_dedicated_memory_counters=" << count_pdh_wildcard_paths(L"\\GPU Adapter Memory(*)\\Dedicated Usage") << "\n";
	std::cout << "gpu.pdh_shared_memory_counters=" << count_pdh_wildcard_paths(L"\\GPU Adapter Memory(*)\\Shared Usage") << "\n";
	const auto gpu_nvml = read_nvml_gpu_summary();
	std::cout << "gpu.nvml_library_loaded=" << (gpu_nvml.library_loaded ? 1 : 0) << "\n";
	std::cout << "gpu.nvml_initialized=" << (gpu_nvml.initialized ? 1 : 0) << "\n";
	std::cout << "gpu.nvml_devices=" << gpu_nvml.devices << "\n";
	std::cout << "gpu.nvml_names=" << gpu_nvml.names << "\n";
	std::cout << "gpu.nvml_utilization_samples=" << gpu_nvml.utilization_samples << "\n";
	std::cout << "gpu.nvml_memory_samples=" << gpu_nvml.memory_samples << "\n";
	std::cout << "gpu.nvml_power_samples=" << gpu_nvml.power_samples << "\n";
	std::cout << "gpu.nvml_graphics_clock_samples=" << gpu_nvml.graphics_clock_samples << "\n";
	std::cout << "gpu.nvml_memory_clock_samples=" << gpu_nvml.memory_clock_samples << "\n";
	std::cout << "gpu.nvml_temperature_samples=" << gpu_nvml.temperature_samples << "\n";
	std::cout << "gpu.nvml_encoder_samples=" << gpu_nvml.encoder_samples << "\n";
	std::cout << "gpu.nvml_decoder_samples=" << gpu_nvml.decoder_samples << "\n";
	std::cout << "gpu.nvml_vram_total=" << gpu_nvml.vram_total << "\n";
	std::cout << "gpu.nvml_vram_used=" << gpu_nvml.vram_used << "\n";
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
	const auto memory_lists = got_perf ? read_windows_memory_lists(perf_page_size) : std::nullopt;
	const auto standby_cache = memory_lists ? memory_lists->cache : 0;
	const auto system_cache = got_perf ? static_cast<std::uint64_t>(perf.SystemCache) * perf_page_size : 0;
	const auto physical_total = got_perf ? static_cast<std::uint64_t>(perf.PhysicalTotal) * perf_page_size : (got_mem ? mem.ullTotalPhys : 0);
	const auto physical_available = got_perf ? static_cast<std::uint64_t>(perf.PhysicalAvailable) * perf_page_size : (got_mem ? mem.ullAvailPhys : 0);
	const auto physical = WindowsMemory::physical(physical_total, physical_available,
		memory_lists ? memory_lists->free : physical_available, system_cache, standby_cache);
	std::cout << "memory.total=" << physical_total << "\n";
	std::cout << "memory.available=" << physical.available << "\n";
	std::cout << "memory.free=" << physical.free << "\n";
	std::cout << "memory.used=" << physical.used << "\n";
	std::cout << "memory.system_cache=" << system_cache << "\n";
	std::cout << "memory.standby_cache=" << standby_cache << "\n";
	std::cout << "memory.cached=" << physical.cached << "\n";
	const auto commit_total = got_perf ? static_cast<std::uint64_t>(perf.CommitLimit) * perf_page_size : (got_mem ? mem.ullTotalPageFile : 0);
	const auto commit_used = got_perf ? static_cast<std::uint64_t>(perf.CommitTotal) * perf_page_size : (got_mem ? mem.ullTotalPageFile - mem.ullAvailPageFile : 0);
	const auto estimated_pagefiles = WindowsMemory::estimated_pagefiles(commit_total, commit_used, physical_total);
	const auto actual_pagefiles = WindowsMemory::read_pagefiles();
	const auto pagefiles = actual_pagefiles.value_or(estimated_pagefiles);
	std::cout << "memory.commit_total=" << commit_total << "\n";
	std::cout << "memory.commit_used=" << commit_used << "\n";
	std::cout << "memory.commit_free=" << (commit_total > commit_used ? commit_total - commit_used : 0) << "\n";
	std::cout << "memory.pagefile_provider=" << (actual_pagefiles ? "wmi" : "commit-estimate") << "\n";
	std::cout << "memory.pagefile_files=" << pagefiles.files << "\n";
	std::cout << "memory.pagefile_total=" << pagefiles.total << "\n";
	std::cout << "memory.pagefile_used=" << pagefiles.used << "\n";
	std::cout << "memory.pagefile_free=" << pagefiles.free << "\n";
	const auto disk_types = count_disk_types();
	std::cout << "disk.count=" << count_logical_disks() << "\n";
	std::cout << "disk.fixed=" << disk_types.fixed << "\n";
	std::cout << "disk.removable=" << disk_types.removable << "\n";
	std::cout << "disk.remote=" << disk_types.remote << "\n";
	std::cout << "disk.ramdisk=" << disk_types.ramdisk << "\n";
	std::cout << "disk.optical=" << disk_types.optical << "\n";
	const auto disk_policy = count_disk_policy_volumes();
	std::cout << "disk.policy_physical_visible=" << disk_policy.physical_visible << "\n";
	std::cout << "disk.policy_all_visible=" << disk_policy.all_visible << "\n";
	std::cout << "disk.policy_readable_network=" << disk_policy.readable_network << "\n";
	const auto disk_io = count_disk_performance_volumes();
	std::cout << "disk.io_candidates=" << disk_io.candidates << "\n";
	std::cout << "disk.io_readable=" << disk_io.readable << "\n";
	std::cout << "disk.io_idle_time_samples=" << disk_io.idle_time_samples << "\n";
	const auto disk_pdh = count_disk_pdh_volumes();
	std::cout << "disk.pdh_candidates=" << disk_pdh.candidates << "\n";
	std::cout << "disk.pdh_queries=" << disk_pdh.queries << "\n";
	std::cout << "disk.pdh_readable=" << disk_pdh.readable << "\n";
	std::cout << "disk.pdh_activity_samples=" << disk_pdh.activity_samples << "\n";
	std::cout << "disk.pdh_read_bytes=" << disk_pdh.read_bytes << "\n";
	std::cout << "disk.pdh_write_bytes=" << disk_pdh.write_bytes << "\n";
	const auto network_metadata = read_network_metadata_summary();
	std::cout << "network.adapters=" << count_network_adapters() << "\n";
	std::cout << "network.if_table2_rows=" << count_network_if_table2_rows() << "\n";
	std::cout << "network.metadata_adapters=" << network_metadata.adapters << "\n";
	std::cout << "network.friendly_names=" << network_metadata.friendly_names << "\n";
	std::cout << "network.addresses=" << network_metadata.addresses << "\n";
	std::cout << "network.link_speeds=" << network_metadata.link_speeds << "\n";
	const auto [readable_command_lines, command_lines_with_args] = count_process_command_lines();
	std::cout << "process.count=" << count_processes() << "\n";
	std::cout << "process.command_lines_readable=" << readable_command_lines << "\n";
	std::cout << "process.command_lines_with_args=" << command_lines_with_args << "\n";
	std::cout << "process.memory_readable=" << count_process_memory_readable(PROCESS_QUERY_LIMITED_INFORMATION) << "\n";
	std::cout << "process.memory_readable_vm_read=" << count_process_memory_readable(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ) << "\n";
	const auto process_states = read_process_state_summary();
	std::cout << "process.state_query_processes=" << (process_states ? process_states->processes : 0) << "\n";
	std::cout << "process.state_query_threads=" << (process_states ? process_states->threads : 0) << "\n";
	std::cout << "process.snapshot_named=" << (process_states ? process_states->named_processes : 0) << "\n";
	std::cout << "process.snapshot_working_set=" << (process_states ? process_states->working_set_processes : 0) << "\n";
	std::cout << "process.snapshot_cpu_time=" << (process_states ? process_states->cpu_time_processes : 0) << "\n";
	std::cout << "process.snapshot_creation_time=" << (process_states ? process_states->creation_time_processes : 0) << "\n";
	std::cout << "process.snapshot_io=" << (process_states ? process_states->io_processes : 0) << "\n";
	std::cout << "process.state_runnable_threads=" << (process_states ? process_states->runnable_threads : 0) << "\n";
	std::cout << "process.state_waiting_threads=" << (process_states ? process_states->waiting_threads : 0) << "\n";
	std::cout << "process.state_suspended_threads=" << (process_states ? process_states->suspended_threads : 0) << "\n";
	std::cout << "process.state_transition_threads=" << (process_states ? process_states->transition_threads : 0) << "\n";
	std::cout << "battery.present=" << (got_power and power.BatteryFlag != 128 and power.BatteryLifePercent != 255 ? 1 : 0) << "\n";
	return 0;
}
