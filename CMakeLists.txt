cmake_minimum_required(VERSION 3.16)
project(modbee_ota CXX)

# The host build: the platform-independent core, for the test suite and for
# host-side tools. Board targets build through Arduino or PlatformIO.

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The test suite builds by default only when this is the top-level project and
# the test directory is there; a project that adds this one as a subdirectory
# gets the library alone.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(MODBEE_OTA_TOP_LEVEL ON)
else()
  set(MODBEE_OTA_TOP_LEVEL OFF)
endif()
option(MODBEE_OTA_TESTS "Build the test suite" ${MODBEE_OTA_TOP_LEVEL})
option(MODBEE_OTA_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

add_library(modbee_ota STATIC
  src/OtaTypes.cpp
  src/OtaPlatform.cpp
  src/OtaHash.cpp
  src/OtaProtocol.cpp
  src/OtaManifest.cpp
  src/OtaUpdater.cpp
  src/OtaServer.cpp
  src/OtaAsyncTarget.cpp
  src/OtaAsyncVerifier.cpp
  src/staged/OtaStagedTarget.cpp
)
add_library(ModBeeOta::modbee_ota ALIAS modbee_ota)
target_include_directories(modbee_ota PUBLIC src)

if(MSVC)
  target_compile_options(modbee_ota PRIVATE /W4)
else()
  target_compile_options(modbee_ota PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MODBEE_OTA_SANITIZE AND NOT MSVC)
  target_compile_options(modbee_ota PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(modbee_ota PUBLIC -fsanitize=address,undefined)
endif()

if(MODBEE_OTA_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  enable_testing()
  add_subdirectory(test)
endif()
