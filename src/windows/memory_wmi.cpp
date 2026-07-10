// SPDX-License-Identifier: Apache-2.0

#include "memory_wmi.hpp"

#include <climits>
#include <cstdint>
#include <mutex>

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>

namespace WindowsMemory {
namespace {

auto variant_to_i64(const VARIANT& value) -> std::optional<long long> {
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
		case VT_UI8: return value.ullVal <= static_cast<unsigned long long>(LLONG_MAX)
			? std::optional<long long>{static_cast<long long>(value.ullVal)} : std::nullopt;
		default: return std::nullopt;
	}
}

} // namespace

auto read_pagefiles() -> std::optional<pagefile_stats> {
	static std::mutex cache_mutex;
	static std::optional<pagefile_stats> cache;
	static ULONGLONG last_attempt = 0;
	std::scoped_lock lock(cache_mutex);
	const auto now = GetTickCount64();
	if (last_attempt != 0 and now - last_attempt < 5000) return cache;
	last_attempt = now;
	cache.reset();

	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool should_uninitialize = SUCCEEDED(init);
	if (FAILED(init) and init != RPC_E_CHANGED_MODE) return std::nullopt;

	const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
		RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
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

	CLSID locator_clsid{};
	IID locator_iid{};
	HRESULT hr = CLSIDFromString(const_cast<LPOLESTR>(L"{4590F811-1D3A-11D0-891F-00AA004B2E24}"), &locator_clsid);
	if (SUCCEEDED(hr)) hr = IIDFromString(const_cast<LPOLESTR>(L"{DC12A687-737F-11CF-884D-00AA004B2E24}"), &locator_iid);
	if (SUCCEEDED(hr)) hr = CoCreateInstance(locator_clsid, nullptr, CLSCTX_INPROC_SERVER, locator_iid, reinterpret_cast<void**>(&locator));
	if (FAILED(hr) or locator == nullptr) {
		cleanup();
		return std::nullopt;
	}

	BSTR namespace_path = SysAllocString(L"ROOT\\CIMV2");
	hr = locator->ConnectServer(namespace_path, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
	SysFreeString(namespace_path);
	if (FAILED(hr) or services == nullptr or FAILED(CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
			nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) {
		cleanup();
		return std::nullopt;
	}

	BSTR query_language = SysAllocString(L"WQL");
	BSTR query = SysAllocString(L"SELECT AllocatedBaseSize, CurrentUsage FROM Win32_PageFileUsage");
	hr = services->ExecQuery(query_language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
	SysFreeString(query);
	SysFreeString(query_language);
	if (FAILED(hr) or enumerator == nullptr) {
		cleanup();
		return std::nullopt;
	}

	uint64_t allocated_mib = 0;
	uint64_t used_mib = 0;
	size_t files = 0;
	IWbemClassObject* object{};
	ULONG returned = 0;
	while (enumerator->Next(static_cast<LONG>(-1), 1, &object, &returned) == WBEM_S_NO_ERROR and returned > 0 and object != nullptr) {
		VARIANT allocated{};
		VARIANT used{};
		long long allocated_value = -1;
		long long used_value = -1;
		if (SUCCEEDED(object->Get(L"AllocatedBaseSize", 0, &allocated, nullptr, nullptr))) {
			if (const auto value = variant_to_i64(allocated)) allocated_value = *value;
		}
		if (SUCCEEDED(object->Get(L"CurrentUsage", 0, &used, nullptr, nullptr))) {
			if (const auto value = variant_to_i64(used)) used_value = *value;
		}
		VariantClear(&allocated);
		VariantClear(&used);
		object->Release();
		object = nullptr;
		if (allocated_value < 0 or used_value < 0) continue;
		allocated_mib = saturating_add(allocated_mib, static_cast<uint64_t>(allocated_value));
		used_mib = saturating_add(used_mib, static_cast<uint64_t>(used_value));
		++files;
	}

	cache = pagefiles_from_mebibytes(allocated_mib, used_mib, files);
	cleanup();
	return cache;
}

} // namespace WindowsMemory
