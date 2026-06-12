cmake_minimum_required(VERSION 3.20.1)
project(scifi-peripheral-example VERSION 0.1.0)

option(BUILD_FOR_ARM64 "Build for ARM64 architecture" OFF)
option(USE_LOCAL_SDK "Use locally built scifi-peripheral-sdk instead of system install" OFF)
set(LOCAL_SDK_PATH "scifi-peripheral-sdk" CACHE PATH "Path to local SDK build directory")

if(BUILD_FOR_ARM64 AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  message(STATUS "Cross compiling for ARM64")
  set(CMAKE_SYSTEM_NAME Linux)
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
  set(CMAKE_C_COMPILER /usr/bin/aarch64-linux-gnu-gcc)
  set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)
  set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
endif()

find_package(cppzmq REQUIRED)
find_package(Protobuf REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)

if(USE_LOCAL_SDK)
  message(STATUS "Using local SDK from: ${LOCAL_SDK_PATH}")
  set(SDK_LIB_PATH "${LOCAL_SDK_PATH}")
  set(SDK_LIB_NAME "scifi-peripheral-sdk")
  link_directories(${SDK_LIB_PATH})
  include_directories("${LOCAL_SDK_PATH}/include")
  set(SCIFI_PERIPHERAL_SDK_LIB ${SDK_LIB_NAME})
else()
  find_library(SCIFI_PERIPHERAL_SDK_LIB
    NAMES scifi-peripheral-sdk libscifi-peripheral-sdk.so.0
    REQUIRED
  )
endif()

message(STATUS "SCIFI_PERIPHERAL_SDK_LIB: ${SCIFI_PERIPHERAL_SDK_LIB}")

# Sources for the plugin .so.
#
# Note: the *only* file unique to "this plugin is loadable" is
# intan_rhd2132_plugin.cpp — it contains the SCIFI_PLUGIN_REGISTER(...) block.
# Everything else is the verbatim Intan implementation, copied from headstage.
set(PLUGIN_SOURCES
  src/driver/intan_rhd2132_peripheral.cpp
  src/driver/intan_rhd2132_registers.cpp
  src/driver/intan_rhd2132_plugin.cpp
)

add_library(scifi-intan-rhd2132 SHARED ${PLUGIN_SOURCES})

set_target_properties(scifi-intan-rhd2132 PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  # Plugin entry point is the only exported symbol; everything else stays hidden
  # to keep the plugin ABI surface minimal and avoid clashing with host symbols.
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  # Bare filename: synapsectl deploy places this directly into /usr/lib/scifi/plugins/.
  PREFIX ""
  OUTPUT_NAME "intan_rhd2132"
)

target_include_directories(scifi-intan-rhd2132 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/driver
)

target_compile_options(scifi-intan-rhd2132 PRIVATE -Wall -Wno-psabi)

target_link_libraries(scifi-intan-rhd2132
  PUBLIC
    cppzmq
    protobuf::libprotobuf
    ${SCIFI_PERIPHERAL_SDK_LIB}
  PRIVATE
    spdlog::spdlog
)

# Resolve transitive NEEDED libs from the SDK's deps (placed in /opt/scifi/lib
# by scifi-headstage-shared-libraries in the builder image, mirroring the
# synapse-example-app convention).
target_link_options(scifi-intan-rhd2132 PRIVATE -Wl,-rpath-link,/opt/scifi/lib)

# Note: this CMakeLists.txt only builds the .so. The .deb packaging happens in
# `synapsectl peripherals build`, which stages files (this .so + the SDK shared
# library extracted from the builder image's /usr/lib) into a temp dir and runs
# `fpm` with Section=synapse-peripherals. scifi-server's DeployApp gRPC handler
# gates installs by that Section, so it has to match exactly.
