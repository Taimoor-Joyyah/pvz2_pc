# Install a portable application, without dependency development files or game data.
set(ACP_ANGLE_DIRECTORY "${PROJECT_BINARY_DIR}/angle" CACHE PATH "Packaged ANGLE DLL directory for installation")
option(ACP_VERIFY_ANGLE_INSTALL "Verify installed ANGLE against the tested build hashes" ON)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
if(MSVC AND NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  message(FATAL_ERROR "MSVC redistributable DLLs were not found; install the matching C++ desktop tools.")
endif()

configure_file("${PROJECT_SOURCE_DIR}/cmake/InstallRuntimeCheck.cmake.in"
               "${PROJECT_BINARY_DIR}/InstallRuntimeCheck.cmake" @ONLY)
install(SCRIPT "${PROJECT_BINARY_DIR}/InstallRuntimeCheck.cmake" COMPONENT Runtime)
install(TARGETS pvz2pc pvz2pc-game pvz2pc-launcher RUNTIME DESTINATION . COMPONENT Runtime)
install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION . COMPONENT Runtime)
install(FILES "${ACP_ANGLE_DIRECTORY}/libEGL.dll"
              "${ACP_ANGLE_DIRECTORY}/libGLESv2.dll"
              "${ACP_ANGLE_DIRECTORY}/d3dcompiler_47.dll"
        DESTINATION angle COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/cmake/angle-windows.lock.json"
        DESTINATION angle RENAME tested-build.json COMPONENT Runtime)
install(FILES "${ACP_ANGLE_DIRECTORY}/NOTICE.txt" "${ACP_ANGLE_DIRECTORY}/LICENSE"
        DESTINATION licenses/angle COMPONENT Runtime OPTIONAL)
install(FILES "${PROJECT_SOURCE_DIR}/README.md" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION . COMPONENT Runtime)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/" DESTINATION docs COMPONENT Runtime
        FILES_MATCHING PATTERN "*.md")
install(FILES "${json_SOURCE_DIR}/LICENSE.MIT" DESTINATION licenses/json COMPONENT Runtime)
install(FILES "${miniz_SOURCE_DIR}/LICENSE" DESTINATION licenses/miniz COMPONENT Runtime)
install(FILES "${boost_headers_SOURCE_DIR}/LICENSE_1_0.txt" DESTINATION licenses/boost COMPONENT Runtime)
install(FILES "${sdl3_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses/SDL3 COMPONENT Runtime)
install(FILES "${sdl3_SOURCE_DIR}/src/hidapi/LICENSE.txt"
              "${sdl3_SOURCE_DIR}/src/hidapi/LICENSE-bsd.txt"
              "${sdl3_SOURCE_DIR}/src/hidapi/LICENSE-orig.txt"
        DESTINATION licenses/SDL3/hidapi COMPONENT Runtime)
install(FILES "${dynarmic_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses/dynarmic COMPONENT Runtime)
foreach(dependency IN ITEMS mcl fmt oaknut robin-map zydis zycore)
  if(dependency STREQUAL "fmt")
    set(license_file LICENSE.rst)
  else()
    set(license_file LICENSE)
  endif()
  install(FILES "${dynarmic_SOURCE_DIR}/externals/${dependency}/${license_file}"
          DESTINATION "licenses/dynarmic/${dependency}" COMPONENT Runtime)
endforeach()
install(FILES "${webview2_SOURCE_DIR}/LICENSE.txt" "${webview2_SOURCE_DIR}/NOTICE.txt"
        DESTINATION licenses/WebView2 COMPONENT Runtime)
# User launcher.json, imported APK/OBB files, profiles and reports are never install inputs.
