cmake_minimum_required(VERSION 3.10)
project(xg170gc_sdk_capture VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
set(MVSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mvsdk" CACHE PATH "Vendor SDK root")
set(MVSDK_LIBRARY "${MVSDK_DIR}/lib/x64/libMVSDK.so")
if(NOT EXISTS "${MVSDK_DIR}/include/CameraApi.h" OR NOT EXISTS "${MVSDK_LIBRARY}")
  message(FATAL_ERROR "Missing Linux x86_64 vendor SDK. Set MVSDK_DIR to its root.")
endif()
add_library(xg170gc_capture STATIC src/camera.cpp)
target_include_directories(xg170gc_capture PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>"
)
target_include_directories(xg170gc_capture PRIVATE "${MVSDK_DIR}/include")
target_link_libraries(xg170gc_capture PUBLIC "${MVSDK_LIBRARY}" Threads::Threads)
target_compile_options(xg170gc_capture PRIVATE -Wall -Wextra -Wpedantic)
add_executable(xg170gc-capture apps/capture.cpp)
target_link_libraries(xg170gc-capture PRIVATE xg170gc_capture)
# Resolve the bundled library without copying it to /lib or /usr/lib.
set_target_properties(xg170gc-capture PROPERTIES
  BUILD_RPATH "${MVSDK_DIR}/lib/x64"
  INSTALL_RPATH "$ORIGIN/../vendor/mvsdk/lib/x64"
)
include(GNUInstallDirs)
install(TARGETS xg170gc_capture xg170gc-capture
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY vendor/mvsdk/ DESTINATION vendor/mvsdk USE_SOURCE_PERMISSIONS)
