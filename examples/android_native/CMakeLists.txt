cmake_minimum_required(VERSION 3.30)
project(acp_android_native_test LANGUAGES CXX)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build this fixture with the Android NDK for arm64-v8a.")
endif()
add_library(acp_native_test SHARED native_test.cpp)
set_target_properties(acp_native_test PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(acp_native_test PRIVATE -femulated-tls -fno-omit-frame-pointer -Wall -Wextra)
target_link_options(acp_native_test PRIVATE -Wl,--hash-style=sysv -Wl,--pack-dyn-relocs=none)
target_link_libraries(acp_native_test PRIVATE log OpenSLES)
add_custom_command(TARGET acp_native_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android/libc++_shared.so"
    "$<TARGET_FILE_DIR:acp_native_test>/libc++_shared.so")
