# SPDX-License-Identifier: Apache-2.0

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  FIND_PACKAGE_ARGS NAMES GTest
)
FetchContent_MakeAvailable(googletest)

add_library(libbtop_test)
target_include_directories(libbtop_test PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(libbtop_test libbtop GTest::gtest_main)

add_executable(btop_test cpu_names.cpp tools.cpp input_windows.cpp process_tree.cpp windows_cpu.cpp windows_disk.cpp windows_gpu.cpp windows_hardware_sensors.cpp windows_memory.cpp windows_network.cpp windows_pdh.cpp windows_process.cpp windows_process_actions.cpp)
target_link_libraries(btop_test libbtop_test)

include(GoogleTest)
gtest_discover_tests(btop_test)
