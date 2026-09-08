# rfdet — CMake build (IDE/CI convenience).
#
# build.sh is the canonical build: it is where `import std;` (GCC 16,
# --compile-std-module) lives. CMake's `import std` support is still gated
# behind CMAKE_EXPERIMENTAL_CXX_IMPORT_STD with a per-release UUID, so this
# file uses the header fallback (RF_IMPORT_STD undefined), which is stable on
# CMake >= 3.28 + Ninja + GCC >= 14 and compiles the identical sources.
cmake_minimum_required(VERSION 3.28)
project(rfdet LANGUAGES CXX)

if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR "C++ modules require the Ninja generator: cmake -G Ninja ...")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

option(RF_TSAN "Build with ThreadSanitizer" OFF)
set(RF_MCPU "" CACHE STRING "-mcpu value (cortex-a76 for Pi 5, cortex-a78ae for Orin, native for dev)")

add_library(rf_flags INTERFACE)
target_compile_options(rf_flags INTERFACE
  -Wall -Wextra -Wpedantic -Werror=return-type
  -ffp-contract=off -fno-exceptions -fno-rtti)
target_link_options(rf_flags INTERFACE -pthread)
if(RF_MCPU)
  target_compile_options(rf_flags INTERFACE "-mcpu=${RF_MCPU}")
endif()
if(RF_TSAN)
  target_compile_options(rf_flags INTERFACE -fsanitize=thread -O1 -g)
  target_link_options(rf_flags INTERFACE -fsanitize=thread)
endif()

add_library(rf STATIC)
target_sources(rf
  PUBLIC FILE_SET CXX_MODULES FILES
    src/rf/core.cppm
    src/rf/memory.cppm
    src/rf/health.cppm
    src/rf/output.cppm
    src/rf/runtime.cppm
    src/rf/signal.cppm
    src/rf/simd.cppm
    src/rf/fft.cppm
    src/rf/dsp.cppm
    src/rf/acquire.cppm
    src/rf/pipeline.cppm)
target_link_libraries(rf PUBLIC rf_flags)

option(RF_WITH_SOAPY "Enable SoapySDR C receiver adapter" OFF)
if(RF_WITH_SOAPY)
  find_package(SoapySDR CONFIG REQUIRED)
  target_sources(rf PUBLIC FILE_SET CXX_MODULES FILES src/rf/soapy.cppm)
  target_compile_definitions(rf PUBLIC RF_WITH_SOAPY=1)
  target_link_libraries(rf PUBLIC SoapySDR)
endif()
add_executable(rfdet src/main.cpp)
target_link_libraries(rfdet PRIVATE rf)

enable_testing()
foreach(t IN ITEMS test_memory test_simd test_pipeline test_fft test_dsp test_acquire test_regression test_health test_runtime test_output test_dsp_boundaries)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE rf)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  foreach(check IN ITEMS replay runtime observability)
    add_test(NAME cli_${check}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_${check}.py" "$<TARGET_FILE:rfdet>")
    set_tests_properties(cli_${check} PROPERTIES TIMEOUT 30)
  endforeach()
  add_test(NAME cli_validation
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_cli.py" "$<TARGET_FILE:rfdet>")
  set_tests_properties(cli_validation PROPERTIES TIMEOUT 30)
  add_test(NAME soapy_stub
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_soapy_stub.py"
      --compiler "${CMAKE_CXX_COMPILER}" --out "${CMAKE_CURRENT_BINARY_DIR}/soapy-stub")
  set_tests_properties(soapy_stub PROPERTIES TIMEOUT 180)
endif()
if(RF_WITH_SOAPY)
  add_executable(test_soapy tests/test_soapy.cpp)
  target_link_libraries(test_soapy PRIVATE rf)
  add_test(NAME test_soapy COMMAND test_soapy)
endif()
