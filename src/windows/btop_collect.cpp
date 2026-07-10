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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
#include <sddl.h>
#include <powerbase.h>
#include <winternl.h>

#include <fmt/format.h>

#include "../btop_config.hpp"
#include "../btop_log.hpp"
#include "../btop_shared.hpp"
#include "../btop_tools.hpp"
#include "disk_helpers.hpp"
#include "cpu_helpers.hpp"
#include "gpu_helpers.hpp"
#include "hardware_sensors.hpp"
#include "memory_helpers.hpp"
#include "memory_wmi.hpp"
#include "network_helpers.hpp"
#include "pdh_helpers.hpp"
#include "process_helpers.hpp"

namespace fs = std::filesystem;
namespace rng = std::ranges;
using namespace Tools;
using std::clamp;
using std::max;
using std::min;
using std::round;
using std::string;
using std::wstring_view;
using std::vector;

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

string wide_to_utf8(const wstring_view input) {
	if (input.empty()) return {};
	const int needed = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
	if (needed <= 0) return {};
	string out(needed, '\0');
	WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), needed, nullptr, nullptr);
	return out;
}

void push_limited(std::deque<long long>& values, long long value, size_t limit) {
	values.push_back(value);
	while (values.size() > limit) values.pop_front();
}

struct adapter_metadata {
	string name;
	vector<string> aliases;
	string ipv4;
	string ipv6;
	uint64_t link_speed{};
	bool connected{};
};

void add_adapter_alias(vector<string>& aliases, const string& alias) {
	const string normalized = (string)trim(alias);
	if (!normalized.empty() and !v_contains(aliases, normalized)) aliases.push_back(normalized);
}

string sockaddr_to_string(const SOCKADDR* address, const int length) {
	static const bool winsock_ready = [] {
		WSADATA data{};
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	char host[NI_MAXHOST]{};
	if (!winsock_ready or address == nullptr) return {};
	if (getnameinfo(address, length, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0) return {};
	return host;
}

std::unordered_map<DWORD, adapter_metadata> read_adapter_metadata() {
	std::unordered_map<DWORD, adapter_metadata> metadata;
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG bytes = 15 * 1024;
	vector<std::byte> buffer(bytes);
	ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.assign(bytes, {});
		result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
	}
	if (result != NO_ERROR) return metadata;

	for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr; adapter = adapter->Next) {
		if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		adapter_metadata info;
		const string friendly_name = wide_to_utf8(adapter->FriendlyName);
		const string description = wide_to_utf8(adapter->Description);
		const string adapter_name = adapter->AdapterName != nullptr ? string{adapter->AdapterName} : string{};
		add_adapter_alias(info.aliases, friendly_name);
		add_adapter_alias(info.aliases, description);
		add_adapter_alias(info.aliases, adapter_name);
		if (adapter->IfIndex != 0) {
			add_adapter_alias(info.aliases, fmt::format("if{}", adapter->IfIndex));
			add_adapter_alias(info.aliases, fmt::format("{}", adapter->IfIndex));
		}
		if (adapter->Ipv6IfIndex != 0) {
			add_adapter_alias(info.aliases, fmt::format("if{}", adapter->Ipv6IfIndex));
			add_adapter_alias(info.aliases, fmt::format("{}", adapter->Ipv6IfIndex));
		}
		info.name = !friendly_name.empty() ? friendly_name : (!description.empty() ? description : adapter_name);
		if (info.name.empty()) info.name = fmt::format("if{}", adapter->IfIndex);
		info.connected = adapter->OperStatus == IfOperStatusUp;
		info.link_speed = max<uint64_t>(adapter->ReceiveLinkSpeed, adapter->TransmitLinkSpeed) / 8;

		for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
			if (unicast->Address.lpSockaddr == nullptr) continue;
			if (unicast->DadState == IpDadStateInvalid or unicast->DadState == IpDadStateTentative
					or unicast->DadState == IpDadStateDuplicate) continue;
			const auto family = unicast->Address.lpSockaddr->sa_family;
			const auto address = sockaddr_to_string(unicast->Address.lpSockaddr, static_cast<int>(unicast->Address.iSockaddrLength));
			if (address.empty()) continue;
			if (family == AF_INET) WindowsNetwork::prefer_address(info.ipv4, address, false);
			else if (family == AF_INET6) WindowsNetwork::prefer_address(info.ipv6, address, true);
		}

		if (adapter->IfIndex != 0) metadata[adapter->IfIndex] = info;
		if (adapter->Ipv6IfIndex != 0) metadata[adapter->Ipv6IfIndex] = info;
	}
	return metadata;
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

struct thermal_sample {
	long long current{};
	long long critical{};
};

struct power_meter_sample {
	int samples{};
	double watts{};
};

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
	uint64_t free{};
	uint64_t cache{};
};

std::optional<windows_memory_list_sample> read_windows_memory_lists(const uint64_t page_size) {
	using NtQuerySystemInformationFn = NTSTATUS (WINAPI *)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
	const auto query = ntdll_function<NtQuerySystemInformationFn>("NtQuerySystemInformation");
	if (query == nullptr or page_size == 0) return std::nullopt;

	system_memory_list_information info{};
	constexpr auto SystemMemoryListInformation = static_cast<SYSTEM_INFORMATION_CLASS>(80);
	if (query(SystemMemoryListInformation, &info, sizeof(info), nullptr) < 0) return std::nullopt;

	uint64_t standby_pages = 0;
	for (const auto pages : info.page_count_by_priority) standby_pages += static_cast<uint64_t>(pages);
	const auto modified_pages = static_cast<uint64_t>(info.modified_page_count) + static_cast<uint64_t>(info.modified_no_write_page_count);
	const auto free_pages = static_cast<uint64_t>(info.zero_page_count) + static_cast<uint64_t>(info.free_page_count);
	return windows_memory_list_sample{free_pages * page_size, (standby_pages + modified_pages) * page_size};
}

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

std::optional<string> variant_to_string(const VARIANT& value) {
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

struct libre_hardware_snapshot {
	bool available{};
	vector<WindowsSensors::sensor> sensors;
};

libre_hardware_snapshot read_libre_hardware_snapshot() {
	static std::mutex cache_mutex;
	static libre_hardware_snapshot cache;
	static uint64_t last_attempt = 0;
	std::scoped_lock lock(cache_mutex);
	const auto now = time_ms();
	const uint64_t refresh_ms = cache.available ? 500 : 5000;
	if (last_attempt != 0 and now - last_attempt < refresh_ms) return cache;
	last_attempt = now;
	cache = {};

	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool should_uninitialize = SUCCEEDED(init);
	if (FAILED(init) and init != RPC_E_CHANGED_MODE) return cache;

	static bool security_called = false;
	if (!security_called) {
		const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
		if (FAILED(security) and security != RPC_E_TOO_LATE) {
			if (should_uninitialize) CoUninitialize();
			return cache;
		}
		security_called = true;
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
		return cache;
	}

	BSTR namespace_path = SysAllocString(L"ROOT\\LibreHardwareMonitor");
	hr = locator->ConnectServer(namespace_path, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
	SysFreeString(namespace_path);
	if (FAILED(hr) or services == nullptr) {
		cleanup();
		return cache;
	}
	if (FAILED(CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) {
		cleanup();
		return cache;
	}

	BSTR query_language = SysAllocString(L"WQL");
	BSTR query = SysAllocString(L"SELECT Identifier, Parent, Name, SensorType, Value FROM Sensor");
	hr = services->ExecQuery(query_language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
	SysFreeString(query);
	SysFreeString(query_language);
	if (FAILED(hr) or enumerator == nullptr) {
		cleanup();
		return cache;
	}

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
		cache.sensors.push_back({*got_identifier, got_parent.value_or(""), *got_name, *got_type, *got_value});
	}

	cache.available = true;
	cleanup();
	return cache;
}

WindowsSensors::cpu_sample read_libre_cpu_sample() {
	const auto snapshot = read_libre_hardware_snapshot();
	return WindowsSensors::aggregate_cpu(snapshot.sensors, snapshot.available);
}

std::optional<power_meter_sample> read_windows_power_meter() {
	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool should_uninitialize = SUCCEEDED(init);
	if (FAILED(init) and init != RPC_E_CHANGED_MODE) return std::nullopt;

	static bool security_called = false;
	if (!security_called) {
		const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
		if (FAILED(security) and security != RPC_E_TOO_LATE) {
			if (should_uninitialize) CoUninitialize();
			return std::nullopt;
		}
		security_called = true;
	}

	IWbemLocator* locator{};
	IWbemServices* services{};
	IEnumWbemClassObject* enumerator{};
	power_meter_sample sample;
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
		std::optional<string> got_name;
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
		++sample.samples;
	}

	cleanup();
	if (sample.samples == 0 or total_watts <= 0.0) return std::nullopt;
	sample.watts = total_watts;
	return sample;
}
std::optional<thermal_sample> read_windows_thermal_zone() {
	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool should_uninitialize = SUCCEEDED(init);
	if (FAILED(init) and init != RPC_E_CHANGED_MODE) return std::nullopt;

	static bool security_called = false;
	if (!security_called) {
		const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
		if (FAILED(security) and security != RPC_E_TOO_LATE) {
			if (should_uninitialize) CoUninitialize();
			return std::nullopt;
		}
		security_called = true;
	}

	IWbemLocator* locator{};
	IWbemServices* services{};
	IEnumWbemClassObject* enumerator{};
	std::optional<thermal_sample> sample;

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
		const long long celsius = llround((*got_current / 10.0) - 273.15);
		if (celsius < -20 or celsius > 125) continue;
		thermal_sample next{celsius, 100};
		if (got_critical and *got_critical > 0) {
			const long long critical_celsius = llround((*got_critical / 10.0) - 273.15);
			if (critical_celsius >= celsius and critical_celsius <= 130) next.critical = critical_celsius;
		}
		if (!sample or next.current > sample->current) sample = next;
	}

	cleanup();
	return sample;
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

string process_command_line(HANDLE process) {
	using NtQueryInformationProcessFn = NTSTATUS (WINAPI *)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
	const auto query = ntdll_function<NtQueryInformationProcessFn>("NtQueryInformationProcess");
	if (query == nullptr) return {};

	ULONG bytes = 0;
	constexpr auto ProcessCommandLineInformation = static_cast<PROCESSINFOCLASS>(60);
	query(process, ProcessCommandLineInformation, nullptr, 0, &bytes);
	if (bytes < sizeof(UNICODE_STRING)) return {};

	vector<std::byte> buffer(bytes);
	if (query(process, ProcessCommandLineInformation, buffer.data(), bytes, &bytes) < 0) return {};
	auto* command = reinterpret_cast<UNICODE_STRING*>(buffer.data());
	if (command->Buffer == nullptr or command->Length == 0) return {};
	return wide_to_utf8(wstring_view(command->Buffer, command->Length / sizeof(wchar_t)));
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

char aggregate_process_state(const SYSTEM_THREAD_INFORMATION* threads, const ULONG count) {
	if (threads == nullptr or count == 0) return 'S';
	bool has_live_thread = false;
	bool has_waiting_thread = false;
	bool all_live_threads_suspended = true;
	bool has_memory_wait = false;
	bool has_transition = false;

	for (ULONG i = 0; i < count; ++i) {
		const ULONG state = threads[i].ThreadState;
		const ULONG reason = threads[i].WaitReason;
		if (state == 4) continue; // Terminated
		has_live_thread = true;

		// Ready, running, standby, or deferred-ready threads make the process runnable.
		if (state == 1 or state == 2 or state == 3 or state == 7) return 'R';
		if (state == 5 or state == 8) { // Waiting or gate wait
			has_waiting_thread = true;
			const bool suspended = reason == 5 or reason == 12;
			all_live_threads_suspended = all_live_threads_suspended and suspended;
			if (reason == 1 or reason == 2 or reason == 3 or reason == 8 or reason == 9 or reason == 10 or reason == 18 or reason == 19) {
				has_memory_wait = true;
			}
		}
		else {
			all_live_threads_suspended = false;
			if (state == 6 or state == 9) has_transition = true;
		}
	}

	if (not has_live_thread) return 'X';
	if (has_waiting_thread and all_live_threads_suspended) return 'T';
	if (has_memory_wait or has_transition) return 'D';
	return 'S';
}

struct process_snapshot {
	string name;
	size_t ppid{};
	size_t threads{};
	char state{'S'};
	uint64_t working_set{};
	uint64_t private_bytes{};
	uint64_t cpu_time_100ns{};
	uint64_t create_time_100ns{};
	uint64_t io_read{};
	uint64_t io_write{};
};

uint64_t nonnegative_large_integer(const LARGE_INTEGER value) {
	return value.QuadPart > 0 ? static_cast<uint64_t>(value.QuadPart) : 0;
}

std::unordered_map<size_t, process_snapshot> query_process_snapshots() {
	using NtQuerySystemInformationFn = NTSTATUS (WINAPI *)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
	const auto query = ntdll_function<NtQuerySystemInformationFn>("NtQuerySystemInformation");
	if (query == nullptr) return {};

	constexpr NTSTATUS status_info_length_mismatch = static_cast<NTSTATUS>(0xC0000004UL);
	ULONG bytes = 1U << 20;
	vector<std::byte> buffer(bytes);
	NTSTATUS status{};
	for (int attempt = 0; attempt < 5; ++attempt) {
		ULONG needed = 0;
		status = query(SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
		if (status >= 0) break;
		if (status != status_info_length_mismatch) return {};
		bytes = max<ULONG>(static_cast<ULONG>(buffer.size() * 2), needed + (64U << 10));
		buffer.resize(bytes);
	}
	if (status < 0) return {};

	std::unordered_map<size_t, process_snapshot> snapshots;
	auto* cursor = buffer.data();
	while (true) {
		auto* process = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(cursor);
		auto* threads = reinterpret_cast<SYSTEM_THREAD_INFORMATION*>(process + 1);
		const auto pid = static_cast<size_t>(reinterpret_cast<ULONG_PTR>(process->UniqueProcessId));
		auto& snapshot = snapshots[pid];
		if (process->ImageName.Buffer != nullptr and process->ImageName.Length > 0) {
			snapshot.name = wide_to_utf8(wstring_view(process->ImageName.Buffer, process->ImageName.Length / sizeof(wchar_t)));
		}
		snapshot.ppid = static_cast<size_t>(reinterpret_cast<ULONG_PTR>(process->InheritedFromUniqueProcessId));
		snapshot.threads = process->NumberOfThreads;
		snapshot.state = aggregate_process_state(threads, process->NumberOfThreads);
		snapshot.working_set = static_cast<uint64_t>(process->VirtualMemoryCounters.WorkingSetSize);
		snapshot.private_bytes = static_cast<uint64_t>(process->PrivatePageCount);
		snapshot.cpu_time_100ns = nonnegative_large_integer(process->KernelTime) + nonnegative_large_integer(process->UserTime);
		snapshot.create_time_100ns = nonnegative_large_integer(process->CreateTime);
		snapshot.io_read = static_cast<uint64_t>(process->IoCounters.ReadTransferCount);
		snapshot.io_write = static_cast<uint64_t>(process->IoCounters.WriteTransferCount);
		if (process->NextEntryOffset == 0) break;
		cursor += process->NextEntryOffset;
	}
	return snapshots;
}
}

#if defined(GPU_SUPPORT)
namespace Gpu {
	vector<gpu_info> gpus;

	namespace {
		using nvmlDevice_t = void*;
		using nvmlReturn_t = int;
		constexpr nvmlReturn_t NVML_SUCCESS = 0;
		constexpr unsigned int NVML_CLOCK_GRAPHICS = 0;
		constexpr unsigned int NVML_CLOCK_MEM = 2;
		constexpr unsigned int NVML_TEMPERATURE_GPU = 0;

		struct dxgi_adapter_info {
			string name;
			string luid_key;
			uint32_t vendor_id{};
			uint64_t dedicated_vram{};
			uint64_t shared_system_memory{};
		};

		struct nvmlUtilization_t {
			unsigned int gpu;
			unsigned int memory;
		};

		struct nvmlMemory_t {
			unsigned long long total;
			unsigned long long free;
			unsigned long long used;
		};

		HMODULE nvml_library = nullptr;
		bool dxgi_init_attempted = false;
		bool nvml_init_attempted = false;
		bool nvml_initialized = false;
		vector<dxgi_adapter_info> dxgi_adapters;
		vector<nvmlDevice_t> nvml_devices;
		vector<size_t> nvml_gpu_indices;

		using nvmlInit_v2_t = nvmlReturn_t (*)();
		using nvmlShutdown_t = nvmlReturn_t (*)();
		using nvmlDeviceGetCount_v2_t = nvmlReturn_t (*)(unsigned int*);
		using nvmlDeviceGetHandleByIndex_v2_t = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
		using nvmlDeviceGetName_t = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
		using nvmlDeviceGetUtilizationRates_t = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
		using nvmlDeviceGetMemoryInfo_t = nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_t*);
		using nvmlDeviceGetPowerUsage_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
		using nvmlDeviceGetPowerManagementLimit_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
		using nvmlDeviceGetClockInfo_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);
		using nvmlDeviceGetTemperature_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);
		using nvmlDeviceGetEncoderUtilization_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, unsigned int*);
		using nvmlDeviceGetDecoderUtilization_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, unsigned int*);

		nvmlInit_v2_t nvmlInit_v2 = nullptr;
		nvmlShutdown_t nvmlShutdown = nullptr;
		nvmlDeviceGetCount_v2_t nvmlDeviceGetCount_v2 = nullptr;
		nvmlDeviceGetHandleByIndex_v2_t nvmlDeviceGetHandleByIndex_v2 = nullptr;
		nvmlDeviceGetName_t nvmlDeviceGetName = nullptr;
		nvmlDeviceGetUtilizationRates_t nvmlDeviceGetUtilizationRates = nullptr;
		nvmlDeviceGetMemoryInfo_t nvmlDeviceGetMemoryInfo = nullptr;
		nvmlDeviceGetPowerUsage_t nvmlDeviceGetPowerUsage = nullptr;
		nvmlDeviceGetPowerManagementLimit_t nvmlDeviceGetPowerManagementLimit = nullptr;
		nvmlDeviceGetClockInfo_t nvmlDeviceGetClockInfo = nullptr;
		nvmlDeviceGetTemperature_t nvmlDeviceGetTemperature = nullptr;
		nvmlDeviceGetEncoderUtilization_t nvmlDeviceGetEncoderUtilization = nullptr;
		nvmlDeviceGetDecoderUtilization_t nvmlDeviceGetDecoderUtilization = nullptr;

		auto trim_gpu_name(string value) -> string {
			auto last = value.find_last_not_of(" \t\r\n\0", string::npos, 5);
			if (last == string::npos) return {};
			value.erase(last + 1);
			auto first = value.find_first_not_of(" \t\r\n\0", 0, 5);
			if (first != string::npos and first > 0) value.erase(0, first);
			return value;
		}

		auto to_lower_copy(string value) -> string {
			rng::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		auto luid_key(const LUID& luid) -> string {
			return fmt::format("luid_0x{:08x}_0x{:08x}", static_cast<uint32_t>(luid.HighPart), luid.LowPart);
		}

		auto is_nvidia_vendor(uint32_t vendor_id) -> bool {
			return vendor_id == 0x10DE;
		}

		auto make_dxgi_gpu(const dxgi_adapter_info& adapter) -> gpu_info {
			gpu_info gpu{};
			gpu.gpu_clock_speed = 0;
			gpu.pwr_usage = 0;
			gpu.pwr_state = 0;
			const auto memory = WindowsGpu::select_memory(adapter.dedicated_vram, adapter.shared_system_memory, -1, -1);
			gpu.mem_total = static_cast<long long>(memory.total);
			gpu.mem_used = 0;
			gpu.mem_shared = memory.shared;
			gpu.mem_clock_speed = 0;
			gpu.pcie_tx = 0;
			gpu.pcie_rx = 0;
			gpu.encoder_utilization = 0;
			gpu.decoder_utilization = 0;
			gpu.supported_functions.gpu_utilization = false;
			gpu.supported_functions.mem_utilization = false;
			gpu.supported_functions.gpu_clock = false;
			gpu.supported_functions.mem_clock = false;
			gpu.supported_functions.pwr_usage = false;
			gpu.supported_functions.pwr_state = false;
			gpu.supported_functions.temp_info = false;
			gpu.supported_functions.mem_total = memory.total > 0;
			gpu.supported_functions.mem_used = false;
			gpu.supported_functions.pcie_txrx = false;
			gpu.supported_functions.encoder_utilization = false;
			gpu.supported_functions.decoder_utilization = false;
			gpu.gpu_percent.at("gpu-totals").push_back(0);
			gpu.gpu_percent.at("gpu-vram-totals").push_back(0);
			gpu.gpu_percent.at("gpu-pwr-totals").push_back(0);
			return gpu;
		}

		template <typename T>
		auto load_nvml_proc(const char* name) -> T {
			return windows_function<T>(nvml_library, name);
		}

		auto load_nvml_library() -> bool {
			if (nvml_library != nullptr) return true;
			nvml_library = LoadLibraryW(L"nvml.dll");
			if (nvml_library == nullptr) nvml_library = LoadLibraryW(L"nvml64.dll");
			return nvml_library != nullptr;
		}
	}

	namespace Dxgi {
		auto init() -> bool {
			if (dxgi_init_attempted) return !dxgi_adapters.empty();
			dxgi_init_attempted = true;

			HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
			if (dxgi == nullptr) return false;
			auto close_library = [&] { FreeLibrary(dxgi); };
			using CreateDXGIFactory1Fn = HRESULT (WINAPI *)(REFIID, void**);
			const auto create_factory = windows_function<CreateDXGIFactory1Fn>(dxgi, "CreateDXGIFactory1");
			if (create_factory == nullptr) {
				close_library();
				return false;
			}

			IID factory_iid{};
			if (FAILED(IIDFromString(const_cast<LPOLESTR>(L"{770AAE78-F26F-4DBA-A829-253C83D1B387}"), &factory_iid))) {
				close_library();
				return false;
			}

			IDXGIFactory1* factory{};
			if (FAILED(create_factory(factory_iid, reinterpret_cast<void**>(&factory))) or factory == nullptr) {
				close_library();
				return false;
			}

			for (UINT index = 0;; ++index) {
				IDXGIAdapter1* adapter{};
				const HRESULT result = factory->EnumAdapters1(index, &adapter);
				if (result == DXGI_ERROR_NOT_FOUND) break;
				if (FAILED(result) or adapter == nullptr) continue;
				DXGI_ADAPTER_DESC1 desc{};
				if (SUCCEEDED(adapter->GetDesc1(&desc)) and (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
					dxgi_adapter_info info;
					info.name = trim_gpu_name(wide_to_utf8(desc.Description));
					if (info.name.empty()) info.name = fmt::format("GPU {}", dxgi_adapters.size() + 1);
					info.luid_key = luid_key(desc.AdapterLuid);
					info.vendor_id = desc.VendorId;
					info.dedicated_vram = static_cast<uint64_t>(desc.DedicatedVideoMemory);
					info.shared_system_memory = static_cast<uint64_t>(desc.SharedSystemMemory);
					dxgi_adapters.push_back(info);
					gpus.push_back(make_dxgi_gpu(info));
					gpu_names.push_back(info.name);
				}
				adapter->Release();
			}

			factory->Release();
			close_library();
			count = static_cast<int>(gpus.size());
			return !dxgi_adapters.empty();
		}
	}

	namespace PdhGpu {
		struct adapter_sample {
			long long utilization = -1;
			long long encoder = -1;
			long long decoder = -1;
			long long dedicated_memory = -1;
			long long shared_memory = -1;
		};

		struct pdh_double_item {
			string name;
			double value{};
		};

		struct pdh_large_item {
			string name;
			long long value{};
		};

		constexpr PDH_STATUS pdh_more_data = static_cast<PDH_STATUS>(0x800007D2);
		bool init_attempted = false;
		bool initialized = false;
		PDH_HQUERY query{};
		PDH_HCOUNTER engine_counter{};
		PDH_HCOUNTER dedicated_memory_counter{};
		PDH_HCOUNTER shared_memory_counter{};
		bool has_engine_counter = false;
		bool has_dedicated_memory_counter = false;
		bool has_shared_memory_counter = false;

		auto add_english_counter(const wchar_t* path, PDH_HCOUNTER& counter) -> bool {
			if (PdhAddEnglishCounterW(query, path, 0, &counter) == ERROR_SUCCESS) return true;
			return PdhAddCounterW(query, path, 0, &counter) == ERROR_SUCCESS;
		}

		auto init() -> bool {
			if (initialized) return true;
			if (init_attempted) return false;
			init_attempted = true;
			if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return false;

			has_engine_counter = add_english_counter(L"\\GPU Engine(*)\\Utilization Percentage", engine_counter);
			has_dedicated_memory_counter = add_english_counter(L"\\GPU Adapter Memory(*)\\Dedicated Usage", dedicated_memory_counter);
			has_shared_memory_counter = add_english_counter(L"\\GPU Adapter Memory(*)\\Shared Usage", shared_memory_counter);
			if (!has_engine_counter and !has_dedicated_memory_counter and !has_shared_memory_counter) {
				PdhCloseQuery(query);
				query = nullptr;
				return false;
			}

			PdhCollectQueryData(query);
			initialized = true;
			return true;
		}

		auto shutdown() -> bool {
			const bool was_initialized = initialized;
			if (query != nullptr) PdhCloseQuery(query);
			query = nullptr;
			engine_counter = nullptr;
			dedicated_memory_counter = nullptr;
			shared_memory_counter = nullptr;
			has_engine_counter = false;
			has_dedicated_memory_counter = false;
			has_shared_memory_counter = false;
			initialized = false;
			init_attempted = false;
			return was_initialized;
		}

		auto adapter_index_for_instance(const string& instance) -> std::optional<size_t> {
			if (instance.empty()) return std::nullopt;
			for (size_t i = 0; i < dxgi_adapters.size(); ++i) {
				if (!dxgi_adapters[i].luid_key.empty() and instance.contains(dxgi_adapters[i].luid_key)) return i;
			}
			return std::nullopt;
		}

		auto read_double_items(PDH_HCOUNTER counter) -> vector<pdh_double_item> {
			DWORD bytes = 0;
			DWORD count = 0;
			PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
			if (status != pdh_more_data or bytes == 0 or count == 0) return {};
			vector<std::byte> buffer(bytes);
			auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
			status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items);
			if (status != ERROR_SUCCESS) return {};
			vector<pdh_double_item> result;
			result.reserve(count);
			for (DWORD i = 0; i < count; ++i) {
				if (items[i].szName == nullptr or not WindowsPdh::valid_counter_status(items[i].FmtValue.CStatus)) continue;
				result.push_back({to_lower_copy(wide_to_utf8(items[i].szName)), items[i].FmtValue.doubleValue});
			}
			return result;
		}

		auto read_large_items(PDH_HCOUNTER counter) -> vector<pdh_large_item> {
			DWORD bytes = 0;
			DWORD count = 0;
			PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, nullptr);
			if (status != pdh_more_data or bytes == 0 or count == 0) return {};
			vector<std::byte> buffer(bytes);
			auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
			status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, items);
			if (status != ERROR_SUCCESS) return {};
			vector<pdh_large_item> result;
			result.reserve(count);
			for (DWORD i = 0; i < count; ++i) {
				if (items[i].szName == nullptr or not WindowsPdh::valid_counter_status(items[i].FmtValue.CStatus)) continue;
				result.push_back({to_lower_copy(wide_to_utf8(items[i].szName)), static_cast<long long>(items[i].FmtValue.largeValue)});
			}
			return result;
		}

		auto collect() -> vector<adapter_sample> {
			vector<adapter_sample> samples(gpus.size());
			if (gpus.empty() or !init()) return samples;
			if (PdhCollectQueryData(query) != ERROR_SUCCESS) return samples;

			if (has_engine_counter) {
				for (const auto& item : read_double_items(engine_counter)) {

					const auto index = adapter_index_for_instance(item.name);
					if (!index or *index >= samples.size()) continue;
					const auto& instance = item.name;
					const long long value = clamp(static_cast<long long>(round(item.value)), 0ll, 100ll);
					if (instance.contains("engtype_3d") or instance.contains("engtype_compute")) {
						samples[*index].utilization = max(samples[*index].utilization, value);
					}
					else if (instance.contains("engtype_videoencode")) {
						samples[*index].encoder = max(samples[*index].encoder, value);
					}
					else if (instance.contains("engtype_videodecode")) {
						samples[*index].decoder = max(samples[*index].decoder, value);
					}
				}
			}

			if (has_dedicated_memory_counter) {
				for (const auto& item : read_large_items(dedicated_memory_counter)) {

					const auto index = adapter_index_for_instance(item.name);
					if (!index or *index >= samples.size()) continue;
					samples[*index].dedicated_memory = max(samples[*index].dedicated_memory, item.value);
				}
			}
			if (has_shared_memory_counter) {
				for (const auto& item : read_large_items(shared_memory_counter)) {
					const auto index = adapter_index_for_instance(item.name);
					if (!index or *index >= samples.size()) continue;
					samples[*index].shared_memory = max(samples[*index].shared_memory, item.value);
				}
			}
			return samples;
		}
	}

	bool has_nvml_device(const size_t gpu_index) {
		return rng::find(nvml_gpu_indices, gpu_index) != nvml_gpu_indices.end();
	}

	std::optional<size_t> dxgi_index_for_libre_sample(const WindowsSensors::gpu_sample& sample) {
		vector<size_t> matches;
		for (size_t i = 0; i < dxgi_adapters.size(); ++i) {
			if (dxgi_adapters[i].vendor_id == sample.vendor_id) matches.push_back(i);
		}
		if (matches.empty()) return std::nullopt;
		if (sample.device_index >= 0 and std::cmp_less(static_cast<size_t>(sample.device_index), matches.size())) return matches.at(sample.device_index);
		return matches.size() == 1 ? std::optional<size_t>{matches.front()} : std::nullopt;
	}

	void apply_libre_hardware_gpu_samples(const vector<PdhGpu::adapter_sample>* pdh_samples = nullptr) {
		const auto snapshot = read_libre_hardware_snapshot();
		if (!snapshot.available or snapshot.sensors.empty()) return;
		for (const auto& sample : WindowsSensors::aggregate_gpus(snapshot.sensors)) {
			const auto index = dxgi_index_for_libre_sample(sample);
			if (!index or *index >= gpus.size()) continue;
			auto& gpu = gpus[*index];
			const bool nvml_device = has_nvml_device(*index);
			const PdhGpu::adapter_sample* pdh = pdh_samples != nullptr and *index < pdh_samples->size() ? &pdh_samples->at(*index) : nullptr;
			const auto pdh_memory = pdh != nullptr
				? WindowsGpu::select_memory(dxgi_adapters[*index].dedicated_vram, dxgi_adapters[*index].shared_system_memory,
					pdh->dedicated_memory, pdh->shared_memory)
				: WindowsGpu::memory_sample{};

			if (sample.utilization and (!nvml_device or nvmlDeviceGetUtilizationRates == nullptr) and (pdh == nullptr or pdh->utilization < 0)) {
				gpu.supported_functions.gpu_utilization = true;
				gpu.gpu_percent.at("gpu-totals").push_back(clamp(*sample.utilization, 0ll, 100ll));
			}
			if (sample.memory_utilization and (!nvml_device or nvmlDeviceGetUtilizationRates == nullptr)
			and not pdh_memory.available) {
				gpu.supported_functions.mem_utilization = true;
				gpu.mem_utilization_percent.push_back(clamp(*sample.memory_utilization, 0ll, 100ll));
			}
			if (sample.temperature and (!nvml_device or nvmlDeviceGetTemperature == nullptr)) {
				if (!gpu.supported_functions.temp_info) gpu.temp.clear();
				gpu.supported_functions.temp_info = true;
				gpu.temp.push_back(*sample.temperature);
			}
			if (sample.power_mw and (!nvml_device or nvmlDeviceGetPowerUsage == nullptr)) {
				if (!gpu.supported_functions.pwr_usage) {
					gpu.pwr_max_usage = 0;
					gpu.gpu_percent.at("gpu-pwr-totals").clear();
				}
				gpu.supported_functions.pwr_usage = true;
				gpu.pwr_usage = *sample.power_mw;
				gpu.pwr_max_usage = max(gpu.pwr_max_usage, gpu.pwr_usage);
				gpu.gpu_percent.at("gpu-pwr-totals").push_back(gpu.pwr_max_usage > 0
					? clamp(static_cast<long long>(round(gpu.pwr_usage * 100.0 / gpu.pwr_max_usage)), 0ll, 100ll) : 0);
			}
			if (sample.gpu_clock_mhz and (!nvml_device or nvmlDeviceGetClockInfo == nullptr)) {
				gpu.supported_functions.gpu_clock = true;
				gpu.gpu_clock_speed = *sample.gpu_clock_mhz;
			}
			if (sample.memory_clock_mhz and (!nvml_device or nvmlDeviceGetClockInfo == nullptr)) {
				gpu.supported_functions.mem_clock = true;
				gpu.mem_clock_speed = *sample.memory_clock_mhz;
			}
		}
	}

	namespace Nvml {
		auto init() -> bool {
			if (nvml_initialized) return true;
			if (nvml_init_attempted) return false;
			nvml_init_attempted = true;

			Dxgi::init();
			if (!load_nvml_library()) return false;

			nvmlInit_v2 = load_nvml_proc<nvmlInit_v2_t>("nvmlInit_v2");
			nvmlShutdown = load_nvml_proc<nvmlShutdown_t>("nvmlShutdown");
			nvmlDeviceGetCount_v2 = load_nvml_proc<nvmlDeviceGetCount_v2_t>("nvmlDeviceGetCount_v2");
			nvmlDeviceGetHandleByIndex_v2 = load_nvml_proc<nvmlDeviceGetHandleByIndex_v2_t>("nvmlDeviceGetHandleByIndex_v2");
			nvmlDeviceGetName = load_nvml_proc<nvmlDeviceGetName_t>("nvmlDeviceGetName");
			nvmlDeviceGetUtilizationRates = load_nvml_proc<nvmlDeviceGetUtilizationRates_t>("nvmlDeviceGetUtilizationRates");
			nvmlDeviceGetMemoryInfo = load_nvml_proc<nvmlDeviceGetMemoryInfo_t>("nvmlDeviceGetMemoryInfo");
			nvmlDeviceGetPowerUsage = load_nvml_proc<nvmlDeviceGetPowerUsage_t>("nvmlDeviceGetPowerUsage");
			nvmlDeviceGetPowerManagementLimit = load_nvml_proc<nvmlDeviceGetPowerManagementLimit_t>("nvmlDeviceGetPowerManagementLimit");
			nvmlDeviceGetClockInfo = load_nvml_proc<nvmlDeviceGetClockInfo_t>("nvmlDeviceGetClockInfo");
			nvmlDeviceGetTemperature = load_nvml_proc<nvmlDeviceGetTemperature_t>("nvmlDeviceGetTemperature");
			nvmlDeviceGetEncoderUtilization = load_nvml_proc<nvmlDeviceGetEncoderUtilization_t>("nvmlDeviceGetEncoderUtilization");
			nvmlDeviceGetDecoderUtilization = load_nvml_proc<nvmlDeviceGetDecoderUtilization_t>("nvmlDeviceGetDecoderUtilization");

			if (nvmlInit_v2 == nullptr or nvmlDeviceGetCount_v2 == nullptr or nvmlDeviceGetHandleByIndex_v2 == nullptr) return false;
			if (nvmlInit_v2() != NVML_SUCCESS) return false;

			unsigned int device_count = 0;
			if (nvmlDeviceGetCount_v2(&device_count) != NVML_SUCCESS or device_count == 0) return false;

			nvml_devices.clear();
			nvml_gpu_indices.clear();
			gpu_pwr_total_max = 0;
			size_t next_nvidia_adapter = 0;
			for (unsigned int i = 0; i < device_count; ++i) {
				nvmlDevice_t device = nullptr;
				if (nvmlDeviceGetHandleByIndex_v2(i, &device) != NVML_SUCCESS or device == nullptr) continue;

				size_t gpu_index = gpus.size();
				while (next_nvidia_adapter < dxgi_adapters.size() and !is_nvidia_vendor(dxgi_adapters[next_nvidia_adapter].vendor_id)) ++next_nvidia_adapter;
				if (next_nvidia_adapter < dxgi_adapters.size()) {
					gpu_index = next_nvidia_adapter++;
				}
				else {
					gpus.emplace_back();
					gpu_names.emplace_back(fmt::format("NVIDIA GPU {}", gpus.size()));
				}

				nvml_devices.push_back(device);
				nvml_gpu_indices.push_back(gpu_index);
				auto& gpu = gpus[gpu_index];
				gpu.gpu_clock_speed = 0;
				gpu.pwr_usage = 0;
				gpu.pwr_state = 0;
				gpu.mem_clock_speed = 0;
				gpu.pcie_tx = 0;
				gpu.pcie_rx = 0;
				gpu.encoder_utilization = 0;
				gpu.decoder_utilization = 0;
				gpu.supported_functions.gpu_utilization = nvmlDeviceGetUtilizationRates != nullptr;
				gpu.supported_functions.mem_utilization = nvmlDeviceGetUtilizationRates != nullptr;
				gpu.supported_functions.gpu_clock = nvmlDeviceGetClockInfo != nullptr;
				gpu.supported_functions.mem_clock = nvmlDeviceGetClockInfo != nullptr;
				gpu.supported_functions.pwr_usage = nvmlDeviceGetPowerUsage != nullptr;
				gpu.supported_functions.pwr_state = false;
				gpu.supported_functions.temp_info = nvmlDeviceGetTemperature != nullptr;
				gpu.supported_functions.mem_total = nvmlDeviceGetMemoryInfo != nullptr or gpu.mem_total > 0;
				gpu.supported_functions.mem_used = nvmlDeviceGetMemoryInfo != nullptr;
				gpu.supported_functions.pcie_txrx = false;
				gpu.supported_functions.encoder_utilization = nvmlDeviceGetEncoderUtilization != nullptr;
				gpu.supported_functions.decoder_utilization = nvmlDeviceGetDecoderUtilization != nullptr;

				if (nvmlDeviceGetName != nullptr) {
					char name[128]{};
					if (nvmlDeviceGetName(device, name, sizeof(name)) == NVML_SUCCESS and name[0] != '\0') {
						gpu_names[gpu_index] = name;
					}
				}

				if (nvmlDeviceGetPowerManagementLimit != nullptr) {
					unsigned int max_power = 0;
					if (nvmlDeviceGetPowerManagementLimit(device, &max_power) == NVML_SUCCESS and max_power > 0) {
						gpu.pwr_max_usage = max_power;
					}
				}
				gpu_pwr_total_max += gpu.pwr_max_usage;
			}

			nvml_initialized = !nvml_devices.empty();
			count = static_cast<int>(gpus.size());
			return nvml_initialized;
		}

		auto shutdown() -> bool {
			const bool was_initialized = nvml_initialized;
			if (nvml_initialized and nvmlShutdown != nullptr) nvmlShutdown();
			nvml_initialized = false;
			nvml_init_attempted = false;
			nvml_devices.clear();
			nvml_gpu_indices.clear();
			if (nvml_library != nullptr) {
				FreeLibrary(nvml_library);
				nvml_library = nullptr;
			}
			return was_initialized;
		}
	}

	namespace Rsmi {
		auto shutdown() -> bool {
			return false;
		}
	}

	namespace Asysfs {
		auto shutdown() -> bool {
			return PdhGpu::shutdown();
		}
	}

	auto collect(bool no_update) -> vector<gpu_info>& {
		if (Runner::stopping or (no_update and not gpus.empty())) return gpus;
		Dxgi::init();
		if (!nvml_initialized and Config::getS("shown_gpus").contains("nvidia")) Nvml::init();

		for (size_t i = 0; i < nvml_devices.size() and i < nvml_gpu_indices.size(); ++i) {
			auto& gpu = gpus[nvml_gpu_indices[i]];
			const auto device = nvml_devices[i];

			if (nvmlDeviceGetUtilizationRates != nullptr) {
				nvmlUtilization_t util{};
				if (nvmlDeviceGetUtilizationRates(device, &util) == NVML_SUCCESS) {
					gpu.gpu_percent.at("gpu-totals").push_back(clamp(static_cast<long long>(util.gpu), 0ll, 100ll));
					gpu.mem_utilization_percent.push_back(clamp(static_cast<long long>(util.memory), 0ll, 100ll));
				}
			}
			if (gpu.gpu_percent.at("gpu-totals").empty()) gpu.gpu_percent.at("gpu-totals").push_back(0);
			if (gpu.mem_utilization_percent.empty()) gpu.mem_utilization_percent.push_back(0);

			if (nvmlDeviceGetMemoryInfo != nullptr) {
				nvmlMemory_t memory{};
				if (nvmlDeviceGetMemoryInfo(device, &memory) == NVML_SUCCESS) {
					gpu.mem_total = static_cast<long long>(memory.total);
					gpu.mem_used = static_cast<long long>(memory.used);
					gpu.mem_shared = false;
					if (memory.total > 0) {
						gpu.gpu_percent.at("gpu-vram-totals").push_back(clamp(static_cast<long long>(round(memory.used * 100.0 / memory.total)), 0ll, 100ll));
					}
				}
			}
			if (gpu.gpu_percent.at("gpu-vram-totals").empty()) gpu.gpu_percent.at("gpu-vram-totals").push_back(0);

			if (nvmlDeviceGetPowerUsage != nullptr) {
				unsigned int power = 0;
				if (nvmlDeviceGetPowerUsage(device, &power) == NVML_SUCCESS) gpu.pwr_usage = power;
			}
			if (gpu.pwr_max_usage > 0) {
				gpu.gpu_percent.at("gpu-pwr-totals").push_back(clamp(static_cast<long long>(round(gpu.pwr_usage * 100.0 / gpu.pwr_max_usage)), 0ll, 100ll));
			}
			if (gpu.gpu_percent.at("gpu-pwr-totals").empty()) gpu.gpu_percent.at("gpu-pwr-totals").push_back(0);

			if (nvmlDeviceGetClockInfo != nullptr) {
				unsigned int clock = 0;
				if (nvmlDeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &clock) == NVML_SUCCESS) gpu.gpu_clock_speed = clock;
				if (nvmlDeviceGetClockInfo(device, NVML_CLOCK_MEM, &clock) == NVML_SUCCESS) gpu.mem_clock_speed = clock;
			}
			if (nvmlDeviceGetTemperature != nullptr) {
				unsigned int temp = 0;
				if (nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) gpu.temp.push_back(temp);
			}
			if (nvmlDeviceGetEncoderUtilization != nullptr) {
				unsigned int util = 0, period = 0;
				if (nvmlDeviceGetEncoderUtilization(device, &util, &period) == NVML_SUCCESS) gpu.encoder_utilization = util;
			}
			if (nvmlDeviceGetDecoderUtilization != nullptr) {
				unsigned int util = 0, period = 0;
				if (nvmlDeviceGetDecoderUtilization(device, &util, &period) == NVML_SUCCESS) gpu.decoder_utilization = util;
			}
		}

		const auto pdh_samples = PdhGpu::collect();
		for (size_t i = 0; i < pdh_samples.size() and i < gpus.size(); ++i) {
			auto& gpu = gpus[i];
			const auto& sample = pdh_samples[i];
			if (sample.utilization >= 0 and (!has_nvml_device(i) or nvmlDeviceGetUtilizationRates == nullptr)) {
				gpu.supported_functions.gpu_utilization = true;
				gpu.gpu_percent.at("gpu-totals").push_back(sample.utilization);
			}
			if (sample.encoder >= 0 and !gpu.supported_functions.encoder_utilization) {
				gpu.supported_functions.encoder_utilization = true;
				gpu.encoder_utilization = sample.encoder;
			}
			if (sample.decoder >= 0 and !gpu.supported_functions.decoder_utilization) {
				gpu.supported_functions.decoder_utilization = true;
				gpu.decoder_utilization = sample.decoder;
			}
			const auto memory = WindowsGpu::select_memory(dxgi_adapters[i].dedicated_vram,
				dxgi_adapters[i].shared_system_memory, sample.dedicated_memory, sample.shared_memory);
			if (memory.available and !gpu.supported_functions.mem_used) {
				gpu.supported_functions.mem_used = true;
				gpu.mem_total = static_cast<long long>(memory.total);
				gpu.mem_used = static_cast<long long>(memory.used);
				gpu.mem_shared = memory.shared;
				if (gpu.mem_total > 0) {
					gpu.supported_functions.mem_utilization = true;
					const long long memory_percent = clamp(static_cast<long long>(round(gpu.mem_used * 100.0 / gpu.mem_total)), 0ll, 100ll);
					gpu.mem_utilization_percent.push_back(memory_percent);
					gpu.gpu_percent.at("gpu-vram-totals").push_back(memory_percent);
				}
			}
		}
		apply_libre_hardware_gpu_samples(&pdh_samples);
		long long avg = 0;

		long long avg_count = 0;
		long long mem_usage_total = 0;
		long long mem_total = 0;
		long long pwr_total = 0;
		gpu_pwr_total_max = 0;
		for (auto& gpu : gpus) {
			if (gpu.supported_functions.gpu_utilization) {
				avg += gpu.gpu_percent.at("gpu-totals").back();
				++avg_count;
			}
			if (gpu.supported_functions.mem_used) mem_usage_total += gpu.mem_used;
			if (gpu.supported_functions.mem_total) mem_total += gpu.mem_total;
			if (gpu.supported_functions.pwr_usage) {
				pwr_total += gpu.pwr_usage;
				gpu_pwr_total_max += gpu.pwr_max_usage;
			}

			if (width != 0) {
				while (std::cmp_greater(gpu.gpu_percent.at("gpu-totals").size(), width * 2)) gpu.gpu_percent.at("gpu-totals").pop_front();
				while (std::cmp_greater(gpu.mem_utilization_percent.size(), width)) gpu.mem_utilization_percent.pop_front();
				while (std::cmp_greater(gpu.gpu_percent.at("gpu-pwr-totals").size(), width)) gpu.gpu_percent.at("gpu-pwr-totals").pop_front();
				while (std::cmp_greater(gpu.temp.size(), 18)) gpu.temp.pop_front();
				while (std::cmp_greater(gpu.gpu_percent.at("gpu-vram-totals").size(), width / 2)) gpu.gpu_percent.at("gpu-vram-totals").pop_front();
			}
		}

		if (avg_count > 0) shared_gpu_percent.at("gpu-average").push_back(avg / avg_count);
		if (mem_total != 0) shared_gpu_percent.at("gpu-vram-total").push_back(static_cast<long long>(round(mem_usage_total * 100.0 / mem_total)));
		if (gpu_pwr_total_max != 0) shared_gpu_percent.at("gpu-pwr-total").push_back(clamp(static_cast<long long>(round(pwr_total * 100.0 / gpu_pwr_total_max)), 0ll, 100ll));

		if (width != 0) {
			while (std::cmp_greater(shared_gpu_percent.at("gpu-average").size(), width * 2)) shared_gpu_percent.at("gpu-average").pop_front();
			while (std::cmp_greater(shared_gpu_percent.at("gpu-pwr-total").size(), width * 2)) shared_gpu_percent.at("gpu-pwr-total").pop_front();
			while (std::cmp_greater(shared_gpu_percent.at("gpu-vram-total").size(), width * 2)) shared_gpu_percent.at("gpu-vram-total").pop_front();
		}

		count = static_cast<int>(gpus.size());
		return gpus;
	}
}
#endif
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
		coreCount = static_cast<long>(WindowsCpu::logical_processor_count());
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
		const auto libre_cpu = read_libre_cpu_sample();
		if (libre_cpu.package_temp) {
			Cpu::available_sensors.push_back("LibreHardwareMonitor");
			Cpu::got_sensors = true;
			Cpu::cpu_temp_only = libre_cpu.core_temps.empty();
		}
		if (read_windows_thermal_zone()) {
			Cpu::available_sensors.push_back("Windows Thermal Zone");
			Cpu::got_sensors = true;
			if (!libre_cpu.package_temp) Cpu::cpu_temp_only = true;
		}
		Cpu::cpuName = Cpu::trim_name(query_registry_string(HKEY_LOCAL_MACHINE,
			LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", L"ProcessorNameString"));
		if (Cpu::cpuName.empty()) Cpu::cpuName = "Windows CPU";
		Cpu::has_battery = true;
		Cpu::supports_watts = libre_cpu.watts.has_value() or read_windows_power_meter().has_value();
		Cpu::core_mapping = Cpu::get_core_mapping();
#if defined(GPU_SUPPORT)
		const auto shown_gpus = Config::getS("shown_gpus");
		if (shown_gpus.contains("nvidia") or shown_gpus.contains("amd") or shown_gpus.contains("intel")) {
			Gpu::Dxgi::init();
		}
		if (shown_gpus.contains("nvidia")) {
			Gpu::Nvml::init();
		}
		Gpu::apply_libre_hardware_gpu_samples();
		if (not Gpu::gpu_names.empty()) {
			for (auto const& [key, _] : Gpu::gpus[0].gpu_percent)
				Cpu::available_fields.push_back(key);
			for (auto const& [key, _] : Gpu::shared_gpu_percent)
				Cpu::available_fields.push_back(key);

			Gpu::count = static_cast<int>(Gpu::gpus.size());
			Gpu::gpu_b_height_offsets.resize(Gpu::gpus.size());
			for (size_t i = 0; i < Gpu::gpu_b_height_offsets.size(); ++i) {
				Gpu::gpu_b_height_offsets[i] = Gpu::gpus[i].supported_functions.gpu_utilization
					+ Gpu::gpus[i].supported_functions.pwr_usage
					+ (Gpu::gpus[i].supported_functions.encoder_utilization or Gpu::gpus[i].supported_functions.decoder_utilization)
					+ (Gpu::gpus[i].supported_functions.mem_total or Gpu::gpus[i].supported_functions.mem_used)
					* (1 + 2 * (Gpu::gpus[i].supported_functions.mem_total and Gpu::gpus[i].supported_functions.mem_used) + 2 * Gpu::gpus[i].supported_functions.mem_utilization);
			}
		}
#endif
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

	struct windows_processor_power_information {
		ULONG Number{};
		ULONG MaxMhz{};
		ULONG CurrentMhz{};
		ULONG MhzLimit{};
		ULONG MaxIdleState{};
		ULONG CurrentIdleState{};
	};

	string format_cpu_frequency_mhz(const double mhz) {
		if (mhz >= 1000.0) {
			string value = fmt::format("{:.1f}", mhz / 1000.0);
			value.resize(3);
			if (value.back() == '.') value.pop_back();
			return value + " GHz";
		}
		return fmt::format("{:.0f} MHz", mhz);
	}

	std::optional<DWORD> registry_cpu_mhz() {
		HKEY key{};
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", 0, KEY_READ, &key) != ERROR_SUCCESS) return std::nullopt;
		DWORD mhz = 0;
		DWORD bytes = sizeof(mhz);
		const auto result = RegQueryValueExW(key, L"~MHz", nullptr, nullptr, reinterpret_cast<BYTE*>(&mhz), &bytes);
		RegCloseKey(key);
		if (result != ERROR_SUCCESS or mhz == 0) return std::nullopt;
		return mhz;
	}

	auto get_cpuHz() -> string {
		vector<double> frequencies;
		vector<windows_processor_power_information> power_info(max<size_t>(1, static_cast<size_t>(Shared::coreCount)));
		if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power_info.data(), static_cast<ULONG>(power_info.size() * sizeof(windows_processor_power_information))) >= 0) {
			for (const auto& info : power_info) {
				if (info.CurrentMhz > 0) frequencies.push_back(static_cast<double>(info.CurrentMhz));
			}
		}

		if (!frequencies.empty()) {
			const auto& freq_mode = Config::getS("freq_mode");
			if (freq_mode == "range") {
				const auto [min_hz, max_hz] = std::minmax_element(frequencies.begin(), frequencies.end());
				return format_cpu_frequency_mhz(*min_hz) + " - " + format_cpu_frequency_mhz(*max_hz);
			}
			if (freq_mode == "lowest") return format_cpu_frequency_mhz(*std::min_element(frequencies.begin(), frequencies.end()));
			if (freq_mode == "highest") return format_cpu_frequency_mhz(*std::max_element(frequencies.begin(), frequencies.end()));
			if (freq_mode == "average") return format_cpu_frequency_mhz(std::accumulate(frequencies.begin(), frequencies.end(), 0.0) / frequencies.size());
			return format_cpu_frequency_mhz(frequencies.front());
		}

		const auto mhz = registry_cpu_mhz();
		return mhz ? format_cpu_frequency_mhz(*mhz) : string{};
	}

	std::optional<double> processor_queue_length() {
		static PDH_HQUERY query{};
		static PDH_HCOUNTER counter{};
		static bool initialized = false;
		static bool available = true;
		if (!available) return std::nullopt;
		if (!initialized) {
			if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
				available = false;
				return std::nullopt;
			}
			PDH_STATUS status = PdhAddEnglishCounterW(query, L"\\System\\Processor Queue Length", 0, &counter);
			if (status != ERROR_SUCCESS) status = PdhAddCounterW(query, L"\\System\\Processor Queue Length", 0, &counter);
			if (status != ERROR_SUCCESS) {
				PdhCloseQuery(query);
				query = nullptr;
				available = false;
				return std::nullopt;
			}
			initialized = true;
		}
		if (PdhCollectQueryData(query) != ERROR_SUCCESS) return std::nullopt;
		PDH_FMT_COUNTERVALUE value{};
		if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS
		or not WindowsPdh::valid_counter_status(value.CStatus)) return std::nullopt;
		return max(0.0, value.doubleValue);
	}

	std::optional<float> get_cpu_watts() {
		const auto libre_cpu = read_libre_cpu_sample();
		if (libre_cpu.watts) return static_cast<float>(*libre_cpu.watts);
		const auto sample = read_windows_power_meter();
		if (!sample) return std::nullopt;
		return static_cast<float>(sample->watts);
	}

	void clear_cpu_temperatures(cpu_info& cpu) {
		for (auto& temperatures : cpu.temp) temperatures.clear();
	}

	void push_libre_cpu_temperatures(cpu_info& cpu, const WindowsSensors::cpu_sample& sample) {
		while (std::cmp_less(cpu.temp.size(), Shared::coreCount + 1)) cpu.temp.emplace_back();
		push_limited(cpu.temp.at(0), *sample.package_temp, 20);
		cpu.temp_max = 100;
		cpu_temp_only = sample.core_temps.empty();
		if (sample.core_temps.empty()) return;

		for (int core = 0; core < Shared::coreCount; ++core) {
			const auto sensor = min<size_t>(sample.core_temps.size() - 1,
				static_cast<size_t>(core) * sample.core_temps.size() / max<long>(1, Shared::coreCount));
			push_limited(cpu.temp.at(core + 1), sample.core_temps.at(sensor).second, 20);
		}
	}

	bool update_cpu_temperatures(cpu_info& cpu) {
		static string active_source;
		const auto& configured = Config::getS("cpu_sensor");
		const bool automatic = configured.empty() or configured == "Auto";
		if (automatic or configured == "LibreHardwareMonitor") {
			const auto libre_cpu = read_libre_cpu_sample();
			if (libre_cpu.package_temp) {
				if (!v_contains(available_sensors, "LibreHardwareMonitor")) available_sensors.push_back("LibreHardwareMonitor");
				if (active_source != "LibreHardwareMonitor") {
					clear_cpu_temperatures(cpu);
					active_source = "LibreHardwareMonitor";
					redraw = true;
				}
				push_libre_cpu_temperatures(cpu, libre_cpu);
				return true;
			}
		}

		if (automatic or configured == "Windows Thermal Zone") {
			if (const auto temp = read_windows_thermal_zone()) {
				if (!v_contains(available_sensors, "Windows Thermal Zone")) available_sensors.push_back("Windows Thermal Zone");
				if (active_source != "Windows Thermal Zone") {
					clear_cpu_temperatures(cpu);
					active_source = "Windows Thermal Zone";
					redraw = true;
				}
				while (cpu.temp.empty()) cpu.temp.emplace_back();
				push_limited(cpu.temp.at(0), temp->current, 20);
				if (temp->critical > 0) cpu.temp_max = temp->critical;
				cpu_temp_only = true;
				return true;
			}
		}
		return false;
	}

	void update_load_average(cpu_info& cpu) {
		static bool initialized = false;
		static uint64_t last_ms = 0;
		const auto queue_length = processor_queue_length();
		const auto now = time_ms();
		if (!queue_length) return;
		if (!initialized or last_ms == 0) {
			cpu.load_avg = {*queue_length, *queue_length, *queue_length};
			initialized = true;
			last_ms = now;
			return;
		}
		const double elapsed = max<uint64_t>(1, now - last_ms) / 1000.0;
		last_ms = now;
		cpu.load_avg = WindowsCpu::smooth_queue_averages(cpu.load_avg, *queue_length, elapsed);
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
		if (Config::getB("show_cpu_watts")) {
			if (const auto watts = get_cpu_watts()) {
				cpu.usage_watts = *watts;
				supports_watts = true;
			} else {
				cpu.usage_watts = 0.0F;
				supports_watts = false;
			}
		}
		update_load_average(cpu);

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
		vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> spi(max<long>(1, Shared::coreCount));
		ULONG returned = 0;
		auto NtQuerySystemInformationPtr = ntdll_function<decltype(&NtQuerySystemInformation)>("NtQuerySystemInformation");
		const auto spi_bytes = static_cast<ULONG>(spi.size() * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
		if (NtQuerySystemInformationPtr and NtQuerySystemInformationPtr(SystemProcessorPerformanceInformation, spi.data(), spi_bytes, &returned) >= 0) {
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

		if (Config::getB("check_temp") and update_cpu_temperatures(cpu)) got_sensors = true;

		if (Config::getB("show_battery") and has_battery) current_bat = get_battery();
		cpu.active_cpus = std::views::iota(0, static_cast<int>(Shared::coreCount)) | std::ranges::to<std::vector<std::int32_t>>();
		return cpu;
	}
}

namespace Mem {
	bool has_swap = true;
	int disk_ios = 0;
	mem_info current_mem;
	std::unordered_map<string, int64_t> old_disk_query_time;
	std::unordered_map<string, int64_t> old_disk_idle_time;

	enum class disk_io_source { none, ioctl, pdh };

	struct disk_pdh_state {
		PDH_HQUERY query{};
		PDH_HCOUNTER read_counter{};
		PDH_HCOUNTER write_counter{};
		PDH_HCOUNTER idle_counter{};
		std::chrono::steady_clock::time_point sampled_at{};
	};

	std::unordered_map<string, disk_pdh_state> disk_pdh_states;
	std::unordered_map<string, disk_io_source> disk_io_sources;
	bool add_disk_pdh_counter(const PDH_HQUERY query, const std::wstring& path, PDH_HCOUNTER& counter) {
		if (PdhAddEnglishCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS) return true;
		return PdhAddCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS;
	}

	void close_disk_pdh(const string& name) {
		const auto it = disk_pdh_states.find(name);
		if (it == disk_pdh_states.end()) return;
		if (it->second.query != nullptr) PdhCloseQuery(it->second.query);
		disk_pdh_states.erase(it);
	}

	bool initialize_disk_pdh(const string& name, const wchar_t drive_letter) {
		disk_pdh_state state;
		if (PdhOpenQueryW(nullptr, 0, &state.query) != ERROR_SUCCESS) return false;

		const std::wstring instance = std::wstring(1, drive_letter) + L":";
		const std::wstring prefix = L"\\LogicalDisk(" + instance + L")\\";
		if (not add_disk_pdh_counter(state.query, prefix + L"Disk Read Bytes/sec", state.read_counter) or
			not add_disk_pdh_counter(state.query, prefix + L"Disk Write Bytes/sec", state.write_counter) or
			not add_disk_pdh_counter(state.query, prefix + L"% Idle Time", state.idle_counter) or
			PdhCollectQueryData(state.query) != ERROR_SUCCESS) {
			PdhCloseQuery(state.query);
			return false;
		}

		state.sampled_at = std::chrono::steady_clock::now();
		disk_pdh_states.insert_or_assign(name, state);
		return true;
	}

	auto read_disk_pdh_value(const PDH_HCOUNTER counter) -> std::optional<double> {
		PDH_FMT_COUNTERVALUE value{};
		if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) return std::nullopt;
		if (not WindowsPdh::valid_counter_status(value.CStatus)) return std::nullopt;
		return value.doubleValue;
	}

	bool update_disk_io_pdh(disk_info& disk, const wchar_t drive_letter) {
		if (not disk_pdh_states.contains(disk.name)) {
			if (not initialize_disk_pdh(disk.name, drive_letter)) return false;
			push_limited(disk.io_read, 0, max(1, Mem::width * 2));
			push_limited(disk.io_write, 0, max(1, Mem::width * 2));
			push_limited(disk.io_activity, 0, max(1, Mem::width * 2));
			return true;
		}

		auto& state = disk_pdh_states.at(disk.name);
		if (PdhCollectQueryData(state.query) != ERROR_SUCCESS) return false;
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration<double>(now - state.sampled_at).count();
		state.sampled_at = now;
		const auto read_rate = read_disk_pdh_value(state.read_counter);
		const auto write_rate = read_disk_pdh_value(state.write_counter);
		const auto idle_percent = read_disk_pdh_value(state.idle_counter);
		if (not read_rate or not write_rate or not idle_percent) return false;
		const auto delta = WindowsDisk::pdh_rates_to_delta(*read_rate, *write_rate, *idle_percent, elapsed);
		if (not delta) return false;

		push_limited(disk.io_read, delta->read_bytes, max(1, Mem::width * 2));
		push_limited(disk.io_write, delta->write_bytes, max(1, Mem::width * 2));
		push_limited(disk.io_activity, delta->activity, max(1, Mem::width * 2));
		return true;
	}

	bool read_disk_performance(const wchar_t drive_letter, DISK_PERFORMANCE& performance) {
		wchar_t volume_path[] = L"\\\\.\\C:";
		volume_path[4] = drive_letter;
		HANDLE volume = CreateFileW(volume_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (volume == INVALID_HANDLE_VALUE) return false;
		DWORD bytes_returned = 0;
		const bool ok = DeviceIoControl(volume, IOCTL_DISK_PERFORMANCE, nullptr, 0, &performance, sizeof(performance), &bytes_returned, nullptr);
		CloseHandle(volume);
		return ok and bytes_returned >= sizeof(DISK_PERFORMANCE);
	}

	long long delta_or_zero(const int64_t current, int64_t& previous) {
		const auto delta = previous > 0 and current >= previous ? current - previous : 0;
		previous = current;
		return static_cast<long long>(delta);
	}

	void update_disk_io(disk_info& disk, const DISK_PERFORMANCE& performance) {
		const auto read_bytes = static_cast<int64_t>(performance.BytesRead.QuadPart);
		const auto write_bytes = static_cast<int64_t>(performance.BytesWritten.QuadPart);
		const auto busy_time = static_cast<int64_t>(performance.ReadTime.QuadPart + performance.WriteTime.QuadPart);
		const auto idle_time = static_cast<int64_t>(performance.IdleTime.QuadPart);
		const auto query_time = static_cast<int64_t>(performance.QueryTime.QuadPart);

		push_limited(disk.io_read, delta_or_zero(read_bytes, disk.old_io.at(0)), max(1, Mem::width * 2));
		push_limited(disk.io_write, delta_or_zero(write_bytes, disk.old_io.at(1)), max(1, Mem::width * 2));

		auto& old_query_time = old_disk_query_time[disk.name];
		auto& old_idle_time = old_disk_idle_time[disk.name];
		const auto activity = WindowsDisk::activity_from_cumulative(
			query_time, idle_time, busy_time, old_query_time, old_idle_time, disk.old_io.at(2));
		push_limited(disk.io_activity, activity, max(1, Mem::width * 2));
	}

	void clear_disk_io(disk_info& disk) {
		disk.io_read.clear();
		disk.io_write.clear();
		disk.io_activity.clear();
		disk.old_io = {0, 0, 0};
		old_disk_query_time.erase(disk.name);
		old_disk_idle_time.erase(disk.name);
	}

	void set_disk_io_source(disk_info& disk, const disk_io_source source) {
		const auto current = disk_io_sources.contains(disk.name) ? disk_io_sources.at(disk.name) : disk_io_source::none;
		if (current == source) return;
		clear_disk_io(disk);
		if (source != disk_io_source::pdh) close_disk_pdh(disk.name);
		disk_io_sources[disk.name] = source;
		redraw = true;
	}

	uint64_t get_totalMem() { return Shared::totalMem; }

	auto collect(bool no_update) -> mem_info& {
		if (Runner::stopping or (no_update and not current_mem.percent.at("used").empty())) return current_mem;
		auto& mem = current_mem;
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (GlobalMemoryStatusEx(&status)) {
			Shared::totalMem = status.ullTotalPhys;
			auto physical = WindowsMemory::physical(status.ullTotalPhys, status.ullAvailPhys, status.ullAvailPhys, 0, 0);
			const auto fallback_commit_used = status.ullTotalPageFile > status.ullAvailPageFile ? status.ullTotalPageFile - status.ullAvailPageFile : 0;
			auto pagefiles = WindowsMemory::estimated_pagefiles(status.ullTotalPageFile, fallback_commit_used, status.ullTotalPhys);

			PERFORMANCE_INFORMATION perf{};
			perf.cb = sizeof(perf);
			if (GetPerformanceInfo(&perf, sizeof(perf))) {
				const auto page_size = static_cast<uint64_t>(perf.PageSize);
				const auto physical_total = static_cast<uint64_t>(perf.PhysicalTotal) * page_size;
				const auto physical_available = static_cast<uint64_t>(perf.PhysicalAvailable) * page_size;
				const auto system_cache = static_cast<uint64_t>(perf.SystemCache) * page_size;
				const auto memory_lists = read_windows_memory_lists(page_size);
				const auto commit_total = static_cast<uint64_t>(perf.CommitTotal) * page_size;
				const auto commit_limit = static_cast<uint64_t>(perf.CommitLimit) * page_size;
				if (physical_total > 0) Shared::totalMem = physical_total;
				physical = WindowsMemory::physical(Shared::totalMem, physical_available,
					memory_lists ? memory_lists->free : physical_available, system_cache,
					memory_lists ? memory_lists->cache : 0);
				pagefiles = WindowsMemory::estimated_pagefiles(commit_limit, commit_total, Shared::totalMem);
			}
			if (const auto actual_pagefiles = WindowsMemory::read_pagefiles()) pagefiles = *actual_pagefiles;
			mem.stats["used"] = physical.used;
			mem.stats["available"] = physical.available;
			mem.stats["cached"] = physical.cached;
			mem.stats["free"] = physical.free;
			mem.stats["swap_total"] = pagefiles.total;
			mem.stats["swap_used"] = pagefiles.used;
			mem.stats["swap_free"] = pagefiles.free;
			has_swap = mem.stats["swap_total"] > 0;
			for (const auto& name : mem_names) {
				const auto denom = max<uint64_t>(1, Shared::totalMem);
				push_limited(mem.percent[name], clamp((long long)round(mem.stats[name] * 100.0 / denom), 0ll, 100ll), max(1, Mem::width));
			}
			for (const auto& name : swap_names) {
				const auto denom = max<uint64_t>(1, mem.stats["swap_total"]);
				push_limited(mem.percent[name], clamp((long long)round(mem.stats[name] * 100.0 / denom), 0ll, 100ll), max(1, Mem::width));
			}
		}

		wchar_t drives[512]{};
		const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
		const bool only_physical = Config::getB("only_physical");
		vector<string> filter;
		bool filter_exclude = false;
		const auto& disks_filter = Config::getS("disks_filter");
		if (!disks_filter.empty()) {
			filter = ssplit(disks_filter);
			if (!filter.empty() and filter.front().starts_with("exclude=")) {
				filter_exclude = true;
				filter.front() = filter.front().substr(8);
				if (filter.front().empty()) filter.erase(filter.begin());
			}
		}

		std::unordered_set<string> found;
		disk_ios = 0;
		for (const wchar_t* drive = drives; drive < drives + length and *drive != L'\0'; drive += std::wcslen(drive) + 1) {
			const UINT type = GetDriveTypeW(drive);
			const auto kind = WindowsDisk::drive_kind_from_code(type);
			if (!WindowsDisk::allowed(kind, only_physical)) continue;
			ULARGE_INTEGER free_bytes{}, total_bytes{}, avail_bytes{};
			if (!GetDiskFreeSpaceExW(drive, &avail_bytes, &total_bytes, &free_bytes) or total_bytes.QuadPart == 0) continue;
			const string name = wide_to_utf8(drive);
			if (!WindowsDisk::filter_allows(name, filter, filter_exclude)) continue;
			found.insert(name);
			auto& disk = mem.disks[name];
			disk.name = name;
			disk.dev = name;
			disk.fstype = WindowsDisk::type_name(kind);
			disk.total = static_cast<int64_t>(total_bytes.QuadPart);
			disk.free = static_cast<int64_t>(free_bytes.QuadPart);
			disk.used = disk.total - disk.free;
			disk.used_percent = clamp((int)round(disk.used * 100.0 / max<int64_t>(1, disk.total)), 0, 100);
			disk.free_percent = 100 - disk.used_percent;
			if (WindowsDisk::supports_io(kind)) {
				DISK_PERFORMANCE performance{};
				if (read_disk_performance(drive[0], performance)) {
					set_disk_io_source(disk, disk_io_source::ioctl);
					update_disk_io(disk, performance);
					++disk_ios;
				}
				else {
					set_disk_io_source(disk, disk_io_source::pdh);
					if (update_disk_io_pdh(disk, drive[0])) ++disk_ios;
					else clear_disk_io(disk);
				}
			}
			else {
				clear_disk_io(disk);
				close_disk_pdh(disk.name);
				disk_io_sources.erase(disk.name);
			}
			if (!v_contains(mem.disks_order, name)) mem.disks_order.push_back(name);
		}
		auto order_end = rng::remove_if(mem.disks_order, [&](const string& name) { return !found.contains(name); });
		mem.disks_order.erase(order_end.begin(), order_end.end());
		for (auto it = mem.disks.begin(); it != mem.disks.end();) {
			if (!found.contains(it->first)) {
				old_disk_query_time.erase(it->first);
				old_disk_idle_time.erase(it->first);
				close_disk_pdh(it->first);
				disk_io_sources.erase(it->first);
				it = mem.disks.erase(it);
			}
			else ++it;
		}
		for (auto it = old_disk_query_time.begin(); it != old_disk_query_time.end();) {
			if (!found.contains(it->first)) it = old_disk_query_time.erase(it);
			else ++it;
		}
		for (auto it = old_disk_idle_time.begin(); it != old_disk_idle_time.end();) {
			if (!found.contains(it->first)) it = old_disk_idle_time.erase(it);
			else ++it;
		}
		return mem;
	}
}

namespace Net {
	vector<string> interfaces;
	string selected_iface;
	std::unordered_map<string, net_info> current_net;
	std::unordered_map<string, uint64_t> graph_max = {{"download", {}}, {"upload", {}}};
	std::unordered_map<string, string> iface_selectors;
	std::unordered_map<string, array<int, 2>> max_count = {{"download", {}}, {"upload", {}}};
	uint64_t timestamp{};
	bool rescale = true;

	auto collect(bool no_update) -> net_info& {
		static net_info empty_net;
		if (Runner::stopping) return empty_net;

		auto& config_iface = Config::getS("net_iface");
		const bool net_sync = Config::getB("net_sync");
		const bool net_auto = Config::getB("net_auto");
		const uint64_t new_timestamp = time_ms();
		const bool refresh = !no_update or current_net.empty();

		if (refresh) {
			MIB_IF_TABLE2* table = nullptr;
			if (GetIfTable2(&table) != NO_ERROR or table == nullptr) return empty_net;

			const auto metadata = read_adapter_metadata();
			iface_selectors.clear();
			std::unordered_set<string> found;
			std::unordered_map<DWORD, string> base_names;
			std::unordered_map<string, size_t> base_name_counts;
			for (ULONG i = 0; i < table->NumEntries; ++i) {
				const auto& row = table->Table[i];
				if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
				const auto meta_it = metadata.find(row.InterfaceIndex);
				const string friendly_name = meta_it != metadata.end() ? meta_it->second.name : string{};
				const string base_name = WindowsNetwork::base_adapter_name(friendly_name, wide_to_utf8(row.Alias),
					wide_to_utf8(row.Description), row.InterfaceIndex);
				base_names[row.InterfaceIndex] = base_name;
				++base_name_counts[WindowsNetwork::normalize_selector(base_name)];
			}
			const double elapsed = timestamp > 0 and new_timestamp > timestamp ? (new_timestamp - timestamp) / 1000.0 : 0.0;
			for (ULONG i = 0; i < table->NumEntries; ++i) {
				const auto& row = table->Table[i];
				if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
				const auto meta_it = metadata.find(row.InterfaceIndex);
				const string base_name = base_names.at(row.InterfaceIndex);
				const string name = WindowsNetwork::display_adapter_name(base_name, row.InterfaceIndex,
					base_name_counts.at(WindowsNetwork::normalize_selector(base_name)) > 1);
				found.insert(name);
				auto add_iface_selector = [&](const string& selector) {
					WindowsNetwork::register_selector(iface_selectors, selector, name);
				};
				add_iface_selector(name);
				add_iface_selector(fmt::format("if{}", row.InterfaceIndex));
				add_iface_selector(fmt::format("{}", row.InterfaceIndex));
				add_iface_selector(wide_to_utf8(row.Alias));
				add_iface_selector(wide_to_utf8(row.Description));
				if (meta_it != metadata.end()) {
					for (const auto& alias : meta_it->second.aliases) add_iface_selector(alias);
				}
				if (!v_contains(interfaces, name)) interfaces.push_back(name);

				auto& net = current_net[name];
				net.connected = meta_it != metadata.end() ? meta_it->second.connected : row.OperStatus == IfOperStatusUp;
				net.ipv4 = meta_it != metadata.end() ? meta_it->second.ipv4 : string{};
				net.ipv6 = meta_it != metadata.end() ? meta_it->second.ipv6 : string{};
				net.link_speed = meta_it != metadata.end() ? meta_it->second.link_speed : max<uint64_t>(row.ReceiveLinkSpeed, row.TransmitLinkSpeed) / 8;

				auto update_dir = [&](const string& dir, uint64_t total) {
					auto& stat = net.stat[dir];
					if (total < stat.last) {
						stat.rollover = 0;
						stat.offset = 0;
						stat.last = total;
						stat.speed = 0;
						stat.total = total;
						push_limited(net.bandwidth[dir], 0, max(1, Net::width));
						return;
					}
					const uint64_t absolute_total = total + stat.rollover;
					stat.speed = elapsed > 0.0 ? static_cast<uint64_t>(round((total - stat.last) / elapsed)) : 0;
					if (stat.speed > stat.top) stat.top = stat.speed;
					if (stat.offset > absolute_total) stat.offset = 0;
					stat.total = absolute_total - stat.offset;
					stat.last = total;
					push_limited(net.bandwidth[dir], static_cast<long long>(stat.speed), max(1, Net::width));

					if (net_auto and selected_iface == name) {
						if (net_sync and stat.speed < net.stat.at(dir == "download" ? "upload" : "download").speed) return;
						if (stat.speed > graph_max[dir]) {
							++max_count[dir][0];
							if (max_count[dir][1] > 0) --max_count[dir][1];
						}
						else if (graph_max[dir] > 10 << 10 and stat.speed < graph_max[dir] / 10) {
							++max_count[dir][1];
							if (max_count[dir][0] > 0) --max_count[dir][0];
						}
					}
				};
				update_dir("download", row.InOctets);
				update_dir("upload", row.OutOctets);
			}

			auto iface_end = rng::remove_if(interfaces, [&](const string& name) { return !found.contains(name); });
			interfaces.erase(iface_end.begin(), iface_end.end());
			for (auto it = current_net.begin(); it != current_net.end();) {
				if (!found.contains(it->first)) it = current_net.erase(it);
				else ++it;
			}
			FreeMibTable(table);
			timestamp = new_timestamp;
		}

		if (current_net.empty()) return empty_net;

		if (selected_iface.empty() or !v_contains(interfaces, selected_iface)) {
			max_count["download"] = {};
			max_count["upload"] = {};
			redraw = true;
			if (net_auto) rescale = true;
			if (!config_iface.empty()) {
				const auto resolved = WindowsNetwork::resolve_selector(iface_selectors, config_iface);
				if (!resolved.empty() and v_contains(interfaces, resolved)) selected_iface = resolved;
				else if (v_contains(interfaces, config_iface)) selected_iface = config_iface;
			}
			if (selected_iface.empty()) {
				auto sorted_interfaces = interfaces;
				rng::sort(sorted_interfaces, [&](const auto& a, const auto& b) {
					return current_net.at(a).stat["download"].total + current_net.at(a).stat["upload"].total >
						current_net.at(b).stat["download"].total + current_net.at(b).stat["upload"].total;
				});
				selected_iface.clear();
				for (const auto& iface : sorted_interfaces) {
					if (current_net.at(iface).connected) {
						selected_iface = iface;
						break;
					}
				}
				if (selected_iface.empty() and !sorted_interfaces.empty()) selected_iface = sorted_interfaces.front();
				else if (sorted_interfaces.empty()) return empty_net;
			}
		}

		if (net_auto and !selected_iface.empty() and current_net.contains(selected_iface)) {
			bool sync = false;
			for (const auto& dir : {"download", "upload"}) {
				for (const auto& sel : {0, 1}) {
					if (rescale or max_count[dir][sel] >= 5) {
						const auto& bandwidth = current_net.at(selected_iface).bandwidth.at(dir);
						const long long avg_speed = bandwidth.size() > 5
							? std::accumulate(bandwidth.rbegin(), bandwidth.rbegin() + 5, 0ll) / 5
							: static_cast<long long>(current_net.at(selected_iface).stat.at(dir).speed);
						graph_max[dir] = max<uint64_t>(static_cast<uint64_t>(avg_speed * (sel == 0 ? 1.3 : 3.0)), 10ULL << 10);
						max_count[dir] = {};
						redraw = true;
						if (net_sync) sync = true;
						break;
					}
				}
				if (sync) {
					const string other = string(dir) == "upload" ? "download" : "upload";
					graph_max[other] = graph_max[dir];
					max_count[other] = {};
					break;
				}
			}
		}
		rescale = false;
		return selected_iface.empty() ? empty_net : current_net.at(selected_iface);
	}
}

namespace Proc {
	vector<proc_info> current_procs;
	atomic<int> numpids{};
	detail_container detailed;
	int filter_found{};
	int collapse = -1, expand = -1, toggle_children = -1, collapse_all = -1;
	std::unordered_map<size_t, uint64_t> old_proc_cpu;
	std::unordered_map<size_t, array<uint64_t, 2>> process_io_totals;
	std::unordered_set<size_t> dead_procs;
	string current_sort, current_filter;
	bool current_rev = false, is_tree_mode = false;
	uint64_t old_system_cpu = 0;

	void collect_details(const size_t pid, vector<proc_info>& procs) {
		if (pid != detailed.last_pid) {
			detailed = {};
			detailed.last_pid = pid;
		}

		auto p_info = rng::find(procs, pid, &proc_info::pid);
		if (p_info == procs.end()) {
			if (detailed.last_pid == pid) detailed.status = "Dead";
			return;
		}

		detailed.entry = *p_info;
		if (not Config::getB("proc_per_core")) detailed.entry.cpu_p *= Shared::coreCount;
		detailed.cpu_percent.push_back(clamp((long long)round(detailed.entry.cpu_p), 0ll, 100ll));
		while (std::cmp_greater(detailed.cpu_percent.size(), Proc::width)) detailed.cpu_percent.pop_front();

		const auto uptime = system_uptime();
		if (detailed.entry.state == 'X') detailed.elapsed = sec_to_dhms(detailed.entry.death_time);
		else detailed.elapsed = uptime > detailed.entry.cpu_s ? sec_to_dhms(static_cast<uint64_t>(uptime - detailed.entry.cpu_s)) : "0s";
		if (detailed.elapsed.size() > 8) detailed.elapsed.resize(detailed.elapsed.size() - 3);

		auto parent = rng::find(procs, detailed.entry.ppid, &proc_info::pid);
		detailed.parent = parent != procs.end() ? parent->name : (detailed.entry.ppid > 0 ? fmt::format("pid {}", detailed.entry.ppid) : "");
		detailed.status = proc_states.contains(detailed.entry.state) ? proc_states.at(detailed.entry.state) : "Unknown";

		detailed.mem_bytes.push_back(static_cast<long long>(detailed.entry.mem));
		while (std::cmp_greater(detailed.mem_bytes.size(), Proc::width)) detailed.mem_bytes.pop_front();
		detailed.memory = floating_humanizer(detailed.entry.mem);
		if (detailed.first_mem == -1 or detailed.first_mem < detailed.mem_bytes.back() / 2 or detailed.first_mem > detailed.mem_bytes.back() * 4) {
			detailed.first_mem = min<uint64_t>(static_cast<uint64_t>(detailed.mem_bytes.back()) * 2, Mem::get_totalMem());
			redraw = true;
		}

		const auto io = process_io_totals.find(pid);
		if (io != process_io_totals.end()) {
			detailed.io_read = floating_humanizer(io->second[0]);
			detailed.io_write = floating_humanizer(io->second[1]);
		}
		else if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)); process != nullptr) {
			IO_COUNTERS io{};
			if (GetProcessIoCounters(process, &io)) {
				detailed.io_read = floating_humanizer(static_cast<uint64_t>(io.ReadTransferCount));
				detailed.io_write = floating_humanizer(static_cast<uint64_t>(io.WriteTransferCount));
			}
			CloseHandle(process);
		}
	}

	auto collect(bool no_update) -> vector<proc_info>& {
		if (Runner::stopping) return current_procs;

		const string sorting = Config::getS("proc_sorting").empty() ? "cpu lazy" : Config::getS("proc_sorting");
		const bool reverse = Config::getB("proc_reversed");
		const string filter = Config::getS("proc_filter");
		const bool tree = Config::getB("proc_tree");
		const bool show_detailed = Config::getB("show_detailed");
		const bool pause_proc_list = Config::getB("pause_proc_list");
		const bool per_core = Config::getB("proc_per_core");
		const size_t detailed_pid = static_cast<size_t>(Config::getI("detailed_pid"));

		bool should_filter = current_filter != filter;
		if (should_filter) current_filter = filter;
		const bool sorted_change = sorting != current_sort or reverse != current_rev or should_filter;
		const bool tree_mode_change = tree != is_tree_mode;
		if (sorted_change) {
			current_sort = sorting;
			current_rev = reverse;
		}
		if (tree_mode_change) is_tree_mode = tree;

		std::unordered_set<size_t> found;
		bool got_detailed = false;
		const bool refresh_processes = not no_update or current_procs.empty();

		if (refresh_processes) {
			const uint64_t system_now = current_system_cpu_time();
			const uint64_t system_delta = max<uint64_t>(1, system_now - old_system_cpu);
			old_system_cpu = system_now;
			const auto process_snapshots = query_process_snapshots();
			FILETIME now_filetime{};
			GetSystemTimeAsFileTime(&now_filetime);
			const uint64_t now_100ns = filetime_to_u64(now_filetime);
			const double uptime_now = system_uptime();

			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snapshot == INVALID_HANDLE_VALUE) return current_procs;
			PROCESSENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
				const size_t pid = entry.th32ProcessID;
				found.insert(pid);
				auto old = rng::find(current_procs, pid, &proc_info::pid);
				if (old == current_procs.end()) {
					if (pause_proc_list) continue;
					current_procs.push_back({pid});
					old = current_procs.end() - 1;
				}
				else if (dead_procs.contains(pid)) continue;

				auto& proc = *old;
				proc.name = wide_to_utf8(entry.szExeFile);
				proc.ppid = entry.th32ParentProcessID;
				proc.threads = entry.cntThreads;
				proc.state = 'S';
				proc.p_nice = 0;
				proc.prefix.clear();
				proc.depth = 0;
				proc.tree_index = 0;
				proc.mem = 0;
				bool got_snapshot_cpu = false;
				const auto snapshot = process_snapshots.find(pid);
				if (snapshot != process_snapshots.end()) {
					const auto& data = snapshot->second;
					if (proc.name.empty() and !data.name.empty()) proc.name = data.name;
					proc.ppid = data.ppid;
					proc.threads = data.threads;
					proc.state = data.state;
					proc.mem = data.working_set;
					const auto old_cpu = old_proc_cpu.find(pid);
					const uint64_t previous_cpu = old_cpu == old_proc_cpu.end() ? data.cpu_time_100ns : old_cpu->second;
					const auto cpu = WindowsProcess::calculate_cpu(data.cpu_time_100ns, previous_cpu, system_delta,
						data.create_time_100ns, now_100ns, uptime_now, per_core, Shared::coreCount);
					old_proc_cpu[pid] = data.cpu_time_100ns;
					proc.cpu_p = cpu.percent;
					proc.cpu_c = cpu.cumulative_percent;
					proc.cpu_t = cpu.cpu_milliseconds;
					proc.cpu_s = cpu.start_uptime_seconds;
					process_io_totals[pid] = {data.io_read, data.io_write};
					got_snapshot_cpu = true;
				}
				HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
				if (process != nullptr) {
					const DWORD priority_class = GetPriorityClass(process);
					proc.p_nice = priority_class != 0 ? WindowsProcess::priority_class_to_nice(priority_class) : 0;
					FILETIME create{}, exit{}, kernel{}, user{};
					if (!got_snapshot_cpu and GetProcessTimes(process, &create, &exit, &kernel, &user)) {
						const uint64_t cpu_now = filetime_to_u64(kernel) + filetime_to_u64(user);
						const auto old_cpu = old_proc_cpu.find(pid);
						const uint64_t previous_cpu = old_cpu == old_proc_cpu.end() ? cpu_now : old_cpu->second;
						const auto cpu = WindowsProcess::calculate_cpu(cpu_now, previous_cpu, system_delta,
							filetime_to_u64(create), now_100ns, uptime_now, per_core, Shared::coreCount);
						old_proc_cpu[pid] = cpu_now;
						proc.cpu_p = cpu.percent;
						proc.cpu_c = cpu.cumulative_percent;
						proc.cpu_t = cpu.cpu_milliseconds;
						proc.cpu_s = cpu.start_uptime_seconds;
					}
					if (proc.mem == 0) {
						PROCESS_MEMORY_COUNTERS_EX pmc{};
						if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) proc.mem = pmc.WorkingSetSize;
					}
					if (!process_io_totals.contains(pid)) {
						IO_COUNTERS io{};
						if (GetProcessIoCounters(process, &io)) process_io_totals[pid] = {io.ReadTransferCount, io.WriteTransferCount};
					}
					proc.cmd = process_command_line(process);
					if (proc.cmd.empty()) proc.cmd = process_path(process);
					proc.user = process_user(process);
					CloseHandle(process);
				}
				if (proc.cmd.empty()) proc.cmd = proc.name;
				if (proc.user.empty()) proc.user = "unknown";
				if (show_detailed and proc.pid == detailed_pid) got_detailed = true;
			}
			CloseHandle(snapshot);

			if (not pause_proc_list) {
				auto erase_end = rng::remove_if(current_procs, [&](const proc_info& p) { return !found.contains(p.pid); });
				current_procs.erase(erase_end.begin(), erase_end.end());
				if (!dead_procs.empty()) dead_procs.clear();
			}
			else {
				const bool keep_dead_proc_usage = Config::getB("keep_dead_proc_usage");
				for (auto& proc : current_procs) {
					if (!found.contains(proc.pid)) {
						if (proc.state != 'X') proc.death_time = static_cast<uint64_t>(system_uptime()) - proc.cpu_s;
						proc.state = 'X';
						dead_procs.emplace(proc.pid);
						if (!keep_dead_proc_usage) {
							proc.cpu_p = 0.0;
							proc.mem = 0;
						}
					}
				}
			}

			for (auto it = old_proc_cpu.begin(); it != old_proc_cpu.end();) {
				if (!found.contains(it->first)) it = old_proc_cpu.erase(it);
				else ++it;
			}
			for (auto it = process_io_totals.begin(); it != process_io_totals.end();) {
				if (!found.contains(it->first)) it = process_io_totals.erase(it);
				else ++it;
			}
		}
		else if (show_detailed and detailed_pid != detailed.last_pid) {
			got_detailed = rng::find(current_procs, detailed_pid, &proc_info::pid) != current_procs.end();
		}

		if (show_detailed and (got_detailed or rng::find(current_procs, detailed_pid, &proc_info::pid) != current_procs.end())) {
			collect_details(detailed_pid, current_procs);
		}
		else if (show_detailed and detailed.status != "Dead") {
			detailed.status = "Dead";
			redraw = true;
		}

		if (should_filter) {
			filter_found = 0;
			for (auto& proc : current_procs) {
				proc.prefix.clear();
				proc.depth = 0;
				if (not tree and not filter.empty()) {
					proc.filtered = !matches_filter(proc, filter);
					if (proc.filtered) filter_found++;
				}
				else {
					proc.filtered = false;
				}
			}
		}
		else if (!tree) {
			for (auto& proc : current_procs) {
				proc.prefix.clear();
				proc.depth = 0;
			}
		}

		if ((sorted_change or tree_mode_change) or (not no_update and not pause_proc_list)) {
			proc_sorter(current_procs, sorting, reverse, tree);
		}

		if (tree and !current_procs.empty() and (not no_update or should_filter or sorted_change or tree_mode_change or collapse != -1 or expand != -1 or toggle_children != -1 or collapse_all != -1)) {
			bool locate_selection = false;

			if (toggle_children != -1) {
				auto collapser = rng::find(current_procs, static_cast<size_t>(toggle_children), &proc_info::pid);
				if (collapser != current_procs.end()) {
					for (auto& proc : current_procs) {
						if (proc.ppid == collapser->pid) proc.collapsed = !proc.collapsed;
					}
					if (Config::getI("proc_selected") > 0) locate_selection = true;
				}
				toggle_children = -1;
			}

			if (const int find_pid = collapse != -1 ? collapse : expand; find_pid != -1) {
				auto collapser = rng::find(current_procs, static_cast<size_t>(find_pid), &proc_info::pid);
				if (collapser != current_procs.end()) {
					if (collapse == expand) collapser->collapsed = !collapser->collapsed;
					else if (collapse > -1) collapser->collapsed = true;
					else if (expand > -1) collapser->collapsed = false;
					if (Config::getI("proc_selected") > 0) locate_selection = true;
				}
				collapse = expand = -1;
			}

			if (collapse_all != -1) {
				toggle_tree_collapse(current_procs);
				collapse_all = -1;
				if (Config::getI("proc_selected") > 0) locate_selection = true;
			}

			if (should_filter or not filter.empty()) filter_found = 0;

			std::unordered_set<size_t> pid_set;
			for (const auto& proc : current_procs) pid_set.insert(proc.pid);
			if (!pause_proc_list) {
				for (auto& proc : current_procs) {
					if (!pid_set.contains(static_cast<size_t>(proc.ppid))) proc.ppid = 0;
				}
			}

			rng::stable_sort(current_procs, rng::less{}, &proc_info::ppid);
			_auto_collapse_oversized(current_procs, tree_mode_change);

			vector<tree_proc> tree_procs;
			tree_procs.reserve(current_procs.size());
			const auto root_ppid = current_procs.front().ppid;
			for (auto& proc : rng::equal_range(current_procs, root_ppid, rng::less{}, &proc_info::ppid)) {
				_tree_gen(proc, current_procs, tree_procs, 0, false, filter, false, no_update, should_filter);
			}

			int index = 0;
			tree_sort(tree_procs, sorting, reverse, (pause_proc_list and not (sorted_change or tree_mode_change)), index, current_procs.size());
			for (auto t = tree_procs.begin(); t != tree_procs.end(); ++t) {
				_collect_prefixes(*t, t == tree_procs.end() - 1);
			}
			rng::stable_sort(current_procs, rng::less{}, &proc_info::tree_index);

			if (locate_selection) {
				auto selected = rng::find(current_procs, static_cast<size_t>(Proc::selected_pid), &proc_info::pid);
				if (selected != current_procs.end()) {
					const int loc = static_cast<int>(selected->tree_index);
					if (Config::getI("proc_start") >= loc or Config::getI("proc_start") <= loc - Proc::select_max) {
						Config::set("proc_start", max(0, loc - 1));
					}
					Config::set("proc_selected", loc - Config::getI("proc_start") + 1);
				}
			}
		}

		numpids = static_cast<int>(current_procs.size()) - filter_found;
		return current_procs;
	}
}

namespace Tools {
	double system_uptime() {
		return GetTickCount64() / 1000.0;
	}
}
