cmake_minimum_required(VERSION 3.20)
project(OllvmPassTest CXX)

# Small test CMakeLists — builds a minimal Windows kernel driver stub
# so we can exercise the passes in CI (requires Windows WDK headers).

if(NOT DEFINED WDK_ROOT)
  # Try to locate the WDK automatically on typical install paths.
  file(GLOB WDK_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10/Include/*/km")
  list(SORT WDK_CANDIDATES)
  list(REVERSE WDK_CANDIDATES)
  list(GET WDK_CANDIDATES 0 WDK_ROOT)
  if(WDK_ROOT)
    get_filename_component(WDK_ROOT "${WDK_ROOT}/../.." ABSOLUTE)
    message(STATUS "Auto-detected WDK at ${WDK_ROOT}")
  else()
    message(WARNING "WDK not found — skipping kernel driver test build")
    return()
  endif()
endif()

# Get WDK version from the directory name.
file(GLOB WDK_VERSION_DIRS "${WDK_ROOT}/Include/*")
list(SORT WDK_VERSION_DIRS)
list(REVERSE WDK_VERSION_DIRS)
list(GET WDK_VERSION_DIRS 0 WDK_LATEST_DIR)
get_filename_component(WDK_VERSION "${WDK_LATEST_DIR}" NAME)
message(STATUS "WDK version: ${WDK_VERSION}")

set(WDK_KM_INCLUDE "${WDK_ROOT}/Include/${WDK_VERSION}/km")
set(WDK_SHARED_INCLUDE "${WDK_ROOT}/Include/${WDK_VERSION}/shared")
set(WDK_UCRT_INCLUDE "${WDK_ROOT}/Include/${WDK_VERSION}/ucrt")

add_executable(test_driver_stub test_driver.cpp)

target_include_directories(test_driver_stub PRIVATE
  ${WDK_KM_INCLUDE}
  ${WDK_SHARED_INCLUDE}
  ${WDK_UCRT_INCLUDE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_definitions(test_driver_stub PRIVATE
  _AMD64_
  _WIN64
  NTDDI_VERSION=0x0A000000
  _NTDDK_
  POOL_NX_OPTIN=1
)

target_compile_options(test_driver_stub PRIVATE
  /kernel
  /GS-
  /Gs4096
  /W3
)
