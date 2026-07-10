// SPDX-License-Identifier: Apache-2.0

#include <limits>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "windows/hardware_sensors.hpp"

TEST(windows_hardware_sensors, prefers_package_and_orders_core_temperatures) {
	const std::vector<WindowsSensors::sensor> sensors = {
		{"/intelcpu/0/temperature/10", "/intelcpu/0", "CPU Core #10", "Temperature", 58.4},
		{"/intelcpu/0/temperature/0", "/intelcpu/0", "CPU Package", "Temperature", 61.6},
		{"/intelcpu/0/temperature/2", "/intelcpu/0", "CPU Core #2", "Temperature", 55.1},
		{"/intelcpu/0/temperature/1", "/intelcpu/0", "CPU Core #1", "Temperature", 54.8},
		{"/intelcpu/0/temperature/11", "/intelcpu/0", "Core Max", "Temperature", 70.0},
		{"/intelcpu/0/power/0", "/intelcpu/0", "CPU Package", "Power", 44.25},
		{"/intelcpu/0/power/1", "/intelcpu/0", "CPU Cores", "Power", 30.0},
	};

	const auto sample = WindowsSensors::aggregate_cpu(sensors, true);
	ASSERT_TRUE(sample.available);
	ASSERT_EQ(sample.package_temp, 62);
	ASSERT_EQ(sample.core_temps.size(), 3U);
	EXPECT_EQ(sample.core_temps.at(0).first, "CPU Core #1");
	EXPECT_EQ(sample.core_temps.at(1).first, "CPU Core #2");
	EXPECT_EQ(sample.core_temps.at(2).first, "CPU Core #10");
	ASSERT_TRUE(sample.watts.has_value());
	EXPECT_DOUBLE_EQ(*sample.watts, 44.25);
}

TEST(windows_hardware_sensors, recognizes_amd_parent_and_tctl_fallback) {
	const std::vector<WindowsSensors::sensor> sensors = {
		{"/amdcpu/0/temperature/0", "/amdcpu/0", "Core (Tctl/Tdie)", "Temperature", 67.2},
		{"/amdcpu/0/temperature/1", "/amdcpu/0", "CCD1", "Temperature", 63.0},
		{"/amdcpu/0/power/0", "/amdcpu/0", "CPU Total", "Power", 88.5},
	};

	const auto sample = WindowsSensors::aggregate_cpu(sensors, true);
	EXPECT_EQ(sample.package_temp, 67);
	EXPECT_TRUE(sample.core_temps.empty());
	ASSERT_TRUE(sample.watts.has_value());
	EXPECT_DOUBLE_EQ(*sample.watts, 88.5);
}

TEST(windows_hardware_sensors, rejects_unrelated_and_invalid_samples) {
	const std::vector<WindowsSensors::sensor> sensors = {
		{"/nvidiagpu/0/temperature/0", "/nvidiagpu/0", "GPU Core", "Temperature", 60.0},
		{"/intelcpu/0/temperature/0", "/intelcpu/0", "CPU Package", "Temperature", 140.0},
		{"/intelcpu/0/power/0", "/intelcpu/0", "CPU Package", "Power", std::numeric_limits<double>::quiet_NaN()},
	};

	const auto sample = WindowsSensors::aggregate_cpu(sensors, false);
	EXPECT_FALSE(sample.available);
	EXPECT_FALSE(sample.package_temp.has_value());
	EXPECT_FALSE(sample.watts.has_value());
	EXPECT_TRUE(sample.core_temps.empty());
}

TEST(windows_hardware_sensors, aggregates_amd_gpu_metrics_and_prefers_core_temperature) {
	const std::vector<WindowsSensors::sensor> sensors = {
		{"/atigpu/0/temperature/0", "/atigpu/0", "GPU Core", "Temperature", 54.2},
		{"/atigpu/0/temperature/1", "/atigpu/0", "GPU Hot Spot", "Temperature", 78.0},
		{"/atigpu/0/power/0", "/atigpu/0", "GPU Board Power", "Power", 112.5},
		{"/atigpu/0/clock/0", "/atigpu/0", "GPU Core", "Clock", 2450.0},
		{"/atigpu/0/clock/1", "/atigpu/0", "GPU Memory", "Clock", 2250.0},
		{"/atigpu/0/load/0", "/atigpu/0", "GPU Core", "Load", 73.4},
		{"/atigpu/0/load/1", "/atigpu/0", "GPU Memory", "Load", 42.2},
	};

	const auto samples = WindowsSensors::aggregate_gpus(sensors);
	ASSERT_EQ(samples.size(), 1U);
	const auto& sample = samples.front();
	EXPECT_EQ(sample.vendor_id, 0x1002U);
	EXPECT_EQ(sample.device_index, 0);
	EXPECT_EQ(sample.temperature, 54);
	EXPECT_EQ(sample.power_mw, 112500);
	EXPECT_EQ(sample.gpu_clock_mhz, 2450U);
	EXPECT_EQ(sample.memory_clock_mhz, 2250U);
	EXPECT_EQ(sample.utilization, 73);
	EXPECT_EQ(sample.memory_utilization, 42);
}

TEST(windows_hardware_sensors, groups_gpu_devices_by_vendor_and_index) {
	const std::vector<WindowsSensors::sensor> sensors = {
		{"/intelgpu/1/temperature/0", "/intelgpu/1", "GPU Core", "Temperature", 49.0},
		{"/nvidiagpu/0/load/0", "/nvidiagpu/0", "GPU Core", "Load", 81.0},
		{"/amdgpu/2/power/0", "/amdgpu/2", "GPU Total", "Power", 90.0},
		{"/storage/0/temperature/0", "/storage/0", "Drive", "Temperature", 40.0},
	};

	const auto samples = WindowsSensors::aggregate_gpus(sensors);
	ASSERT_EQ(samples.size(), 3U);
	EXPECT_EQ(samples.at(0).vendor_id, 0x1002U);
	EXPECT_EQ(samples.at(0).device_index, 2);
	EXPECT_EQ(samples.at(1).vendor_id, 0x10DEU);
	EXPECT_EQ(samples.at(1).device_index, 0);
	EXPECT_EQ(samples.at(2).vendor_id, 0x8086U);
	EXPECT_EQ(samples.at(2).device_index, 1);
}

TEST(windows_hardware_sensors, aggregates_intel_gpu_metrics) {
	const std::vector<WindowsSensors::sensor> sensors = {
		{"/intelgpu/0/temperature/0", "/intelgpu/0", "GPU Core", "Temperature", 51.4},
		{"/intelgpu/0/power/0", "/intelgpu/0", "GPU Package", "Power", 18.75},
		{"/intelgpu/0/clock/0", "/intelgpu/0", "GPU Graphics", "Clock", 1450.0},
		{"/intelgpu/0/clock/1", "/intelgpu/0", "GPU Memory", "Clock", 1600.0},
		{"/intelgpu/0/load/0", "/intelgpu/0", "GPU Core", "Load", 64.2},
		{"/intelgpu/0/load/1", "/intelgpu/0", "GPU Memory", "Load", 37.8},
	};

	const auto samples = WindowsSensors::aggregate_gpus(sensors);
	ASSERT_EQ(samples.size(), 1U);
	const auto& sample = samples.front();
	EXPECT_EQ(sample.vendor_id, 0x8086U);
	EXPECT_EQ(sample.device_index, 0);
	EXPECT_EQ(sample.temperature, 51);
	EXPECT_EQ(sample.power_mw, 18750);
	EXPECT_EQ(sample.gpu_clock_mhz, 1450U);
	EXPECT_EQ(sample.memory_clock_mhz, 1600U);
	EXPECT_EQ(sample.utilization, 64);
	EXPECT_EQ(sample.memory_utilization, 38);
}
#endif
